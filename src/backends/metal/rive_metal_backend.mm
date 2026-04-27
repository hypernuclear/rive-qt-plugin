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
#include <rive/renderer/metal/render_context_metal_impl.h>
#include <rive/renderer/render_context.hpp>
#include <rive/renderer/rive_renderer.hpp>

Q_LOGGING_CATEGORY(lcRiveMetalBackend, "rive.metal.backend")

namespace {

rive::Fit toRiveFit(RiveRenderBackend::FitMode f)
{
    switch (f)
    {
    case RiveRenderBackend::FitMode::Contain:   return rive::Fit::contain;
    case RiveRenderBackend::FitMode::Cover:     return rive::Fit::cover;
    case RiveRenderBackend::FitMode::Fill:      return rive::Fit::fill;
    case RiveRenderBackend::FitMode::None:      return rive::Fit::none;
    case RiveRenderBackend::FitMode::ScaleDown: return rive::Fit::scaleDown;
    }
    return rive::Fit::contain;
}

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

} // namespace

struct RiveMetalBackend::Impl
{
    QPointer<QQuickWindow> window;
    QRhi* rhi = nullptr;
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;

    std::unique_ptr<rive::gpu::RenderContext> renderContext;
    std::unique_ptr<RiveQtFactory> factory;
    rive::rcp<rive::gpu::RenderTargetMetal> renderTarget;

    id<MTLTexture> targetTexture = nil;
    QRhiTexture* qrhiTexture = nullptr;
    QSGTexture* qsgTexture = nullptr;
    QSize textureSize;

    uint64_t currentFrameNumber = 0;

    void scheduleTextureCleanup()
    {
        if (qsgTexture)
        {
            if (window)
            {
                window->scheduleRenderJob(new TextureCleanupJob(qsgTexture),
                                          QQuickWindow::AfterSwapStage);
            }
            // If the window is gone, leak the wrappers — the QRhi has
            // already torn down its side and the process is shutting
            // down anyway.
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
    m_impl->factory.reset();
    m_impl->renderTarget = nullptr;
    m_impl->renderContext.reset();

    // RHI resources: schedule for deletion if the window is still live,
    // otherwise leak (see scheduleTextureCleanup comment).
    m_impl->scheduleTextureCleanup();
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

    // QImage-backed factory wrapper so embedded raster art in .riv
    // files decodes (rive_decoders is disabled in this build).
    m_impl->factory = std::make_unique<RiveQtFactory>(m_impl->renderContext.get());

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

    m_impl->qsgTexture = m_impl->window->createTextureFromRhiTexture(qrhiTex);
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

void RiveMetalBackend::renderFrame(rive::ArtboardInstance* artboard, FitMode fit)
{
    if (!artboard || !m_impl->renderContext || !m_impl->renderTarget ||
        !m_impl->targetTexture)
        return;

    @autoreleasepool
    {
        const uint32_t w = m_impl->renderTarget->width();
        const uint32_t h = m_impl->renderTarget->height();

        rive::gpu::RenderContext::FrameDescriptor frameDesc;
        frameDesc.renderTargetWidth = w;
        frameDesc.renderTargetHeight = h;
        frameDesc.loadAction = rive::gpu::LoadAction::clear;
        frameDesc.clearColor = 0;
        m_impl->renderContext->beginFrame(frameDesc);

        rive::RiveRenderer renderer(m_impl->renderContext.get());
        renderer.save();

        const rive::AABB frame(0.0f, 0.0f, static_cast<float>(w),
                               static_cast<float>(h));
        renderer.align(toRiveFit(fit), rive::Alignment::center, frame,
                       artboard->bounds());
        artboard->draw(&renderer);
        renderer.restore();

        m_impl->renderTarget->setTargetTexture(m_impl->targetTexture);

        id<MTLCommandBuffer> cb = [m_impl->queue commandBuffer];
        m_impl->currentFrameNumber += 1;
        rive::gpu::RenderContext::FlushResources flushRes;
        flushRes.renderTarget = m_impl->renderTarget.get();
        flushRes.externalCommandBuffer = (__bridge void*) cb;
        flushRes.currentFrameNumber = m_impl->currentFrameNumber;
        flushRes.safeFrameNumber =
            m_impl->currentFrameNumber > 0 ? m_impl->currentFrameNumber - 1 : 0;
        m_impl->renderContext->flush(flushRes);
        [cb commit];

        m_impl->renderTarget->setTargetTexture(nil);
    }
}

void RiveMetalBackend::abandonGraphicsResources()
{
    m_impl->factory.reset();
    m_impl->renderTarget = nullptr;
    m_impl->renderContext.reset();
    m_impl->qsgTexture = nullptr;
    m_impl->qrhiTexture = nullptr;
    m_impl->targetTexture = nil;
    m_impl->textureSize = QSize();
    m_impl->device = nil;
    m_impl->queue = nil;
    m_impl->rhi = nullptr;
    m_impl->window = nullptr;
}

// Static factory — at the moment there's only Metal, but the switch
// already exists so adding D3D/Vulkan later is a matter of adding
// branches, not restructuring callers.
std::unique_ptr<RiveRenderBackend>
RiveRenderBackend::create(QQuickWindow* window, QString* errorOut)
{
    if (!window)
    {
        if (errorOut)
            *errorOut = QStringLiteral("RiveRenderBackend::create: null window");
        return nullptr;
    }
    QSGRendererInterface* rif = window->rendererInterface();
    if (!rif)
    {
        if (errorOut)
            *errorOut = QStringLiteral(
                "RiveRenderBackend::create: window has no renderer interface");
        return nullptr;
    }
    switch (rif->graphicsApi())
    {
    case QSGRendererInterface::Metal:
        return std::make_unique<RiveMetalBackend>();
    default:
        if (errorOut)
            *errorOut = QStringLiteral(
                "RiveRenderBackend::create: no backend for this RHI graphics API");
        return nullptr;
    }
}
