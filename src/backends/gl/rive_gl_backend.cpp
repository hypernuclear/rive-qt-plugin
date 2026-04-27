// OpenGL backend impl. Graphics-only — like Metal/D3D11, doesn't know
// about rive::File / StateMachineInstance; those live in RiveView and
// the domain wrappers. See rive_render_backend.h for the contract.
//
// Notes:
//
// * Rive's GL backend assumes the calling thread has a current GL
//   context AND that GLAD's function-pointer table has been populated.
//   Qt's QRhi keeps the QOpenGLContext current on the render thread
//   during updatePaintNode, so the first condition is met. For GLAD,
//   we call gladLoadCustomLoader once with a thunk that delegates to
//   QOpenGLContext::getProcAddress.
//
// * State-cache contract. Rive caches GL state internally and assumes
//   nothing else mutates it between calls. We sandwich every flush
//   with invalidateGLState() (before — Qt scribbled state since our
//   last frame) and unbindGLInternalResources() (after — Qt is about
//   to take the context back).
//
// * Zero-copy: one ID3D11Texture2D... wait, GL: one GL texture name,
//   wrapped via QRhiTexture::createFrom + createTextureFromRhiTexture.
//   Rive's TextureRenderTargetGL points at the same GLuint via
//   setTargetTexture; Qt's compositor samples it. No glTexSubImage2D,
//   no glReadPixels round-trip.
//
// * GL texture deletion: we don't glDeleteTextures(old) on resize —
//   Qt's QSGTexture (created via createTextureFromRhiTexture) takes
//   ownership of the QRhiTexture, which transitively manages the GL
//   texture. Same lifetime trap as Metal/D3D11 — see scheduleTextureCleanup.

#include "rive_gl_backend.h"

#include "../../rive/rive_qt_factory.h"

#include <QLoggingCategory>
#include <QOpenGLContext>
#include <QPointer>
#include <QQuickWindow>
#include <QRunnable>
#include <QSGRendererInterface>
#include <QSGTexture>
#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>

#include <atomic>

#include <rive/artboard.hpp>
#include <rive/layout.hpp>
#include <rive/math/aabb.hpp>
#include <rive/renderer/gl/render_context_gl_impl.hpp>
#include <rive/renderer/gl/render_target_gl.hpp>
#include <rive/renderer/render_context.hpp>
#include <rive/renderer/rive_renderer.hpp>

Q_LOGGING_CATEGORY(lcRiveGLBackend, "rive.gl.backend")

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

class TextureCleanupJob final : public QRunnable
{
public:
    explicit TextureCleanupJob(QSGTexture* qsg) : m_qsg(qsg) {}
    void run() override { delete m_qsg; }

private:
    QSGTexture* m_qsg = nullptr;
};

// GLAD's load function expects a (const char*) -> void(*)() callable.
// QOpenGLContext::getProcAddress fits with a reinterpret_cast — Qt's
// QFunctionPointer is itself a void(*)() typedef under the hood.
GLADapiproc qtGetGLProcAddress(const char* name)
{
    QOpenGLContext* ctx = QOpenGLContext::currentContext();
    if (!ctx)
        return nullptr;
    return reinterpret_cast<GLADapiproc>(ctx->getProcAddress(name));
}

// Idempotent: gladLoadCustomLoader can be called multiple times without
// harm, but doing it once per process keeps the log clean. Atomic
// because Qt's render thread isn't necessarily the same one across
// QRhi tear-down/re-init.
bool ensureGladLoaded()
{
    static std::atomic<bool> s_loaded{false};
    bool expected = false;
    if (!s_loaded.compare_exchange_strong(expected, true))
        return true;
    if (!gladLoadCustomLoader(&qtGetGLProcAddress))
    {
        s_loaded.store(false);
        return false;
    }
    return true;
}

} // namespace

struct RiveGLBackend::Impl
{
    QPointer<QQuickWindow> window;
    QRhi* rhi = nullptr;
    QOpenGLContext* glContext = nullptr;

    std::unique_ptr<rive::gpu::RenderContext> renderContext;
    std::unique_ptr<RiveQtFactory> factory;
    rive::rcp<rive::gpu::TextureRenderTargetGL> renderTarget;

    GLuint targetTextureId = 0;
    QRhiTexture* qrhiTexture = nullptr;
    QSGTexture* qsgTexture = nullptr;
    QSize textureSize;

    uint64_t currentFrameNumber = 0;

