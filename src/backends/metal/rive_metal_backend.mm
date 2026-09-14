// Metal backend impl. Graphics-only — it doesn't know about rive::File
// or StateMachineInstance; those live in RiveView / the domain wrappers.
// See rive_render_backend.h for the contract comments.
//
// Notes:
//
// * We pull MTLDevice + MTLCommandQueue from Qt's QRhi so rive shares
//   the compositor's queue. GPU-side ordering on a shared Metal queue
//   naturally serializes our writes before the compositor reads.
//
// * The QSGPlainTexture returned by createTextureFromRhiTexture
//   transitively owns the QRhiTexture — empirically deleting both
//   crashes. We only delete the QSGTexture; QRhi-side cleanup cascades.
//   Deferred to AfterSwapStage via scheduleRenderJob so in-flight batch
//   renderer state doesn't see a freed pointer.

#include "rive_metal_backend.h"

#include "../../rive/rive_qt_factory.h"

#include <QLoggingCategory>
#include <QPointer>
#include <QQuickWindow>
#include <QRunnable>
#include <QSGRendererInterface>
#include <QSGTexture>
#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>

#import <Metal/Metal.h>

#include <rive/artboard.hpp>
#include <rive/layout.hpp>
#include <rive/math/aabb.hpp>
// Complete rive::gpu::Texture before the impl header: MSVC instantiates
// rcp<Texture>::~rcp at RenderContextImpl::platformDecodeImageTexture's
// by-value return declaration and needs the full type (Clang doesn't).
#include <rive/renderer/texture.hpp>
#include <rive/renderer/metal/render_context_metal_impl.h>
#include <rive/renderer/render_context.hpp>
#include <rive/renderer/rive_renderer.hpp>
// Rive's census helper uses "slots" as a variable name.
#pragma push_macro("slots")
#undef slots
#include <rive/renderer/cmd/deferred_host.hpp>
#pragma pop_macro("slots")

#include <atomic>

#include "../rive_render_backend_helpers.h"

Q_LOGGING_CATEGORY(lcRiveMetalBackend, "rive.metal.backend")

namespace {

// Scheduled at AfterSwapStage so the compositor is guaranteed done with
// the QSGTexture before we delete it. Deleting the QSGPlainTexture
// also releases the QRhiTexture it wraps.
class TextureCleanupJob final : public QRunnable
{
public:
    explicit TextureCleanupJob(QSGTexture* qsg) : m_qsg(qsg) {}
    void run() override { delete m_qsg; }

private:
    QSGTexture* m_qsg = nullptr;
};

// Replay canvas passes and the final 2D draw on Qt's Metal queue. Rive owns
// recording/replay order; this sink supplies the screen target and command
// buffer used by both Ore (scripted GPU work) and the screen flush.
class MetalFrameSink final : public rive::cmd::HostFrameSink
{
public:
    MetalFrameSink(rive::gpu::RenderContext* context, rive::Factory* factory,
                   const QSize& size, id<MTLCommandBuffer> buffer, bool replayOre) :
        HostFrameSink(true, 0, 0, replayOre), m_context(context),
        m_factory(factory), m_size(size), m_buffer(buffer), m_renderer(context)
    {}

    rive::Factory* factory() override { return m_factory; }
    rive::gpu::RenderContext* renderContext() override { return m_context; }
    void* oreCommandBuffer() override { return (__bridge void*)m_buffer; }
    rive::Renderer* beginScreen(uint64_t, bool clear, uint32_t color) override
    {
        rive::gpu::RenderContext::FrameDescriptor desc;
        desc.renderTargetWidth = m_size.width();
        desc.renderTargetHeight = m_size.height();
        desc.loadAction = clear ? rive::gpu::LoadAction::clear
                                : rive::gpu::LoadAction::preserveRenderTarget;
        desc.clearColor = color;
        m_context->beginFrame(desc);
        return &m_renderer;
    }

private:
    rive::gpu::RenderContext* m_context;
    rive::Factory* m_factory;
    QSize m_size;
    id<MTLCommandBuffer> m_buffer;
    rive::RiveRenderer m_renderer;
};

} // namespace

