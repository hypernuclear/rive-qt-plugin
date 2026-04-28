// D3D12 backend impl. Graphics-only — like Metal/D3D11/GL/Vulkan, doesn't
// know about rive::File / StateMachineInstance; those live in RiveView
// and the domain wrappers. See rive_render_backend.h for the contract.
//
// Notes:
//
// * We pull ID3D12Device + ID3D12CommandQueue from Qt's QRhi
//   (QRhiD3D12NativeHandles). Submitting our flush on Qt's queue
//   preserves ordering relative to Qt's own commands without explicit
//   cross-queue fence sync.
//
// * Bootstrap: RenderContextD3D12Impl::MakeContext takes a
//   one-shot ID3D12GraphicsCommandList that it records init commands
//   into (static buffers, default texture data). We allocate that list
//   here, hand it to MakeContext, close + execute it on Qt's queue,
//   then fence-wait for completion before per-frame work starts.
//
// * Per-frame ring: kFramesInFlight command allocators + DIRECT command
//   lists + fences. Mirrors the Vulkan backend's ring shape. CPU waits
//   on the slot's fence before recycling its allocator/list.
//
// * Zero-copy: we allocate an ID3D12Resource (R8G8B8A8_UNORM,
//   ALLOW_RENDER_TARGET | ALLOW_UNORDERED_ACCESS), wrap it in a
//   QRhiTexture via createFrom, and pass the same resource to Rive's
//   RenderTargetD3D12::setTargetTexture. Rive renders into the
//   resource, Qt's compositor samples it. No copy.
//
// * Resource state: we keep the target in COMMON state at rest. COMMON
//   promotes/demotes freely for most operations, so Qt's compositor
//   can transition to PIXEL_SHADER_RESOURCE for sampling without
//   explicit coordination. After Rive's flush we explicitly transition
//   back to COMMON. NativeTexture::layout is set accordingly.

#include "rive_d3d12_backend.h"

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

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <rive/artboard.hpp>
#include <rive/layout.hpp>
#include <rive/math/aabb.hpp>
#include <rive/renderer/d3d12/render_context_d3d12_impl.hpp>
#include <rive/renderer/render_context.hpp>
#include <rive/renderer/rive_renderer.hpp>

using Microsoft::WRL::ComPtr;

Q_LOGGING_CATEGORY(lcRiveD3D12Backend, "rive.d3d12.backend")

namespace {

constexpr int kFramesInFlight = 2;

class TextureCleanupJob final : public QRunnable
{
public:
    explicit TextureCleanupJob(QSGTexture* qsg) : m_qsg(qsg) {}
    void run() override { delete m_qsg; }

private:
    QSGTexture* m_qsg = nullptr;
};

// Best-effort vendor lookup. Mirrors the D3D11 backend's heuristic.
bool detectIntelGpu(ID3D12Device* device)
{
    if (!device)
        return false;
    LUID luid = device->GetAdapterLuid();
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))))
        return false;
    ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter))))
        return false;
    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(adapter->GetDesc1(&desc)))
        return false;
    return desc.VendorId == 0x163C ||
           desc.VendorId == 0x8086 ||
           desc.VendorId == 0x8087;
}

} // namespace

struct RiveD3D12Backend::Impl
{
    QPointer<QQuickWindow> window;
    QRhi* rhi = nullptr;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> commandQueue; // borrowed from Qt; AddRef'd

    std::unique_ptr<rive::gpu::RenderContext> renderContext;
    std::unique_ptr<RiveQtFactory> factory;
    rive::rcp<rive::gpu::RenderTargetD3D12> renderTarget;

    ComPtr<ID3D12Resource> targetTexture;
    QRhiTexture* qrhiTexture = nullptr;
    QSGTexture* qsgTexture = nullptr;
    QSize textureSize;

    // Per-frame command-buffer ring.
    ComPtr<ID3D12CommandAllocator> cmdAllocators[kFramesInFlight];
    ComPtr<ID3D12GraphicsCommandList> cmdLists[kFramesInFlight];
    ComPtr<ID3D12Fence> frameFence;
    HANDLE frameFenceEvent = nullptr;
    UINT64 frameFenceValues[kFramesInFlight] = {0, 0};
    UINT64 nextFrameFenceValue = 1;
    int frameSlot = 0;
    uint64_t currentFrameNumber = 0;