    rive::gpu::RenderContextGLImpl* glImpl() const
    {
        return renderContext
            ? renderContext->static_impl_cast<rive::gpu::RenderContextGLImpl>()
            : nullptr;
    }

    void scheduleTextureCleanup()
    {
        if (qsgTexture)
        {
            if (window)
            {
                window->scheduleRenderJob(new TextureCleanupJob(qsgTexture),
                                          QQuickWindow::AfterSwapStage);
            }
            // Window-gone path: leak the wrappers (process is shutting
            // down anyway, and the GL context may already be torn down
            // so glDeleteTextures would be unsafe).
            qsgTexture = nullptr;
        }
        // QRhiTexture is transitively owned by QSGPlainTexture; the
        // GLuint texture name is owned by the QRhiTexture (Qt 6.6+
        // adopts ownership through createFrom + createTextureFromRhi).
        qrhiTexture = nullptr;
        targetTextureId = 0;
        textureSize = QSize();
    }
};

RiveGLBackend::RiveGLBackend() : m_impl(std::make_unique<Impl>()) {}

RiveGLBackend::~RiveGLBackend()
{
    m_impl->factory.reset();
    m_impl->renderTarget = nullptr;
    m_impl->renderContext.reset();
    m_impl->scheduleTextureCleanup();
}

bool RiveGLBackend::initialize(QQuickWindow* window, QString* errorOut)
{
    if (m_impl->window == window && m_impl->renderContext)
        return true;

    auto setError = [&](const QString& msg) {
        if (errorOut)
            *errorOut = msg;
        qCWarning(lcRiveGLBackend) << msg;
    };

    if (!window)
    {
        setError(QStringLiteral("RiveGLBackend: null window"));
        return false;
    }

    QSGRendererInterface* rif = window->rendererInterface();
    if (!rif || rif->graphicsApi() != QSGRendererInterface::OpenGL)
    {
        setError(QStringLiteral(
            "RiveGLBackend: scene graph isn't on the OpenGL RHI "
            "(set QQuickWindow::setGraphicsApi(QSGRendererInterface::"
            "OpenGL) or QSG_RHI_BACKEND=opengl before constructing the "
            "QQuickWindow)"));
        return false;
    }

    auto* rhi = static_cast<QRhi*>(
        rif->getResource(window, QSGRendererInterface::RhiResource));
    if (!rhi)
    {
        setError(QStringLiteral("RiveGLBackend: QRhi resource unavailable"));
        return false;
    }

    auto* nh = static_cast<const QRhiGles2NativeHandles*>(rhi->nativeHandles());
    if (!nh || !nh->context)
    {
        setError(QStringLiteral(
            "RiveGLBackend: QRhiGles2NativeHandles missing QOpenGLContext"));
        return false;
    }

    QOpenGLContext* current = QOpenGLContext::currentContext();
    if (!current)
    {
        setError(QStringLiteral(
            "RiveGLBackend: no current QOpenGLContext at initialize() — "
            "QRhi should have made the context current before invoking "
            "updatePaintNode"));
        return false;
    }

    if (!ensureGladLoaded())
    {
        setError(QStringLiteral(
            "RiveGLBackend: gladLoadCustomLoader failed (QOpenGLContext::"
            "getProcAddress returned null for required symbols?)"));
        return false;
    }

    m_impl->window = window;
    m_impl->rhi = rhi;
    m_impl->glContext = nh->context;

    rive::gpu::RenderContextGLImpl::ContextOptions opts;
    m_impl->renderContext = rive::gpu::RenderContextGLImpl::MakeContext(opts);
    if (!m_impl->renderContext)
    {
        setError(QStringLiteral(
            "RiveGLBackend: RenderContextGLImpl::MakeContext failed"));
        return false;
    }

    m_impl->factory =
        std::make_unique<RiveQtFactory>(m_impl->renderContext.get());

    qCInfo(lcRiveGLBackend) << "Initialized on" << reinterpret_cast<const char*>(
        glGetString(GL_RENDERER));
    return true;
}

bool RiveGLBackend::isInitialized() const
{
    return m_impl->renderContext != nullptr;
}

rive::Factory* RiveGLBackend::factory() const
{
    return m_impl->factory.get();
}