struct RiveMetalBackend::Impl
{
    QPointer<QQuickWindow> window;
    QRhi* rhi = nullptr;
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;

    std::unique_ptr<rive::gpu::RenderContext> renderContext;
    std::unique_ptr<rive::cmd::DeferredSession> session;
    std::unique_ptr<rive::cmd::DeferredInlineHost> host;
    std::unique_ptr<RiveQtFactory> replayFactory;
    std::unique_ptr<RiveQtFactory> factory;
    rive::rcp<rive::gpu::RenderTargetMetal> renderTarget;

    id<MTLTexture> targetTexture = nil;
    QRhiTexture* qrhiTexture = nullptr;
    QSGTexture* qsgTexture = nullptr;
    QSize textureSize;

    uint64_t currentFrameNumber = 0;
    std::shared_ptr<std::atomic<uint64_t>> completedFrame =
        std::make_shared<std::atomic<uint64_t>>(0);
    id<MTLCommandBuffer> lastCommandBuffer = nil;

    void releaseRenderingState()
    {
        // A completion can arrive after the QObject/window disappears; its
        // counter is independently owned. Drain before releasing replay's
        // resident resources and their context, only on teardown.
        [lastCommandBuffer waitUntilCompleted];
        lastCommandBuffer = nil;
        factory.reset();
        replayFactory.reset();
        host.reset();
        session.reset();
        renderTarget = nullptr;
        renderContext.reset();
    }

    // sync=true for shutdown paths (~RiveMetalBackend,
    // abandonGraphicsResources): delete inline. AfterSwapStage won't
    // fire after sceneGraphInvalidated, so a deferred deletion would
    // leak the QRhiTexture and trip Qt's "QRhi going down with N
    // unreleased resources" warning during QRhi destruction.
    void scheduleTextureCleanup(bool sync = false)
    {
        if (qsgTexture)
        {
            if (sync || !window)
            {
                delete qsgTexture;
            }
            else
            {
                window->scheduleRenderJob(new TextureCleanupJob(qsgTexture),
                                          QQuickWindow::AfterSwapStage);
            }
            qsgTexture = nullptr;
        }
        // QRhiTexture is transitively owned by QSGPlainTexture.
        qrhiTexture = nullptr;
        targetTexture = nil;
        textureSize = QSize();
    }
};

RiveMetalBackend::RiveMetalBackend() : m_impl(std::make_unique<Impl>()) {}

RiveMetalBackend::~RiveMetalBackend()
{
    // Drop rive state before the device disappears. Factory borrows
    // the RenderContext so it goes first.
    m_impl->releaseRenderingState();

    m_impl->scheduleTextureCleanup(/*sync=*/true);
}