    rive::gpu::RenderContextD3D12Impl* d3d12Impl() const
    {
        return renderContext
            ? renderContext->static_impl_cast<rive::gpu::RenderContextD3D12Impl>()
            : nullptr;
    }

    bool createBootstrapAndContext(const rive::gpu::D3DContextOptions& opts,
                                   QString* errorOut)
    {
        // Bootstrap: a one-shot DIRECT command list used by MakeContext
        // to record initialization commands (static buffer uploads,
        // default texture data).
        ComPtr<ID3D12CommandAllocator> bootAllocator;
        if (FAILED(device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(&bootAllocator))))
        {
            if (errorOut)
                *errorOut = QStringLiteral(
                    "RiveD3D12Backend: bootstrap CreateCommandAllocator failed");
            return false;
        }
        ComPtr<ID3D12GraphicsCommandList> bootList;
        if (FAILED(device->CreateCommandList(
                0, D3D12_COMMAND_LIST_TYPE_DIRECT, bootAllocator.Get(),
                nullptr, IID_PPV_ARGS(&bootList))))
        {
            if (errorOut)
                *errorOut = QStringLiteral(
                    "RiveD3D12Backend: bootstrap CreateCommandList failed");
            return false;
        }

        renderContext = rive::gpu::RenderContextD3D12Impl::MakeContext(
            device, bootList.Get(), opts);
        if (!renderContext)
        {
            if (errorOut)
                *errorOut = QStringLiteral(
                    "RiveD3D12Backend: RenderContextD3D12Impl::MakeContext failed");
            return false;
        }

        if (FAILED(bootList->Close()))
        {
            if (errorOut)
                *errorOut = QStringLiteral(
                    "RiveD3D12Backend: bootstrap CL Close failed");
            return false;
        }
        ID3D12CommandList* lists[] = {bootList.Get()};
        commandQueue->ExecuteCommandLists(1, lists);

        // Create the per-frame fence + signal event used for both the
        // bootstrap wait and per-slot recycling.
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                       IID_PPV_ARGS(&frameFence))))
        {
            if (errorOut)
                *errorOut = QStringLiteral(
                    "RiveD3D12Backend: CreateFence failed");
            return false;
        }
        frameFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!frameFenceEvent)
        {
            if (errorOut)
                *errorOut = QStringLiteral(
                    "RiveD3D12Backend: CreateEvent failed");
            return false;
        }

        // Wait for the bootstrap CL to complete before continuing.
        const UINT64 bootFenceValue = nextFrameFenceValue++;
        if (FAILED(commandQueue->Signal(frameFence.Get(), bootFenceValue)))
        {
            if (errorOut)
                *errorOut = QStringLiteral(
                    "RiveD3D12Backend: bootstrap Signal failed");
            return false;
        }
        if (frameFence->GetCompletedValue() < bootFenceValue)
        {
            if (FAILED(frameFence->SetEventOnCompletion(bootFenceValue,
                                                        frameFenceEvent)))
            {
                if (errorOut)
                    *errorOut = QStringLiteral(
                        "RiveD3D12Backend: bootstrap SetEventOnCompletion failed");
                return false;
            }
            WaitForSingleObject(frameFenceEvent, INFINITE);
        }

        // Allocate the per-frame ring. Each list is created closed (we
        // pass null pipeline state and rely on Reset() before recording).
        for (int i = 0; i < kFramesInFlight; ++i)
        {
            if (FAILED(device->CreateCommandAllocator(
                    D3D12_COMMAND_LIST_TYPE_DIRECT,
                    IID_PPV_ARGS(&cmdAllocators[i]))))
            {
                if (errorOut)
                    *errorOut = QStringLiteral(
                        "RiveD3D12Backend: per-frame CreateCommandAllocator failed");
                return false;
            }
            if (FAILED(device->CreateCommandList(
                    0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                    cmdAllocators[i].Get(), nullptr,
                    IID_PPV_ARGS(&cmdLists[i]))))
            {
                if (errorOut)
                    *errorOut = QStringLiteral(
                        "RiveD3D12Backend: per-frame CreateCommandList failed");
                return false;
            }
            // Lists are open after creation; close so Reset() at frame
            // start has the expected state machine.
            if (FAILED(cmdLists[i]->Close()))
            {
                if (errorOut)
                    *errorOut = QStringLiteral(
                        "RiveD3D12Backend: per-frame CL Close failed");
                return false;
            }
        }

        return true;
    }

    void waitForGpuIdle()
    {
        if (!commandQueue || !frameFence || !frameFenceEvent)
            return;
        const UINT64 v = nextFrameFenceValue++;
        if (FAILED(commandQueue->Signal(frameFence.Get(), v)))
            return;
        if (frameFence->GetCompletedValue() < v)
        {
            frameFence->SetEventOnCompletion(v, frameFenceEvent);
            WaitForSingleObject(frameFenceEvent, INFINITE);
        }
    }

    void destroyFrameRing()
    {
        for (int i = 0; i < kFramesInFlight; ++i)
        {
            cmdLists[i].Reset();
            cmdAllocators[i].Reset();
            frameFenceValues[i] = 0;
        }
        if (frameFenceEvent)
        {
            CloseHandle(frameFenceEvent);
            frameFenceEvent = nullptr;
        }
        frameFence.Reset();
        nextFrameFenceValue = 1;
        frameSlot = 0;
    }

    // sync=true for shutdown paths (~RiveD3D12Backend,
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
        // Drop the rive render target before the resource it references.
        renderTarget = nullptr;
        targetTexture.Reset();
        textureSize = QSize();
    }
};

