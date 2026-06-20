// Vulkan backend impl. Graphics-only — like Metal/D3D11/GL, doesn't
// know about rive::File / StateMachineInstance; those live in RiveView
// and the domain wrappers. See rive_render_backend.h for the contract.
//
// Notes:
//
// * We extract VkInstance/VkPhysicalDevice/VkDevice/queue from Qt's
//   QRhi (QRhiVulkanNativeHandles) and from Qt's QVulkanInstance
//   (vkInstance + getInstanceProcAddr loader). Rive's
//   RenderContextVulkanImpl::MakeContext takes those plus a
//   PFN_vkGetInstanceProcAddr — Qt's QVulkanInstance::getInstanceProcAddr
//   is exactly that, fetched via getInstanceProcAddr("vkGetInstanceProcAddr").
//
// * Zero-copy: we allocate a VkImage via Rive's VulkanContext (uses
//   Rive's vendored VMA), wrap it in a QRhiTexture via createFrom, and
//   pass the same VkImage + VkImageView to Rive's
//   RenderTargetVulkanImpl::setTargetImageView. Rive renders into the
//   image, Qt's compositor samples it. No copy.
//
// * Layout: first-cut keeps the target image in VK_IMAGE_LAYOUT_GENERAL
//   throughout. GENERAL covers both color-attachment writes and shader
//   sampling, at a (small) perf cost vs the OPTIMAL variants. Optimal
//   transitions are a follow-up if we observe sampling-perf issues.
//
// * Sync: Vulkan submissions on the same queue execute in order, but
//   memory visibility across submits requires explicit barriers. At
//   the end of our per-frame CB we emit an image memory barrier to
//   make Rive's writes visible to Qt's subsequent fragment-shader
//   reads.
//
// * Per-frame command buffers: we own a small ring of VkCommandBuffers
//   + VkFences, allocated against Qt's vkDevice + gfxQueueFamilyIdx,
//   submitted to Qt's gfxQueue. CPU waits on the fence at the start
//   of each frame slot to ensure last use is done before recycling.
//   All raw vk* calls go through Qt's QVulkanDeviceFunctions — no
//   link-time dependency on vulkan-1.lib.

#include "rive_vulkan_backend.h"

#include "../rive_render_backend_helpers.h"
#include "../../rive/rive_qt_factory.h"

#include <QLoggingCategory>
#include <QPointer>
#include <QQuickWindow>
#include <QRunnable>
#include <QSGRendererInterface>
#include <QSGTexture>
#include <QVulkanDeviceFunctions>
#include <QVulkanFunctions>
#include <QVulkanInstance>
#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>

#include <vulkan/vulkan.h>

#include <rive/artboard.hpp>
#include <rive/layout.hpp>
#include <rive/math/aabb.hpp>
#include <rive/renderer/render_context.hpp>
#include <rive/renderer/rive_renderer.hpp>
// Complete rive::gpu::Texture before the impl header: MSVC instantiates
// rcp<Texture>::~rcp at RenderContextImpl::platformDecodeImageTexture's
// by-value return declaration and needs the full type (Clang doesn't).
#include <rive/renderer/texture.hpp>
#include <rive/renderer/vulkan/render_context_vulkan_impl.hpp>
#include <rive/renderer/vulkan/render_target_vulkan.hpp>
#include <rive/renderer/vulkan/vulkan_context.hpp>
#include <rive/renderer/vulkan/vkutil.hpp>

Q_LOGGING_CATEGORY(lcRiveVulkanBackend, "rive.vulkan.backend")

namespace {

constexpr int kFramesInFlight = 2;
constexpr VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkImageUsageFlags kTargetUsageFlags =
    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
    VK_IMAGE_USAGE_SAMPLED_BIT |
    VK_IMAGE_USAGE_STORAGE_BIT |
    VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |  // PLS subpass-load reads.
    VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

class TextureCleanupJob final : public QRunnable
{
public:
    explicit TextureCleanupJob(QSGTexture* qsg) : m_qsg(qsg) {}
    void run() override { delete m_qsg; }

private:
    QSGTexture* m_qsg = nullptr;
};

} // namespace

struct RiveVulkanBackend::Impl
{
    QPointer<QQuickWindow> window;
    QRhi* rhi = nullptr;
    QVulkanInstance* qVkInst = nullptr;
    VkInstance vkInstance = VK_NULL_HANDLE;
    VkPhysicalDevice physDev = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t gfxQueueFamilyIdx = 0;
    VkQueue gfxQueue = VK_NULL_HANDLE;
    QVulkanDeviceFunctions* devFns = nullptr;