QSGTexture* RiveGLBackend::ensureTexture(const QSize& pixelSize)
{
    if (!m_impl->renderContext || !m_impl->window)
        return nullptr;
    if (pixelSize.isEmpty())
        return nullptr;
    if (m_impl->textureSize == pixelSize && m_impl->qsgTexture)
        return m_impl->qsgTexture;

    m_impl->scheduleTextureCleanup();

    // Allocate a fresh GL texture. Rive will sample + render to it via
    // its own internal FBO (TextureRenderTargetGL wraps an FBO around
    // the texture name); Qt's compositor samples it via SRV. No
    // intermediate copy.
    GLuint tex = 0;
    glGenTextures(1, &tex);
    if (tex == 0)
    {
        qCWarning(lcRiveGLBackend) << "glGenTextures failed";
        return nullptr;
    }
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
                 pixelSize.width(), pixelSize.height(), 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    m_impl->targetTextureId = tex;
    m_impl->renderTarget = rive::make_rcp<rive::gpu::TextureRenderTargetGL>(
        static_cast<uint32_t>(pixelSize.width()),
        static_cast<uint32_t>(pixelSize.height()));

    QRhiTexture* qrhiTex = m_impl->rhi->newTexture(
        QRhiTexture::RGBA8, pixelSize, 1,
        QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource);
    if (!qrhiTex)
    {
        qCWarning(lcRiveGLBackend) << "QRhi::newTexture returned null";
        glDeleteTextures(1, &tex);
        m_impl->targetTextureId = 0;
        m_impl->renderTarget = nullptr;
        return nullptr;
    }
    QRhiTexture::NativeTexture native;
    native.object = static_cast<quint64>(tex);
    native.layout = 0;
    if (!qrhiTex->createFrom(native))
    {
        qCWarning(lcRiveGLBackend) << "QRhiTexture::createFrom failed";
        delete qrhiTex;
        glDeleteTextures(1, &tex);
        m_impl->targetTextureId = 0;
        m_impl->renderTarget = nullptr;
        return nullptr;
    }
    m_impl->qrhiTexture = qrhiTex;

    m_impl->qsgTexture = m_impl->window->createTextureFromRhiTexture(qrhiTex);
    if (!m_impl->qsgTexture)
    {
        qCWarning(lcRiveGLBackend) << "createTextureFromRhiTexture failed";
        m_impl->qrhiTexture = nullptr;
        delete qrhiTex;
        glDeleteTextures(1, &tex);
        m_impl->targetTextureId = 0;
        m_impl->renderTarget = nullptr;
        return nullptr;
    }

    m_impl->textureSize = pixelSize;
    return m_impl->qsgTexture;
}

void RiveGLBackend::renderFrame(rive::ArtboardInstance* artboard, FitMode fit)
{
    if (!artboard || !m_impl->renderContext || !m_impl->renderTarget ||
        m_impl->targetTextureId == 0)
        return;

    auto* impl = m_impl->glImpl();
    if (!impl)
        return;

    // Tell Rive that GL state was modified externally (Qt's RHI ran
    // its own scene-graph between our frames). Without this, Rive's
    // internal cache thinks bindings still hold and skips rebinds.
    impl->invalidateGLState();

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

    m_impl->renderTarget->setTargetTexture(m_impl->targetTextureId);

    m_impl->currentFrameNumber += 1;
    rive::gpu::RenderContext::FlushResources flushRes;
    flushRes.renderTarget = m_impl->renderTarget.get();
    // externalCommandBuffer left null — GL is immediate-mode.
    flushRes.currentFrameNumber = m_impl->currentFrameNumber;
    flushRes.safeFrameNumber =
        m_impl->currentFrameNumber > 0 ? m_impl->currentFrameNumber - 1 : 0;
    m_impl->renderContext->flush(flushRes);

    // Yield GL state cleanly — unbind Rive's VAOs/programs/FBOs so
    // Qt's RHI doesn't see them when it takes the context back.
    impl->unbindGLInternalResources();
}

void RiveGLBackend::abandonGraphicsResources()
{
    // QRhi tear-down. The GL context may already be invalid; we drop
    // CPU-side wrappers but skip glDeleteTextures (the driver reaps
    // orphaned names when the context dies).
    m_impl->factory.reset();
    m_impl->renderTarget = nullptr;
    m_impl->renderContext.reset();
    m_impl->qsgTexture = nullptr;
    m_impl->qrhiTexture = nullptr;
    m_impl->targetTextureId = 0;
    m_impl->textureSize = QSize();
    m_impl->glContext = nullptr;
    m_impl->rhi = nullptr;
    m_impl->window = nullptr;
}