RiveD3D12Backend::RiveD3D12Backend() : m_impl(std::make_unique<Impl>()) {}

RiveD3D12Backend::~RiveD3D12Backend()
{
    // Make sure all GPU work is done before tearing things down.
    m_impl->waitForGpuIdle();
    // Drop rive state before the device disappears. Factory borrows
    // the RenderContext so it goes first.
    m_impl->factory.reset();
    m_impl->renderTarget = nullptr;
    m_impl->renderContext.reset();
    m_impl->scheduleTextureCleanup(/*sync=*/true);
    m_impl->destroyFrameRing();
}

bool RiveD3D12Backend::initialize(QQuickWindow* window, QString* errorOut)
{
    if (m_impl->window == window && m_impl->renderContext)
        return true;

    auto setError = [&](const QString& msg) {
        if (errorOut)
            *errorOut = msg;
        qCWarning(lcRiveD3D12Backend) << msg;
    };

    if (!window)
    {
        setError(QStringLiteral("RiveD3D12Backend: null window"));
        return false;
    }

    QSGRendererInterface* rif = window->rendererInterface();
    if (!rif || rif->graphicsApi() != QSGRendererInterface::Direct3D12)
    {
        setError(QStringLiteral(
            "RiveD3D12Backend: scene graph isn't on the Direct3D12 RHI "
            "(set QQuickWindow::setGraphicsApi(QSGRendererInterface::"
            "Direct3D12) or QSG_RHI_BACKEND=d3d12 before constructing "
            "the QQuickWindow)"));
        return false;
    }

    auto* rhi = static_cast<QRhi*>(
        rif->getResource(window, QSGRendererInterface::RhiResource));
    if (!rhi)
    {
        setError(QStringLiteral("RiveD3D12Backend: QRhi resource unavailable"));
        return false;
    }

    auto* nh = static_cast<const QRhiD3D12NativeHandles*>(rhi->nativeHandles());
    if (!nh || !nh->dev || !nh->commandQueue)
    {
        setError(QStringLiteral(
            "RiveD3D12Backend: QRhiD3D12NativeHandles missing device/queue"));
        return false;
    }

    m_impl->window = window;
    m_impl->rhi = rhi;
    // QRhi owns the device + command queue; ComPtr's AddRef gives us a
    // co-ownership ref, dropped in abandonGraphicsResources / dtor.
    m_impl->device = static_cast<ID3D12Device*>(nh->dev);
    m_impl->commandQueue = static_cast<ID3D12CommandQueue*>(nh->commandQueue);

    rive::gpu::D3DContextOptions opts;
    opts.isIntel = detectIntelGpu(m_impl->device.Get());

    if (!m_impl->createBootstrapAndContext(opts, errorOut))
        return false;

    m_impl->factory =
        std::make_unique<RiveQtFactory>(m_impl->renderContext.get());

    qCInfo(lcRiveD3D12Backend) << "Initialized" << (opts.isIntel ? "(Intel)" : "");
    return true;
}