bool RiveMetalBackend::initialize(QQuickWindow* window, QString* errorOut)
{
    if (m_impl->window == window && m_impl->renderContext)
        return true;

    auto setError = [&](const QString& msg) {
        if (errorOut)
            *errorOut = msg;
        qCWarning(lcRiveMetalBackend) << msg;
    };

    if (!window)
    {
        setError(QStringLiteral("RiveMetalBackend: null window"));
        return false;
    }

    QSGRendererInterface* rif = window->rendererInterface();
    if (!rif || rif->graphicsApi() != QSGRendererInterface::Metal)
    {
        setError(QStringLiteral(
            "RiveMetalBackend: scene graph isn't on the Metal RHI "
            "(set QQuickWindow::setGraphicsApi(QSGRendererInterface::Metal) "
            "before constructing the QQuickWindow)"));
        return false;
    }

    auto* rhi = static_cast<QRhi*>(rif->getResource(window, QSGRendererInterface::RhiResource));
    if (!rhi)
    {
        setError(QStringLiteral("RiveMetalBackend: QRhi resource unavailable"));
        return false;
    }

    auto* nh = static_cast<const QRhiMetalNativeHandles*>(rhi->nativeHandles());
    if (!nh || !nh->dev || !nh->cmdQueue)
    {
        setError(QStringLiteral(
            "RiveMetalBackend: QRhiMetalNativeHandles missing device/queue"));
        return false;
    }

    m_impl->window = window;
    m_impl->rhi = rhi;
    m_impl->device = (id<MTLDevice>) nh->dev;
    m_impl->queue = (id<MTLCommandQueue>) nh->cmdQueue;

    rive::gpu::RenderContextMetalImpl::ContextOptions opts;
    m_impl->renderContext =
        rive::gpu::RenderContextMetalImpl::MakeContext(m_impl->device, opts);
    if (!m_impl->renderContext)
    {
        setError(QStringLiteral(
            "RiveMetalBackend: RenderContextMetalImpl::MakeContext failed"));
        return false;
    }

    auto* context = m_impl->renderContext.get();
    context->static_impl_cast<rive::gpu::RenderContextMetalImpl>()->setCommandQueue(m_impl->queue);
    auto* ore = context->ore();
    if (!ore)
    {
        setError(QStringLiteral("RiveMetalBackend: GPU canvas context unavailable"));
        m_impl->releaseRenderingState();
        return false;
    }
    m_impl->session = std::make_unique<rive::cmd::DeferredSession>(
        rive::ore::ReplayCaps::from(*ore));
    m_impl->session->bindRenderContext(context);
    m_impl->host = std::make_unique<rive::cmd::DeferredInlineHost>();
    m_impl->host->bindSession(m_impl->session.get());
    // Paths, paints, buffers and GPU script calls record through the same
    // session. QImage uploads remain on the real device; the session retains
    // those images through its foreign-image registry for replay.
    m_impl->replayFactory = std::make_unique<RiveQtFactory>(context);
    m_impl->factory = std::make_unique<RiveQtFactory>(context, m_impl->session.get());

    qCInfo(lcRiveMetalBackend) << "Initialized on device" << m_impl->device.name.UTF8String;
    return true;
}

bool RiveMetalBackend::isInitialized() const
{
    return m_impl->renderContext != nullptr;
}

rive::Factory* RiveMetalBackend::factory() const
{
    return m_impl->factory.get();
}

QSGTexture* RiveMetalBackend::ensureTexture(const QSize& pixelSize)
{
    if (!m_impl->renderContext || !m_impl->window)
        return nullptr;
    if (pixelSize.isEmpty())
        return nullptr;
    if (m_impl->textureSize == pixelSize && m_impl->qsgTexture)
        return m_impl->qsgTexture;

    m_impl->scheduleTextureCleanup();

    @autoreleasepool
    {
        MTLTextureDescriptor* desc = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                         width:static_cast<NSUInteger>(pixelSize.width())
                                        height:static_cast<NSUInteger>(pixelSize.height())
                                     mipmapped:NO];
        desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        desc.storageMode = MTLStorageModePrivate;
        m_impl->targetTexture = [m_impl->device newTextureWithDescriptor:desc];
    }

    if (!m_impl->targetTexture)
    {
        qCWarning(lcRiveMetalBackend) << "Failed to allocate MTLTexture at" << pixelSize;
        return nullptr;
    }

    auto* impl =
        m_impl->renderContext->static_impl_cast<rive::gpu::RenderContextMetalImpl>();
    m_impl->renderTarget = impl->makeRenderTarget(
        MTLPixelFormatBGRA8Unorm,
        static_cast<uint32_t>(pixelSize.width()),
        static_cast<uint32_t>(pixelSize.height()));

    QRhiTexture* qrhiTex = m_impl->rhi->newTexture(
        QRhiTexture::BGRA8, pixelSize, 1,
        QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource);
    if (!qrhiTex)
    {
        qCWarning(lcRiveMetalBackend) << "QRhi::newTexture returned null";
        m_impl->targetTexture = nil;
        return nullptr;
    }
    QRhiTexture::NativeTexture native;
    native.object = reinterpret_cast<quint64>((__bridge void*)m_impl->targetTexture);
    native.layout = 0;
    if (!qrhiTex->createFrom(native))
    {
        qCWarning(lcRiveMetalBackend) << "QRhiTexture::createFrom failed";
        delete qrhiTex;
        m_impl->targetTexture = nil;
        return nullptr;
    }
    m_impl->qrhiTexture = qrhiTex;

    m_impl->qsgTexture = m_impl->window->createTextureFromRhiTexture(
        qrhiTex,
        // Rive clears to transparent and artboards may not cover the
        // item; without the alpha flag the scene graph composites the
        // texture as opaque and transparent pixels render black.
        QQuickWindow::TextureHasAlphaChannel);
    if (!m_impl->qsgTexture)
    {
        qCWarning(lcRiveMetalBackend) << "createTextureFromRhiTexture failed";
        m_impl->qrhiTexture = nullptr;
        delete qrhiTex;
        m_impl->targetTexture = nil;
        return nullptr;
    }

    m_impl->textureSize = pixelSize;
    return m_impl->qsgTexture;
}

