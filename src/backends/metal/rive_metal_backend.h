#ifndef RIVE_METAL_BACKEND_H
#define RIVE_METAL_BACKEND_H

// Metal implementation of RiveRenderBackend. See rive_render_backend.h
// for the interface contract.
//
// ObjC++ implementation lives in the .mm file; this header stays pure
// C++ so consumers don't need to compile against <Metal/Metal.h>.

#include "../rive_render_backend.h"

#include <memory>

class RiveMetalBackend final : public RiveRenderBackend
{
public:
    RiveMetalBackend();
    ~RiveMetalBackend() override;

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

#endif // RIVE_METAL_BACKEND_H
