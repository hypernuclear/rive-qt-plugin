// rive_metal_renderer.mm — see header for the high-level rationale.
//
// Implementation notes:
//
// * Qt 6's scene graph on macOS runs on a Metal RHI by default. We pull
//   the MTLDevice + MTLCommandQueue out of the QRhi so rive's commands
//   share a queue with Qt's compositor — that gives us free GPU-side
//   serialization (commit order on a Metal queue is also execution
//   order), so rive's writes to the target texture are guaranteed to
//   be visible by the time Qt samples it for compositing.
//
// * We allocate the target texture ourselves (BGRA8Unorm, RenderTarget
//   + ShaderRead usage). It's adopted by Qt via QRhiTexture::createFrom
//   and surfaced to the scene graph via QQuickWindow::createTextureFrom-
//   RhiTexture. No copies — the QSGSimpleTextureNode in RiveView
//   references this same MTLTexture.
//
// * Image decoding (.riv files containing embedded raster art) is NOT
//   supported in this build — we pass --no-rive-decoders to keep the
//   PLS build small. A QImage-backed Factory wrapper is the obvious
//   follow-up.

#include "rive_metal_renderer.h"

#include <QLoggingCategory>
#include <QPointer>
#include <QQuickWindow>
#include <QRunnable>
#include <QSGRendererInterface>
#include <QSGTexture>
#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>

#import <Metal/Metal.h>

#include <rive/animation/state_machine_instance.hpp>
#include <rive/artboard.hpp>
#include <rive/file.hpp>
#include <rive/layout.hpp>
#include <rive/math/aabb.hpp>
#include <rive/renderer/metal/render_context_metal_impl.h>
#include <rive/renderer/render_context.hpp>
#include <rive/renderer/rive_renderer.hpp>

Q_LOGGING_CATEGORY(lcRiveMetal, "rive.metal")

namespace {

// QRhi resources can't be deleted synchronously from updatePaintNode:
// the scene-graph batch renderer caches QRhiTexture pointers across
// frames, so freeing one mid-sync leaves dangling pointers in batches
// that haven't rebuilt yet. Qt's idiom is to defer to AfterSwapStage —
// the compositor is guaranteed done with the texture and the render
// thread is between frames.
//
// We delete only the QSGTexture: empirically, the QSGPlainTexture
// returned by QQuickWindow::createTextureFromRhiTexture takes ownership
// of the QRhiTexture and frees it on destruction (despite the doc
// claim that "the caller is responsible"). Deleting both crashes —
// see git history for the bad-access stack traces.
class TextureCleanupJob final : public QRunnable
{
public:
    explicit TextureCleanupJob(QSGTexture* qsg) : m_qsg(qsg) {}

    void run() override { delete m_qsg; }

private:
    QSGTexture* m_qsg = nullptr;
};

rive::Fit toRiveFit(RiveMetalRenderer::FitMode f)
{
    switch (f)
    {
    case RiveMetalRenderer::FitMode::Contain:   return rive::Fit::contain;
    case RiveMetalRenderer::FitMode::Cover:     return rive::Fit::cover;
    case RiveMetalRenderer::FitMode::Fill:      return rive::Fit::fill;
    case RiveMetalRenderer::FitMode::None:      return rive::Fit::none;
    case RiveMetalRenderer::FitMode::ScaleDown: return rive::Fit::scaleDown;
    }
    return rive::Fit::contain;
}

} // namespace

struct RiveMetalRenderer::Impl
{
    // QPointer so we can detect window destruction without dangling.
    // Used to gate scheduleRenderJob — calling it on a freed window
    // would crash and leak the cleanup job.
    QPointer<QQuickWindow> window;
    QRhi* rhi = nullptr;
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;

    std::unique_ptr<rive::gpu::RenderContext> renderContext;
    rive::rcp<rive::gpu::RenderTargetMetal> renderTarget;

    // The texture rive renders into, wrapped for Qt's scene graph. We
    // own the MTLTexture; the QRhiTexture adopts it via createFrom; the
    // QSGTexture wraps the QRhiTexture. All three rotate together when
    // the item size changes.
    id<MTLTexture> targetTexture = nil;
    QRhiTexture* qrhiTexture = nullptr;
    QSGTexture* qsgTexture = nullptr;
    QSize textureSize;

    // Loaded animation. rive::File is rcp-managed; the artboard /
    // state-machine come out of the file as unique_ptrs.
    rive::rcp<rive::File> file;
    std::unique_ptr<rive::ArtboardInstance> artboard;
    std::unique_ptr<rive::StateMachineInstance> stateMachine;

    // Frame counter for FlushResources. Resource recycling inside rive
    // expects a monotonically-increasing currentFrameNumber and a
    // safeFrameNumber that lags it. We don't pipeline anything CPU-side
    // so safeFrameNumber = currentFrameNumber - 1 is a safe approximation
    // (rive holds onto buffers for a couple of frames internally).
    uint64_t currentFrameNumber = 0;

