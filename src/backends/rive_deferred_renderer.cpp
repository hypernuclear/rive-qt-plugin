#include "rive_deferred_renderer.h"
#include "rive_render_backend_helpers.h"

#include <rive/artboard.hpp>

namespace {
class FrameSink final : public rive::cmd::HostFrameSink
{
public:
    FrameSink(rive::gpu::RenderContext* context, rive::Factory* factory,
              const QSize& size, void* commandBuffer, bool replayOre,
              bool disableRasterOrdering, const RiveDeferredRenderer::CanvasFlush& flush,
              const RiveDeferredRenderer::OreFrame& oreFrame) :
        HostFrameSink(true, 0, 0, replayOre), m_context(context), m_factory(factory),
        m_size(size), m_commandBuffer(commandBuffer), m_flushCanvas(flush), m_renderer(context),
        m_oreFrame(oreFrame)
    {
        m_disableRasterOrdering = disableRasterOrdering;
    }

    rive::Factory* factory() override { return m_factory; }
    rive::gpu::RenderContext* renderContext() override { return m_context; }
    void* oreCommandBuffer() override { return m_commandBuffer; }
    void beginOreFrame() override
    {
        if (auto* ore = oreContext()) {
            auto desc = m_oreFrame ? m_oreFrame() : rive::ore::Context::FrameDescriptor{};
            desc.externalCommandBuffer = m_commandBuffer;
            ore->beginFrame(desc);
        }
    }
    rive::Renderer* beginScreen(uint64_t, bool clear, uint32_t color) override
    {
        rive::gpu::RenderContext::FrameDescriptor desc;
        desc.renderTargetWidth = m_size.width();
        desc.renderTargetHeight = m_size.height();
        desc.loadAction = clear ? rive::gpu::LoadAction::clear
                                : rive::gpu::LoadAction::preserveRenderTarget;
        desc.clearColor = color;
        desc.disableRasterOrdering = m_disableRasterOrdering;
        m_context->beginFrame(desc);
        return &m_renderer;
    }

    void endCanvasContent() override
    {
        if (!m_flushCanvas) {
            // Metal needs independently submitted canvas flushes: its buffer
            // ring can block before a large multi-canvas frame is finished.
            HostFrameSink::endCanvasContent();
            return;
        }
        if (m_activeCanvas) {
            m_flushCanvas(m_activeCanvas->renderTarget());
            m_activeCanvas = nullptr;
        }
    }

private:
    rive::gpu::RenderContext* m_context;
    rive::Factory* m_factory;
    QSize m_size;
    void* m_commandBuffer;
    const RiveDeferredRenderer::CanvasFlush& m_flushCanvas;
    rive::RiveRenderer m_renderer;
    const RiveDeferredRenderer::OreFrame& m_oreFrame;
};
}

std::unique_ptr<RiveDeferredRenderer> RiveDeferredRenderer::create(
    rive::gpu::RenderContext* context, QString* errorOut)
{
    auto* ore = context ? context->ore() : nullptr;
    if (!ore) {
        if (errorOut)
            *errorOut = QStringLiteral("Rive GPU canvas context unavailable for this backend");
        return nullptr;
    }
    return std::unique_ptr<RiveDeferredRenderer>(new RiveDeferredRenderer(context, *ore));
}

RiveDeferredRenderer::RiveDeferredRenderer(rive::gpu::RenderContext* context,
                                         rive::ore::Context& ore) :
    m_context(context), m_session(rive::ore::ReplayCaps::from(ore)),
    m_replayFactory(context), m_factory(context, &m_session)
{
    m_session.bindRenderContext(context);
    m_host.bindSession(&m_session);
}

void RiveDeferredRenderer::render(rive::ArtboardInstance* artboard, const QSize& size,
                                  RiveRenderBackend::FitMode fit,
                                  RiveRenderBackend::AlignmentMode alignment,
                                  void* oreCommandBuffer, bool disableRasterOrdering,
                                  const CanvasFlush& flushCanvas,
                                  const std::function<void()>& present,
                                  const OreFrame& oreFrame)
{
    // Import and advance may already have recorded one-shot script work.
    // Preserve it until replay, then reset both streams together.
    m_host.beginRecord(true, 0);
    auto* renderer = m_host.screenRenderer();
    renderer->save();
    renderer->align(rive_qt::toRiveFit(fit), rive_qt::toRiveAlignment(alignment),
                    rive::AABB(0, 0, size.width(), size.height()), artboard->bounds());
    artboard->draw(renderer);
    renderer->restore();
    m_usedOre |= m_host.replayOre();
    FrameSink sink(m_context, &m_replayFactory, size, oreCommandBuffer,
                    m_host.replayOre(), disableRasterOrdering, flushCanvas,
                    oreFrame);
    m_host.replayInline(sink, present);
}