    std::unique_ptr<rive::gpu::RenderContext> renderContext;
    std::unique_ptr<RiveQtFactory> factory;
    rive::rcp<rive::gpu::RenderTargetVulkanImpl> renderTarget;

    rive::rcp<rive::gpu::vkutil::Image> targetImage;
    rive::rcp<rive::gpu::vkutil::ImageView> targetImageView;
    rive::gpu::vkutil::ImageAccess targetLastAccess;
    QRhiTexture* qrhiTexture = nullptr;
    QSGTexture* qsgTexture = nullptr;
    QSize textureSize;

    // Per-frame command-buffer ring.
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    VkCommandBuffer cmdBuffers[kFramesInFlight] = {VK_NULL_HANDLE,
                                                   VK_NULL_HANDLE};
    VkFence frameFences[kFramesInFlight] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    bool fenceArmed[kFramesInFlight] = {false, false};
    int frameSlot = 0;
    uint64_t currentFrameNumber = 0;

    rive::gpu::RenderContextVulkanImpl* vkImpl() const
    {
        return renderContext
            ? renderContext->static_impl_cast<rive::gpu::RenderContextVulkanImpl>()
            : nullptr;
    }

    rive::gpu::VulkanContext* vk() const
    {
        auto* impl = vkImpl();
        return impl ? impl->vulkanContext() : nullptr;
    }

    bool createCommandRing(QString* errorOut)
    {
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = gfxQueueFamilyIdx;
        if (devFns->vkCreateCommandPool(device, &poolInfo, nullptr, &cmdPool)
            != VK_SUCCESS)
        {
            if (errorOut)
                *errorOut = QStringLiteral(
                    "RiveVulkanBackend: vkCreateCommandPool failed");
            return false;
        }

        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = cmdPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = kFramesInFlight;
        if (devFns->vkAllocateCommandBuffers(device, &allocInfo, cmdBuffers)
            != VK_SUCCESS)
        {
            if (errorOut)
                *errorOut = QStringLiteral(
                    "RiveVulkanBackend: vkAllocateCommandBuffers failed");
            return false;
        }

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        // Unsignalled — we set fenceArmed[i] = false to skip the wait
        // on first use, then arm + signal in submission.
        for (int i = 0; i < kFramesInFlight; ++i)
        {
            if (devFns->vkCreateFence(device, &fenceInfo, nullptr,
                                      &frameFences[i]) != VK_SUCCESS)
            {
                if (errorOut)
                    *errorOut = QStringLiteral(
                        "RiveVulkanBackend: vkCreateFence failed");
                return false;
            }
        }
        return true;
    }

    void destroyCommandRing()
    {
        if (!devFns || device == VK_NULL_HANDLE)
            return;
        for (int i = 0; i < kFramesInFlight; ++i)
        {
            if (frameFences[i] != VK_NULL_HANDLE)
            {
                devFns->vkDestroyFence(device, frameFences[i], nullptr);
                frameFences[i] = VK_NULL_HANDLE;
            }
            cmdBuffers[i] = VK_NULL_HANDLE; // freed with the pool below
            fenceArmed[i] = false;
        }
        if (cmdPool != VK_NULL_HANDLE)
        {
            devFns->vkDestroyCommandPool(device, cmdPool, nullptr);
            cmdPool = VK_NULL_HANDLE;
        }
    }

    // sync=true for shutdown paths (~RiveVulkanBackend,
    // abandonGraphicsResources): we must delete the QSGTexture inline,
    // because by shutdown there's no more swap to fire AfterSwapStage,
    // and Qt's QRhi will warn ("going down with N unreleased
    // resources") when its tear-down finds a live native texture we
    // never deleted.
    //
    // sync=false (default) for in-flight resize: defer to AfterSwapStage
    // so the compositor's queued jobs don't dereference a freed pointer.
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
        // Wait for the GPU to finish any in-flight use of the old image
        // before dropping our refs. cheapest correct option — happens
        // only on resize/destroy, never per-frame.
        if (devFns && device != VK_NULL_HANDLE && (targetImage || targetImageView))
        {
            devFns->vkDeviceWaitIdle(device);
        }
        qrhiTexture = nullptr;
        // Drop renderTarget before the image/view it references so its
        // bare-handle copies are released before vkDestroyImage{View}.
        renderTarget = nullptr;
        targetImageView = nullptr;
        targetImage = nullptr;
        targetLastAccess = rive::gpu::vkutil::ImageAccess{};
        textureSize = QSize();
    }
};

