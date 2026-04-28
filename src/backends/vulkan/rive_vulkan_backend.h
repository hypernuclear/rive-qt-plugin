#ifndef RIVE_VULKAN_BACKEND_H
#define RIVE_VULKAN_BACKEND_H

// Vulkan implementation of RiveRenderBackend. See rive_render_backend.h
// for the interface contract.
//
// Pimpl: keeps Vulkan headers + Rive's vulkan/* surface out of consumer
// TUs. Only the .cpp pulls in <vulkan/vulkan.h>.

#include "../rive_render_backend.h"

#include <memory>

class RiveVulkanBackend final : public RiveRenderBackend
{
public:
    RiveVulkanBackend();
    ~RiveVulkanBackend() override;

    bool initialize(QQuickWindow* window, QString* errorOut) override;
    bool isInitialized() const override;
    rive::Factory* factory() const override;
    QSGTexture* ensureTexture(const QSize& pixelSize) override;
    void renderFrame(rive::ArtboardInstance* artboard, FitMode fit,
                     AlignmentMode alignment) override;
    void abandonGraphicsResources() override;
    // Vulkan textures are sampled top-left like D3D/Metal — no flip.
    bool textureOriginIsBottomLeft() const override { return false; }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

#endif // RIVE_VULKAN_BACKEND_H