bool RiveD3D12Backend::isInitialized() const
{
    return m_impl->renderContext != nullptr;
}

rive::Factory* RiveD3D12Backend::factory() const
{
    return m_impl->factory.get();
}

QSGTexture* RiveD3D12Backend::ensureTexture(const QSize& pixelSize)
{
    if (!m_impl->renderContext || !m_impl->window)
        return nullptr;
    if (pixelSize.isEmpty())
        return nullptr;
    if (m_impl->textureSize == pixelSize && m_impl->qsgTexture)
        return m_impl->qsgTexture;

    m_impl->scheduleTextureCleanup();

    // Resource flags: ALLOW_RENDER_TARGET for Rive's RTV writes,
    // ALLOW_UNORDERED_ACCESS so PLS writes go straight to our texture
    // instead of round-tripping through an internal offscreen UAV
    // target. SHADER_RESOURCE is implicit (no flag needed for SRV use).
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Alignment = 0;
    desc.Width = static_cast<UINT64>(pixelSize.width());
    desc.Height = static_cast<UINT>(pixelSize.height());
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;

    HRESULT hr = m_impl->device->CreateCommittedResource(
        &heap,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(&m_impl->targetTexture));
    if (FAILED(hr))
    {
        qCWarning(lcRiveD3D12Backend)
            << "CreateCommittedResource failed at" << pixelSize
            << "hr" << Qt::hex << hr;
        m_impl->targetTexture.Reset();
        return nullptr;
    }

    auto* impl = m_impl->d3d12Impl();
    m_impl->renderTarget = impl->makeRenderTarget(
        static_cast<uint32_t>(pixelSize.width()),
        static_cast<uint32_t>(pixelSize.height()));

    QRhiTexture* qrhiTex = m_impl->rhi->newTexture(
        QRhiTexture::RGBA8, pixelSize, 1,
        QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource);
    if (!qrhiTex)
    {
        qCWarning(lcRiveD3D12Backend) << "QRhi::newTexture returned null";
        m_impl->renderTarget = nullptr;
        m_impl->targetTexture.Reset();
        return nullptr;
    }
    QRhiTexture::NativeTexture native;
    native.object = reinterpret_cast<quint64>(m_impl->targetTexture.Get());
    // Tell Qt the texture's current resource state. COMMON promotes
    // freely on read, which lets Qt's compositor sample without an
    // explicit transition. We restore COMMON at the end of every flush.
    native.layout = static_cast<int>(D3D12_RESOURCE_STATE_COMMON);
    if (!qrhiTex->createFrom(native))
    {
        qCWarning(lcRiveD3D12Backend) << "QRhiTexture::createFrom failed";
        delete qrhiTex;
        m_impl->renderTarget = nullptr;
        m_impl->targetTexture.Reset();
        return nullptr;
    }
    m_impl->qrhiTexture = qrhiTex;

    m_impl->qsgTexture = m_impl->window->createTextureFromRhiTexture(qrhiTex);
    if (!m_impl->qsgTexture)
    {
        qCWarning(lcRiveD3D12Backend) << "createTextureFromRhiTexture failed";
        m_impl->qrhiTexture = nullptr;
        delete qrhiTex;
        m_impl->renderTarget = nullptr;
        m_impl->targetTexture.Reset();
        return nullptr;
    }

    m_impl->textureSize = pixelSize;
    return m_impl->qsgTexture;
}

