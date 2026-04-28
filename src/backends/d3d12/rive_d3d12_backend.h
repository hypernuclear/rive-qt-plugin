#ifndef RIVE_D3D12_BACKEND_H
#define RIVE_D3D12_BACKEND_H

// D3D12 implementation of RiveRenderBackend. See rive_render_backend.h
// for the interface contract.
//
// Pimpl: the .cpp pulls in <d3d12.h>, <dxgi.h>, and rive's PLS D3D12
// headers. Keeping those out of this header lets consumers compile
// without the Windows SDK / DirectX surface in their TUs.

#include "../rive_render_backend.h"

#include <memory>

class RiveD3D12Backend final : public RiveRenderBackend
{
public:
    RiveD3D12Backend();
    ~RiveD3D12Backend() override;

    bool initialize(QQuickWindow* window, QString* errorOut) override;
    bool isInitialized() const override;
    rive::Factory* factory() const override;
    QSGTexture* ensureTexture(const QSize& pixelSize) override;
    void renderFrame(rive::ArtboardInstance* artboard, FitMode fit) override;
    void abandonGraphicsResources() override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

#endif // RIVE_D3D12_BACKEND_H
