// D3D11 backend impl. Graphics-only — like the Metal backend, it doesn't
// know about rive::File or StateMachineInstance; those live in RiveView
// and the domain wrappers. See rive_render_backend.h for the contract.
//
// Notes:
//
// * We pull ID3D11Device + ID3D11DeviceContext from Qt's QRhi so rive
//   issues commands on the same immediate context the compositor uses.
//   Unlike Metal (per-frame MTLCommandBuffer on a shared queue), D3D11
//   has no command-buffer abstraction at this level — submission is
//   implicit on the immediate context, and Qt's Present cycle drives
//   the actual GPU dispatch. Our renderFrame runs *before* Qt's render
//   pass, so any state Rive sets is replaced by Qt before the
//   compositor draws.
//
// * Zero-copy: the same ID3D11Texture2D is the destination Rive renders
//   into (RTV + UAV) and the source Qt's compositor samples (SRV).
//   Required bind flags: RENDER_TARGET | SHADER_RESOURCE |
//   UNORDERED_ACCESS. Without UAV, Rive's PLS path falls back to an
//   internal offscreen texture and copies, defeating the zero-copy goal.
//
// * The QSGPlainTexture returned by createTextureFromRhiTexture
//   transitively owns the QRhiTexture — same trap as Metal. We only
//   delete the QSGTexture; QRhi-side cleanup cascades. Deferred to
//   AfterSwapStage via scheduleRenderJob.

#include "rive_d3d11_backend.h"

#include "../rive_render_backend_helpers.h"
#include "../../rive/rive_qt_factory.h"

#include <QLoggingCategory>
#include <QPointer>
#include <QQuickWindow>
#include <QRunnable>
#include <QSGRendererInterface>
#include <QSGTexture>
#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>

#include <d3d11.h>
#include <dxgi.h>

#include <rive/artboard.hpp>
#include <rive/layout.hpp>
#include <rive/math/aabb.hpp>
// Complete rive::gpu::Texture before the impl header: MSVC instantiates
// rcp<Texture>::~rcp at RenderContextImpl::platformDecodeImageTexture's
// by-value return declaration and needs the full type (Clang doesn't).
#include <rive/renderer/texture.hpp>
#include <rive/renderer/d3d11/render_context_d3d_impl.hpp>
#include <rive/renderer/render_context.hpp>
#include <rive/renderer/rive_renderer.hpp>

Q_LOGGING_CATEGORY(lcRiveD3D11Backend, "rive.d3d11.backend")

namespace {

class TextureCleanupJob final : public QRunnable
{
public:
    explicit TextureCleanupJob(QSGTexture* qsg) : m_qsg(qsg) {}
    void run() override { delete m_qsg; }

private:
    QSGTexture* m_qsg = nullptr;
};

// Best-effort vendor-ID lookup. Rive's PLS pipeline takes vendor-specific
// paths (Intel quirks in particular) when D3DContextOptions::isIntel is
// set. Returning false here just means "stay on the generic path", which
// is safe; the worst case is a perf delta on Intel iGPUs.
bool detectIntelGpu(ID3D11Device* device)
{
    if (!device)
        return false;
    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))))
        return false;
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDevice->GetAdapter(&adapter)))
        return false;
    DXGI_ADAPTER_DESC desc{};
    if (FAILED(adapter->GetDesc(&desc)))
        return false;
    // Vendor IDs that Rive's fiddle context treats as Intel.
    return desc.VendorId == 0x163C ||
           desc.VendorId == 0x8086 ||
           desc.VendorId == 0x8087;
}

} // namespace

struct RiveD3D11Backend::Impl
{
    QPointer<QQuickWindow> window;
    QRhi* rhi = nullptr;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;

    std::unique_ptr<rive::gpu::RenderContext> renderContext;
    std::unique_ptr<RiveQtFactory> factory;
    rive::rcp<rive::gpu::RenderTargetD3D> renderTarget;

    ComPtr<ID3D11Texture2D> targetTexture;
    QRhiTexture* qrhiTexture = nullptr;
    QSGTexture* qsgTexture = nullptr;
    QSize textureSize;

    uint64_t currentFrameNumber = 0;