    // Defers texture deletion to the render thread's post-swap stage.
    // Synchronous deletion from updatePaintNode crashes because Qt's
    // batch renderer holds cached QRhiTexture* across frames — see the
    // TextureCleanupJob comment above.
    //
    // If the window has already been destroyed (app shutdown path),
    // we leak the resources: the QRhi tears down its own resource
    // pool, and the process is exiting anyway. Better than dereferencing
    // a freed QRhi.
    void scheduleTextureCleanup()
    {
        if (qsgTexture)
        {
            if (window)
            {
                window->scheduleRenderJob(new TextureCleanupJob(qsgTexture),
                                          QQuickWindow::AfterSwapStage);
            }
            qsgTexture = nullptr;
        }
        // qrhiTexture is owned (transitively) by qsgTexture — don't
        // double-free. Just null our local reference.
        qrhiTexture = nullptr;
        targetTexture = nil;
        textureSize = QSize();
    }
};

RiveMetalRenderer::RiveMetalRenderer() : m_impl(std::make_unique<Impl>()) {}

RiveMetalRenderer::~RiveMetalRenderer()
{
    // Drop rive state. The PLS RenderContext owns rive-side GPU
    // resources tied to the MTLDevice; releasing it before the device
    // disappears is correct.
    m_impl->stateMachine.reset();
    m_impl->artboard.reset();
    m_impl->file = nullptr;
    m_impl->renderTarget = nullptr;
    m_impl->renderContext.reset();

    // Textures: schedule deferred cleanup if the window is still alive
    // (normal teardown path, e.g. ~RiveView while the window persists).
    // If the window is gone (app shutdown, sceneGraphInvalidated path),
    // scheduleTextureCleanup leaks the wrappers — the QRhi has already
    // disposed of the underlying resources.
    m_impl->scheduleTextureCleanup();
}

bool RiveMetalRenderer::initialize(QQuickWindow* window, QString* errorOut)
{
    if (m_impl->window == window && m_impl->renderContext)
        return true;

    auto setError = [&](const QString& msg) {
        if (errorOut)
            *errorOut = msg;
        qCWarning(lcRiveMetal) << msg;
    };

    if (!window)
    {
        setError(QStringLiteral("RiveMetalRenderer: null window"));
        return false;
    }

    QSGRendererInterface* rif = window->rendererInterface();
    if (!rif || rif->graphicsApi() != QSGRendererInterface::Metal)
    {
        setError(QStringLiteral("RiveMetalRenderer: scene graph isn't on the Metal RHI "
                                "(set QQuickWindow::setGraphicsApi(QSGRendererInterface::Metal) "
                                "before constructing the QQuickWindow)"));
        return false;
    }

    auto* rhi = static_cast<QRhi*>(rif->getResource(window, QSGRendererInterface::RhiResource));
    if (!rhi)
    {
        setError(QStringLiteral("RiveMetalRenderer: QRhi resource unavailable"));
        return false;
    }

    auto* nh = static_cast<const QRhiMetalNativeHandles*>(rhi->nativeHandles());
    if (!nh || !nh->dev || !nh->cmdQueue)
    {
        setError(QStringLiteral("RiveMetalRenderer: QRhiMetalNativeHandles missing device/queue"));
        return false;
    }

    m_impl->window = window;
    m_impl->rhi = rhi;
    // Qt's QRhiMetalNativeHandles uses Q_FORWARD_DECLARE_OBJC_CLASS, so
    // dev/cmdQueue are already ObjC class pointers — no __bridge cast.
    m_impl->device = (id<MTLDevice>) nh->dev;
    m_impl->queue = (id<MTLCommandQueue>) nh->cmdQueue;

    rive::gpu::RenderContextMetalImpl::ContextOptions opts;
    m_impl->renderContext =
        rive::gpu::RenderContextMetalImpl::MakeContext(m_impl->device, opts);
    if (!m_impl->renderContext)
    {
        setError(QStringLiteral("RiveMetalRenderer: RenderContextMetalImpl::MakeContext failed"));
        return false;
    }

    qCInfo(lcRiveMetal) << "Initialized on device" << m_impl->device.name.UTF8String;
    return true;
}

