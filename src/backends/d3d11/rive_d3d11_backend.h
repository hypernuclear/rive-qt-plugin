#ifndef RIVE_D3D11_BACKEND_H
#define RIVE_D3D11_BACKEND_H

// D3D11 implementation of RiveRenderBackend. See rive_render_backend.h
// for the interface contract.
//
// Pimpl: the .cpp pulls in <d3d11.h>, <dxgi.h>, and rive's PLS D3D
// headers. Keeping those out of this header lets consumers compile
// without the Windows SDK / DirectX surface in their TUs.

#include "../rive_render_backend.h"

#include <memory>

class RiveD3D11Backend final : public RiveRenderBackend
{
public:
    RiveD3D11Backend();
    ~RiveD3D11Backend() override;

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

#endif // RIVE_D3D11_BACKEND_H