    // sync=true for shutdown paths (~RiveD3D11Backend,
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
        targetTexture.Reset();
        textureSize = QSize();
    }
};

RiveD3D11Backend::RiveD3D11Backend() : m_impl(std::make_unique<Impl>()) {}

RiveD3D11Backend::~RiveD3D11Backend()
{
    // Drop rive state before the device disappears. Factory borrows
    // the RenderContext so it goes first.
    m_impl->factory.reset();
    m_impl->renderTarget = nullptr;
    m_impl->renderContext.reset();

    m_impl->scheduleTextureCleanup(/*sync=*/true);
}

bool RiveD3D11Backend::initialize(QQuickWindow* window, QString* errorOut)
{
    if (m_impl->window == window && m_impl->renderContext)
        return true;

    auto setError = [&](const QString& msg) {
        if (errorOut)
            *errorOut = msg;
        qCWarning(lcRiveD3D11Backend) << msg;
    };

    if (!window)
    {
        setError(QStringLiteral("RiveD3D11Backend: null window"));
        return false;
    }

    QSGRendererInterface* rif = window->rendererInterface();
    if (!rif || rif->graphicsApi() != QSGRendererInterface::Direct3D11)
    {
        setError(QStringLiteral(
            "RiveD3D11Backend: scene graph isn't on the Direct3D11 RHI "
            "(set QQuickWindow::setGraphicsApi(QSGRendererInterface::"
            "Direct3D11) before constructing the QQuickWindow, or unset "
            "QSG_RHI_BACKEND if it's pointing elsewhere)"));
        return false;
    }

    auto* rhi = static_cast<QRhi*>(
        rif->getResource(window, QSGRendererInterface::RhiResource));
    if (!rhi)
    {
        setError(QStringLiteral("RiveD3D11Backend: QRhi resource unavailable"));
        return false;
    }

    auto* nh = static_cast<const QRhiD3D11NativeHandles*>(rhi->nativeHandles());
    if (!nh || !nh->dev || !nh->context)
    {
        setError(QStringLiteral(
            "RiveD3D11Backend: QRhiD3D11NativeHandles missing device/context"));
        return false;
    }

    m_impl->window = window;
    m_impl->rhi = rhi;
    // QRhi owns the device/context; ComPtr's AddRef gives us a
    // co-ownership ref, dropped in abandonGraphicsResources / dtor.
    m_impl->device = static_cast<ID3D11Device*>(nh->dev);
    m_impl->context = static_cast<ID3D11DeviceContext*>(nh->context);

    rive::gpu::D3DContextOptions opts;
    opts.isIntel = detectIntelGpu(m_impl->device.Get());
    m_impl->renderContext = rive::gpu::RenderContextD3DImpl::MakeContext(
        m_impl->device, m_impl->context, opts);
    if (!m_impl->renderContext)
    {
        setError(QStringLiteral(
            "RiveD3D11Backend: RenderContextD3DImpl::MakeContext failed"));
        return false;
    }

    // QImage-backed factory wrapper so embedded raster art in .riv
    // files decodes (rive_decoders is disabled in this build).
    m_impl->factory =
        std::make_unique<RiveQtFactory>(m_impl->renderContext.get());

    qCInfo(lcRiveD3D11Backend) << "Initialized" << (opts.isIntel ? "(Intel)" : "");
    return true;
}

bool RiveD3D11Backend::isInitialized() const
{
    return m_impl->renderContext != nullptr;
}

rive::Factory* RiveD3D11Backend::factory() const
{
    return m_impl->factory.get();
}

