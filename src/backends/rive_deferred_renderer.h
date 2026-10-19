#pragma once

#include "rive_render_backend.h"
#include "../rive/rive_qt_factory.h"

// Upstream uses "slots" as an ordinary identifier in its GPU census.
#pragma push_macro("slots")
#undef slots
#include <rive/renderer/texture.hpp>
#include <rive/renderer/cmd/deferred_host.hpp>
#pragma pop_macro("slots")

#include <functional>
#include <memory>

// One recorder per view. It borrows the real context (which D3D11 may share
// with sibling views), and must die before that context. All import resources
// and script calls use this session; platform backends only supply submission.
class RiveDeferredRenderer
{
public:
    using CanvasFlush = std::function<void(rive::gpu::RenderTarget*)>;
    using OreFrame = std::function<rive::ore::Context::FrameDescriptor()>;

    static std::unique_ptr<RiveDeferredRenderer> create(
        rive::gpu::RenderContext* context, QString* errorOut);
    rive::Factory* factory() { return &m_factory; }
    bool usesOre() const { return m_usedOre || m_host.replayOre(); }

    void render(rive::ArtboardInstance* artboard, const QSize& size,
                RiveRenderBackend::FitMode fit, RiveRenderBackend::AlignmentMode alignment,
                void* oreCommandBuffer, bool disableRasterOrdering,
                const CanvasFlush& flushCanvas, const std::function<void()>& present,
                const OreFrame& oreFrame = {});

private:
    RiveDeferredRenderer(rive::gpu::RenderContext* context, rive::ore::Context& ore);
    rive::gpu::RenderContext* m_context;
    rive::cmd::DeferredSession m_session;
    rive::cmd::DeferredInlineHost m_host;
    RiveQtFactory m_replayFactory;
    RiveQtFactory m_factory;
    bool m_usedOre = false;
};