void RiveD3D12Backend::renderFrame(rive::ArtboardInstance* artboard,
                                   FitMode fit, AlignmentMode alignment)
{
    if (!artboard || !m_impl->renderContext || !m_impl->renderTarget ||
        !m_impl->targetTexture)
        return;

    // Wait for this slot's previous use to complete before recycling
    // the allocator + command list.
    const int slot = m_impl->frameSlot;
    if (m_impl->frameFenceValues[slot] != 0 &&
        m_impl->frameFence->GetCompletedValue() < m_impl->frameFenceValues[slot])
    {
        m_impl->frameFence->SetEventOnCompletion(m_impl->frameFenceValues[slot],
                                                 m_impl->frameFenceEvent);
        WaitForSingleObject(m_impl->frameFenceEvent, INFINITE);
    }

    if (FAILED(m_impl->cmdAllocators[slot]->Reset()))
        return;
    if (FAILED(m_impl->cmdLists[slot]->Reset(m_impl->cmdAllocators[slot].Get(),
                                             nullptr)))
        return;

    ID3D12GraphicsCommandList* cmd = m_impl->cmdLists[slot].Get();

    // No external state transitions: Rive's RenderTargetD3D12 wraps our
    // ID3D12Resource via makeExternalTexture(..., RESOURCE_STATE_PRESENT)
    // (PRESENT == COMMON == 0) and tracks every transition the PLS
    // pipeline needs internally. Adding our own barriers here would
    // diverge our state from Rive's tracker and corrupt subsequent
    // transitions. RESOURCE_STATE_RENDER_TARGET | UNORDERED_ACCESS is
    // additionally not a legal combined state — they're mutually
    // exclusive write states.

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
    rive::gpu::RenderContextD3D12Impl::CommandLists cmdLists{};
    cmdLists.copyComandList = nullptr; // direct list handles copies too
    cmdLists.directComandList = cmd;

    rive::gpu::RenderContext::FlushResources flushRes;
    flushRes.renderTarget = m_impl->renderTarget.get();
    flushRes.externalCommandBuffer = &cmdLists;
    flushRes.currentFrameNumber = m_impl->currentFrameNumber;
    flushRes.safeFrameNumber =
        m_impl->currentFrameNumber > 0 ? m_impl->currentFrameNumber - 1 : 0;
    m_impl->renderContext->flush(flushRes);

    // No "clear target" call: the rcp<D3D12Texture> overload of
    // setTargetTexture deref's its argument unconditionally
    // (SetName(...)) and crashes on null. The ComPtr<ID3D12Resource>
    // overload re-wraps the resource fresh every frame via
    // makeExternalTexture, so a stale wrapper from the previous frame
    // is harmless — it's replaced at the next setTargetTexture call.
    // Resize churn is handled in ensureTexture by resetting
    // m_impl->renderTarget entirely.
    //
    // Rive leaves the texture in COMMON state at the end of flush
    // (per render_context_d3d12_impl.cpp ~line 1736) — matches what we
    // told QRhi via NativeTexture::layout.

    if (FAILED(cmd->Close()))
        return;

    ID3D12CommandList* lists[] = {cmd};
    m_impl->commandQueue->ExecuteCommandLists(1, lists);

    const UINT64 v = m_impl->nextFrameFenceValue++;
    if (SUCCEEDED(m_impl->commandQueue->Signal(m_impl->frameFence.Get(), v)))
    {
        m_impl->frameFenceValues[slot] = v;
    }

    m_impl->frameSlot = (slot + 1) % kFramesInFlight;
}

void RiveD3D12Backend::abandonGraphicsResources()
{
    // sceneGraphInvalidated fires before QRhi tear-down. Make sure
    // all our submitted work is done before we let the queue go away.
    m_impl->waitForGpuIdle();
    // Delete texture inline so its QRhiTexture is released before QRhi
    // destruction walks its tracker (otherwise: "QRhi going down with N
    // unreleased resources" warning).
    m_impl->scheduleTextureCleanup(/*sync=*/true);
    m_impl->factory.reset();
    m_impl->renderTarget = nullptr;
    m_impl->renderContext.reset();
    m_impl->destroyFrameRing();
    m_impl->commandQueue.Reset();
    m_impl->device.Reset();
    m_impl->rhi = nullptr;
    m_impl->window = nullptr;
}