QSGTexture* RiveD3D11Backend::ensureTexture(const QSize& pixelSize)
{
    if (!m_impl->renderContext || !m_impl->window)
        return nullptr;
    if (pixelSize.isEmpty())
        return nullptr;
    if (m_impl->textureSize == pixelSize && m_impl->qsgTexture)
        return m_impl->qsgTexture;

    m_impl->scheduleTextureCleanup();

    // Bind flags: RT for Rive's RTV writes, SR for Qt's compositor SRV
    // sampling, UAV so RenderTargetD3D::targetTextureSupportsUAV() is
    // true and Rive's PLS path writes directly into our texture instead
    // of round-tripping through an internal offscreen UAV target.
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(pixelSize.width());
    desc.Height = static_cast<UINT>(pixelSize.height());
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE |
                     D3D11_BIND_UNORDERED_ACCESS;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;

    HRESULT hr = m_impl->device->CreateTexture2D(
        &desc, nullptr, m_impl->targetTexture.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        qCWarning(lcRiveD3D11Backend)
            << "CreateTexture2D failed at" << pixelSize << "hr" << Qt::hex << hr;
        m_impl->targetTexture.Reset();
        return nullptr;
    }

    auto* impl = m_impl->renderContext
        ->static_impl_cast<rive::gpu::RenderContextD3DImpl>();
    m_impl->renderTarget = impl->makeRenderTarget(
        static_cast<uint32_t>(pixelSize.width()),
        static_cast<uint32_t>(pixelSize.height()));

    QRhiTexture* qrhiTex = m_impl->rhi->newTexture(
        QRhiTexture::RGBA8, pixelSize, 1,
        QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource);
    if (!qrhiTex)
    {
        qCWarning(lcRiveD3D11Backend) << "QRhi::newTexture returned null";
        m_impl->targetTexture.Reset();
        return nullptr;
    }
    QRhiTexture::NativeTexture native;
    native.object = reinterpret_cast<quint64>(m_impl->targetTexture.Get());
    native.layout = 0;
    if (!qrhiTex->createFrom(native))
    {
        qCWarning(lcRiveD3D11Backend) << "QRhiTexture::createFrom failed";
        delete qrhiTex;
        m_impl->targetTexture.Reset();
        return nullptr;
    }
    m_impl->qrhiTexture = qrhiTex;

    m_impl->qsgTexture = m_impl->window->createTextureFromRhiTexture(qrhiTex);
    if (!m_impl->qsgTexture)
    {
        qCWarning(lcRiveD3D11Backend) << "createTextureFromRhiTexture failed";
        m_impl->qrhiTexture = nullptr;
        delete qrhiTex;
        m_impl->targetTexture.Reset();
        return nullptr;
    }

    m_impl->textureSize = pixelSize;
    return m_impl->qsgTexture;
}

void RiveD3D11Backend::renderFrame(rive::ArtboardInstance* artboard,
                                   FitMode fit, AlignmentMode alignment)
{
    if (!artboard || !m_impl->renderContext || !m_impl->renderTarget ||
        !m_impl->targetTexture)
        return;

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
    renderer.align(rive_qt::toRiveFit(fit),
                   rive_qt::toRiveAlignment(alignment),
                   frame, artboard->bounds());
    artboard->draw(&renderer);
    renderer.restore();

    m_impl->renderTarget->setTargetTexture(m_impl->targetTexture);

    m_impl->currentFrameNumber += 1;
    rive::gpu::RenderContext::FlushResources flushRes;
    flushRes.renderTarget = m_impl->renderTarget.get();
    // externalCommandBuffer left null — D3D11 issues to the immediate
    // context, no per-frame command-buffer handoff.
    flushRes.currentFrameNumber = m_impl->currentFrameNumber;
    flushRes.safeFrameNumber =
        m_impl->currentFrameNumber > 0 ? m_impl->currentFrameNumber - 1 : 0;
    m_impl->renderContext->flush(flushRes);

    // Drop the ComPtr ref so the next ensureTexture can replace the
    // texture without RenderTargetD3D holding a stale reference.
    m_impl->renderTarget->setTargetTexture(nullptr);
}

void RiveD3D11Backend::abandonGraphicsResources()
{
    // sceneGraphInvalidated fires before QRhi tears down — delete the
    // QSGTexture inline so its QRhiTexture (and the underlying
    // ID3D11Texture2D wrapper) is released before QRhi destruction
    // walks its tracker. Otherwise Qt warns about unreleased resources.
    m_impl->scheduleTextureCleanup(/*sync=*/true);
    m_impl->factory.reset();
    m_impl->renderTarget = nullptr;
    m_impl->renderContext.reset();
    m_impl->context.Reset();
    m_impl->device.Reset();
    m_impl->rhi = nullptr;
    m_impl->window = nullptr;
}
