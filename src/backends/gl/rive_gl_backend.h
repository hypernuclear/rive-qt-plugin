#ifndef RIVE_GL_BACKEND_H
#define RIVE_GL_BACKEND_H

// OpenGL implementation of RiveRenderBackend. See rive_render_backend.h
// for the interface contract.
//
// Pimpl: keeps GLAD + Rive GL headers out of consumer TUs. The .cpp is
// the only place that pulls in <glad/gles2.h>.

#include "../rive_render_backend.h"

#include <memory>

class RiveGLBackend final : public RiveRenderBackend
{
public:
    RiveGLBackend();
    ~RiveGLBackend() override;

    bool initialize(QQuickWindow* window, QString* errorOut) override;
    bool isInitialized() const override;
    rive::Factory* factory() const override;
    QSGTexture* ensureTexture(const QSize& pixelSize) override;
    void renderFrame(rive::ArtboardInstance* artboard, FitMode fit) override;
    void abandonGraphicsResources() override;
    bool textureOriginIsBottomLeft() const override { return true; }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

#endif // RIVE_GL_BACKEND_H