void RiveMetalBackend::renderFrame(rive::ArtboardInstance* artboard, FitMode fit,
                                   AlignmentMode alignment)
{
    if (!artboard || !m_impl->renderContext || !m_impl->renderTarget ||
        !m_impl->targetTexture)
        return;

    @autoreleasepool
    {
        const uint32_t w = m_impl->renderTarget->width();
        const uint32_t h = m_impl->renderTarget->height();

        // Import and advance may already have recorded script work. Keep it
        // until this frame has replayed, including one-shot resource creates.
        m_impl->host->beginRecord(true, 0);
        auto* renderer = m_impl->host->screenRenderer();
        renderer->save();

        const rive::AABB frame(0.0f, 0.0f, static_cast<float>(w),
                               static_cast<float>(h));
        renderer->align(rive_qt::toRiveFit(fit),
                       rive_qt::toRiveAlignment(alignment),
                       frame, artboard->bounds());
        artboard->draw(renderer);
        renderer->restore();

        m_impl->renderTarget->setTargetTexture(m_impl->targetTexture);

        id<MTLCommandBuffer> cb = [m_impl->queue commandBuffer];
        MetalFrameSink sink(m_impl->renderContext.get(), m_impl->replayFactory.get(),
                            QSize(w, h), cb, m_impl->host->replayOre());
        m_impl->host->replayInline(sink, [&] {
            const uint64_t frameNumber = ++m_impl->currentFrameNumber;
            rive::gpu::RenderContext::FlushResources flushRes;
            flushRes.renderTarget = m_impl->renderTarget.get();
            flushRes.externalCommandBuffer = (__bridge void*)cb;
            flushRes.currentFrameNumber = frameNumber;
            flushRes.safeFrameNumber = m_impl->completedFrame->load(std::memory_order_acquire);
            m_impl->renderContext->flush(flushRes);
            auto completedFrame = m_impl->completedFrame;
            [cb addCompletedHandler:^(id<MTLCommandBuffer>) {
                uint64_t previous = completedFrame->load(std::memory_order_relaxed);
                while (previous < frameNumber
                       && !completedFrame->compare_exchange_weak(previous, frameNumber,
                           std::memory_order_release, std::memory_order_relaxed)) {}
            }];
            m_impl->lastCommandBuffer = cb;
            [cb commit];
        });

        m_impl->renderTarget->setTargetTexture(nil);
    }
}

void RiveMetalBackend::abandonGraphicsResources()
{
    // sceneGraphInvalidated fires before QRhi tears down — delete the
    // QSGTexture inline so its QRhiTexture (and the underlying
    // MTLTexture wrapper) is released before QRhi destruction walks
    // its tracker. Skipping this leaks the QRhiTexture and trips Qt's
    // "QRhi going down with N unreleased resources" warning.
    m_impl->releaseRenderingState();
    m_impl->scheduleTextureCleanup(/*sync=*/true);
    m_impl->device = nil;
    m_impl->queue = nil;
    m_impl->rhi = nullptr;
    m_impl->window = nullptr;
}