RiveVulkanBackend::RiveVulkanBackend() : m_impl(std::make_unique<Impl>()) {}

RiveVulkanBackend::~RiveVulkanBackend()
{
    if (m_impl->devFns && m_impl->device != VK_NULL_HANDLE)
    {
        // Make sure all GPU work is done before we tear down our
        // per-frame ring + the rive context (which destroys lots of
        // VkObjects).
        m_impl->devFns->vkDeviceWaitIdle(m_impl->device);
    }
    m_impl->scheduleTextureCleanup(/*sync=*/true);
    m_impl->factory.reset();
    m_impl->renderTarget = nullptr;
    m_impl->renderContext.reset();
    m_impl->destroyCommandRing();
}

bool RiveVulkanBackend::initialize(QQuickWindow* window, QString* errorOut)
{
    if (m_impl->window == window && m_impl->renderContext)
        return true;

    auto setError = [&](const QString& msg) {
        if (errorOut)
            *errorOut = msg;
        qCWarning(lcRiveVulkanBackend) << msg;
    };

    if (!window)
    {
        setError(QStringLiteral("RiveVulkanBackend: null window"));
        return false;
    }

    QSGRendererInterface* rif = window->rendererInterface();
    if (!rif || rif->graphicsApi() != QSGRendererInterface::Vulkan)
    {
        setError(QStringLiteral(
            "RiveVulkanBackend: scene graph isn't on the Vulkan RHI "
            "(set QQuickWindow::setGraphicsApi(QSGRendererInterface::"
            "Vulkan) or QSG_RHI_BACKEND=vulkan before constructing the "
            "QQuickWindow)"));
        return false;
    }

    auto* rhi = static_cast<QRhi*>(
        rif->getResource(window, QSGRendererInterface::RhiResource));
    if (!rhi)
    {
        setError(QStringLiteral("RiveVulkanBackend: QRhi resource unavailable"));
        return false;
    }

    auto* nh = static_cast<const QRhiVulkanNativeHandles*>(rhi->nativeHandles());
    if (!nh || !nh->dev || !nh->physDev || !nh->gfxQueue || !nh->inst)
    {
        setError(QStringLiteral(
            "RiveVulkanBackend: QRhiVulkanNativeHandles missing required "
            "fields (physDev/dev/gfxQueue/inst)"));
        return false;
    }

    m_impl->window = window;
    m_impl->rhi = rhi;
    m_impl->qVkInst = nh->inst;
    m_impl->vkInstance = nh->inst->vkInstance();
    m_impl->physDev = nh->physDev;
    m_impl->device = nh->dev;
    m_impl->gfxQueueFamilyIdx = nh->gfxQueueFamilyIdx;
    m_impl->gfxQueue = nh->gfxQueue;
    m_impl->devFns = nh->inst->deviceFunctions(nh->dev);
    if (!m_impl->devFns)
    {
        setError(QStringLiteral(
            "RiveVulkanBackend: QVulkanInstance::deviceFunctions returned null"));
        return false;
    }

    auto fp_vkGetInstanceProcAddr =
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(
            nh->inst->getInstanceProcAddr("vkGetInstanceProcAddr"));
    if (!fp_vkGetInstanceProcAddr)
    {
        setError(QStringLiteral(
            "RiveVulkanBackend: failed to resolve vkGetInstanceProcAddr "
            "from QVulkanInstance"));
        return false;
    }

    // Conservative VulkanFeatures — defaults to API 1.1, no
    // optional-extension features enabled. Rive falls back to atomic
    // mode without rasterizer-ordered access; correctness is preserved,
    // performance can come from a follow-up that queries Qt's physDev
    // for VK_EXT_fragment_shader_interlock /
    // VK_EXT_rasterization_order_attachment_access support.
    rive::gpu::VulkanFeatures features;

    rive::gpu::RenderContextVulkanImpl::ContextOptions opts;
    m_impl->renderContext = rive::gpu::RenderContextVulkanImpl::MakeContext(
        m_impl->vkInstance,
        m_impl->physDev,
        m_impl->device,
        features,
        fp_vkGetInstanceProcAddr,
        opts);
    if (!m_impl->renderContext)
    {
        setError(QStringLiteral(
            "RiveVulkanBackend: RenderContextVulkanImpl::MakeContext failed"));
        return false;
    }

    if (!m_impl->createCommandRing(errorOut))
        return false;

    m_impl->factory =
        std::make_unique<RiveQtFactory>(m_impl->renderContext.get());

    qCInfo(lcRiveVulkanBackend) << "Initialized";
    return true;
}