bool RiveMetalRenderer::loadFile(const QByteArray& bytes, QString* errorOut)
{
    auto setError = [&](const QString& msg) {
        if (errorOut)
            *errorOut = msg;
    };

    unloadFile();

    if (!m_impl->renderContext)
    {
        setError(QStringLiteral("RiveMetalRenderer: not initialized"));
        return false;
    }
    if (bytes.isEmpty())
    {
        setError(QStringLiteral("RiveMetalRenderer: empty .riv data"));
        return false;
    }

    rive::ImportResult result = rive::ImportResult::malformed;
    rive::Factory* factory = m_impl->renderContext.get();
    auto file = rive::File::import(
        rive::Span<const uint8_t>(reinterpret_cast<const uint8_t*>(bytes.constData()),
                                  static_cast<size_t>(bytes.size())),
        factory,
        &result);
    if (result != rive::ImportResult::success || !file)
    {
        setError(result == rive::ImportResult::unsupportedVersion
                     ? QStringLiteral("Unsupported .riv version")
                     : QStringLiteral("Malformed .riv file"));
        return false;
    }

    m_impl->file = file;
    m_impl->artboard = m_impl->file->artboardDefault();
    if (!m_impl->artboard)
    {
        setError(QStringLiteral("No default artboard in .riv file"));
        m_impl->file = nullptr;
        return false;
    }

    // State machines are optional — many sample files animate solely
    // through the default timeline.
    m_impl->stateMachine = m_impl->artboard->defaultStateMachine();
    return true;
}

void RiveMetalRenderer::unloadFile()
{
    m_impl->stateMachine.reset();
    m_impl->artboard.reset();
    m_impl->file = nullptr;
}

bool RiveMetalRenderer::hasArtboard() const
{
    return m_impl && m_impl->artboard != nullptr;
}

bool RiveMetalRenderer::advance(float deltaSeconds)
{
    if (!m_impl->artboard)
        return false;

    if (m_impl->stateMachine)
        return m_impl->stateMachine->advanceAndApply(deltaSeconds);
    return m_impl->artboard->advance(deltaSeconds);
}

QSGTexture* RiveMetalRenderer::ensureTexture(const QSize& pixelSize)
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
        // Private storage — the texture only lives on the GPU; CPU
        // never reads it back, so private (vs managed/shared) avoids
        // the page-allocation cost of a CPU-mappable buffer.
        desc.storageMode = MTLStorageModePrivate;
        m_impl->targetTexture = [m_impl->device newTextureWithDescriptor:desc];
    }

    if (!m_impl->targetTexture)
    {
        qCWarning(lcRiveMetal) << "Failed to allocate MTLTexture at" << pixelSize;
        return nullptr;
    }

    auto* impl =
        m_impl->renderContext->static_impl_cast<rive::gpu::RenderContextMetalImpl>();
    m_impl->renderTarget = impl->makeRenderTarget(MTLPixelFormatBGRA8Unorm,
                                                  static_cast<uint32_t>(pixelSize.width()),
                                                  static_cast<uint32_t>(pixelSize.height()));

    QRhiTexture* qrhiTex = m_impl->rhi->newTexture(QRhiTexture::BGRA8, pixelSize, 1,
                                                   QRhiTexture::RenderTarget |
                                                       QRhiTexture::UsedAsTransferSource);
    if (!qrhiTex)
    {
        qCWarning(lcRiveMetal) << "QRhi::newTexture returned null";
        m_impl->targetTexture = nil;
        return nullptr;
    }
    QRhiTexture::NativeTexture native;
    native.object = reinterpret_cast<quint64>((__bridge void*)m_impl->targetTexture);
    native.layout = 0; // unused on Metal
    if (!qrhiTex->createFrom(native))
    {
        qCWarning(lcRiveMetal) << "QRhiTexture::createFrom failed";
        delete qrhiTex;
        m_impl->targetTexture = nil;
        return nullptr;
    }
    m_impl->qrhiTexture = qrhiTex;

    // OwnsTexture would tell QSGTexture to delete the QRhiTexture on
    // destruction — we pass the explicit flag so we don't have to track
    // the QRhiTexture manually after this point.
    m_impl->qsgTexture =
        m_impl->window->createTextureFromRhiTexture(qrhiTex);
    if (!m_impl->qsgTexture)
    {
        qCWarning(lcRiveMetal) << "createTextureFromRhiTexture failed";
        m_impl->qrhiTexture = nullptr;
        delete qrhiTex;
        m_impl->targetTexture = nil;
        return nullptr;
    }

    m_impl->textureSize = pixelSize;
    return m_impl->qsgTexture;
}

void RiveMetalRenderer::renderFrame(FitMode fit)
{
    if (!m_impl->artboard || !m_impl->renderContext || !m_impl->renderTarget ||
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
        frameDesc.clearColor = 0; // transparent — let Qt see through.
        m_impl->renderContext->beginFrame(frameDesc);

        rive::RiveRenderer renderer(m_impl->renderContext.get());
        renderer.save();

        const rive::AABB frame(0.0f, 0.0f, static_cast<float>(w),
                               static_cast<float>(h));
        renderer.align(toRiveFit(fit), rive::Alignment::center, frame,
                       m_impl->artboard->bounds());
        m_impl->artboard->draw(&renderer);
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

        // Drop the strong reference inside the render target so the
        // texture can be re-bound (or replaced) on the next frame
        // without holding a stale id<>.
        m_impl->renderTarget->setTargetTexture(nil);
    }
}