bool RiveVulkanBackend::isInitialized() const
{
    return m_impl->renderContext != nullptr;
}

rive::Factory* RiveVulkanBackend::factory() const
{
    return m_impl->factory.get();
}

QSGTexture* RiveVulkanBackend::ensureTexture(const QSize& pixelSize)
{
    if (!m_impl->renderContext || !m_impl->window)
        return nullptr;
    if (pixelSize.isEmpty())
        return nullptr;
    if (m_impl->textureSize == pixelSize && m_impl->qsgTexture)
        return m_impl->qsgTexture;

    m_impl->scheduleTextureCleanup();

    auto* vk = m_impl->vk();
    if (!vk)
        return nullptr;

    // Allocate the VkImage via Rive's VulkanContext (VMA-backed).
    // Usage flags must match what we tell makeRenderTarget below, so
    // Rive's pipeline barriers stay coherent.
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = kColorFormat;
    imageInfo.extent = {static_cast<uint32_t>(pixelSize.width()),
                        static_cast<uint32_t>(pixelSize.height()),
                        1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = kTargetUsageFlags;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    m_impl->targetImage = vk->makeImage(imageInfo, "rive_qt_target_image");
    if (!m_impl->targetImage)
    {
        qCWarning(lcRiveVulkanBackend) << "VulkanContext::makeImage failed";
        return nullptr;
    }
    m_impl->targetImageView = vk->makeImageView(m_impl->targetImage,
                                                "rive_qt_target_view");
    if (!m_impl->targetImageView)
    {
        qCWarning(lcRiveVulkanBackend) << "VulkanContext::makeImageView failed";
        m_impl->targetImage = nullptr;
        return nullptr;
    }

    auto* impl = m_impl->vkImpl();
    m_impl->renderTarget = impl->makeRenderTarget(
        static_cast<uint32_t>(pixelSize.width()),
        static_cast<uint32_t>(pixelSize.height()),
        kColorFormat,
        kTargetUsageFlags);

    // Track the layout we're about to put the image in. Rive's
    // setTargetImageView will take it from here.
    m_impl->targetLastAccess = rive::gpu::vkutil::ImageAccess{};

    QRhiTexture* qrhiTex = m_impl->rhi->newTexture(
        QRhiTexture::RGBA8, pixelSize, 1,
        QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource);
    if (!qrhiTex)
    {
        qCWarning(lcRiveVulkanBackend) << "QRhi::newTexture returned null";
        m_impl->renderTarget = nullptr;
        m_impl->targetImageView = nullptr;
        m_impl->targetImage = nullptr;
        return nullptr;
    }
    QRhiTexture::NativeTexture native;
    native.object = reinterpret_cast<quint64>(static_cast<VkImage>(*m_impl->targetImage));
    // Tell Qt the image is in GENERAL layout — matches what we'll
    // leave it in after our barriers below.
    native.layout = static_cast<int>(VK_IMAGE_LAYOUT_GENERAL);
    if (!qrhiTex->createFrom(native))
    {
        qCWarning(lcRiveVulkanBackend) << "QRhiTexture::createFrom failed";
        delete qrhiTex;
        m_impl->renderTarget = nullptr;
        m_impl->targetImageView = nullptr;
        m_impl->targetImage = nullptr;
        return nullptr;
    }
    m_impl->qrhiTexture = qrhiTex;

    m_impl->qsgTexture = m_impl->window->createTextureFromRhiTexture(qrhiTex);
    if (!m_impl->qsgTexture)
    {
        qCWarning(lcRiveVulkanBackend) << "createTextureFromRhiTexture failed";
        m_impl->qrhiTexture = nullptr;
        delete qrhiTex;
        m_impl->renderTarget = nullptr;
        m_impl->targetImageView = nullptr;
        m_impl->targetImage = nullptr;
        return nullptr;
    }

    m_impl->textureSize = pixelSize;
    return m_impl->qsgTexture;
}

void RiveVulkanBackend::renderFrame(rive::ArtboardInstance* artboard,
                                    FitMode fit, AlignmentMode alignment)
{
    if (!artboard || !m_impl->renderContext || !m_impl->renderTarget ||
        !m_impl->targetImage || !m_impl->targetImageView)
        return;
    if (!m_impl->devFns)
        return;

    // Wait for this slot's previous use to complete before recycling.
    const int slot = m_impl->frameSlot;
    VkFence fence = m_impl->frameFences[slot];
    VkCommandBuffer cb = m_impl->cmdBuffers[slot];
    if (m_impl->fenceArmed[slot])
    {
        m_impl->devFns->vkWaitForFences(m_impl->device, 1, &fence, VK_TRUE,
                                        UINT64_MAX);
        m_impl->devFns->vkResetFences(m_impl->device, 1, &fence);
    }
    m_impl->devFns->vkResetCommandBuffer(cb, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (m_impl->devFns->vkBeginCommandBuffer(cb, &beginInfo) != VK_SUCCESS)
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

    m_impl->renderTarget->setTargetImageView(m_impl->targetImageView->vkImageView(),
                                             static_cast<VkImage>(*m_impl->targetImage),
                                             m_impl->targetLastAccess);

    m_impl->currentFrameNumber += 1;
    rive::gpu::RenderContext::FlushResources flushRes;
    flushRes.renderTarget = m_impl->renderTarget.get();
    flushRes.externalCommandBuffer = cb;
    flushRes.currentFrameNumber = m_impl->currentFrameNumber;
    flushRes.safeFrameNumber =
        m_impl->currentFrameNumber > 0 ? m_impl->currentFrameNumber - 1 : 0;
    m_impl->renderContext->flush(flushRes);

    // Pull the new image-access state from the render target so the
    // next renderFrame's setTargetImageView starts from the right
    // baseline. (Rive updates this internally as its render-pass runs.)
    m_impl->targetLastAccess = m_impl->renderTarget->targetLastAccess();

    // Memory + execution barrier: make Rive's color-attachment writes
    // visible to Qt's compositor, which will sample the image in the
    // fragment shader of its scene-graph render pass. Qt submits its
    // own VkCommandBuffer on the same queue *after* our submit
    // returns — same-queue submission order + this barrier give the
    // visibility guarantee.
    VkImageMemoryBarrier presentBarrier{};
    presentBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    presentBarrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                   VK_ACCESS_SHADER_WRITE_BIT;
    presentBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    presentBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    presentBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    presentBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    presentBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    presentBarrier.image = static_cast<VkImage>(*m_impl->targetImage);
    presentBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    presentBarrier.subresourceRange.baseMipLevel = 0;
    presentBarrier.subresourceRange.levelCount = 1;
    presentBarrier.subresourceRange.baseArrayLayer = 0;
    presentBarrier.subresourceRange.layerCount = 1;
    m_impl->devFns->vkCmdPipelineBarrier(
        cb,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0,
        0, nullptr,
        0, nullptr,
        1, &presentBarrier);

    if (m_impl->devFns->vkEndCommandBuffer(cb) != VK_SUCCESS)
        return;

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    if (m_impl->devFns->vkQueueSubmit(m_impl->gfxQueue, 1, &submit, fence)
        == VK_SUCCESS)
    {
        m_impl->fenceArmed[slot] = true;
    }

    m_impl->frameSlot = (slot + 1) % kFramesInFlight;
}

void RiveVulkanBackend::abandonGraphicsResources()
{
    // sceneGraphInvalidated fires before QRhi is torn down, so we can
    // (and must) delete the QSGTexture/QRhiTexture inline. Skipping
    // the deletion would leak the QRhiTexture and trigger Qt's
    // "QRhi going down with N unreleased resources" warning during
    // QRhi destruction.
    m_impl->scheduleTextureCleanup(/*sync=*/true);
    m_impl->factory.reset();
    m_impl->renderContext.reset();
    m_impl->destroyCommandRing();
    m_impl->textureSize = QSize();
    m_impl->gfxQueue = VK_NULL_HANDLE;
    m_impl->device = VK_NULL_HANDLE;
    m_impl->physDev = VK_NULL_HANDLE;
    m_impl->vkInstance = VK_NULL_HANDLE;
    m_impl->qVkInst = nullptr;
    m_impl->devFns = nullptr;
    m_impl->rhi = nullptr;
    m_impl->window = nullptr;
}
