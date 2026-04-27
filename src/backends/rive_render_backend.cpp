// Platform dispatch for the RiveRenderBackend abstract factory.
//
// Concrete backend implementations live in per-RHI subdirectories
// (metal/, eventually d3d/, vulkan/, gl/). This file is the only place
// that knows which ones exist on which platforms — adding a new backend
// is one #include + one switch arm here.
//
// Always compiled, even on platforms with no backend, so RiveView can
// link and report a clean runtime error instead of a vague link failure.

#include "rive_render_backend.h"

#include <QQuickWindow>
#include <QSGRendererInterface>

#if defined(__APPLE__)
#include "metal/rive_metal_backend.h"
#endif

#if defined(_WIN32) && !defined(__APPLE__)
#include "d3d11/rive_d3d11_backend.h"
#endif

// GL backend covers Windows + Linux. macOS is intentionally excluded:
// Apple deprecated GL and we have a Metal backend there anyway.
#if !defined(__APPLE__)
#include "gl/rive_gl_backend.h"
#endif

std::unique_ptr<RiveRenderBackend>
RiveRenderBackend::create(QQuickWindow* window, QString* errorOut)
{
    if (!window)
    {
        if (errorOut)
            *errorOut = QStringLiteral("RiveRenderBackend::create: null window");
        return nullptr;
    }
    QSGRendererInterface* rif = window->rendererInterface();
    if (!rif)
    {
        if (errorOut)
            *errorOut = QStringLiteral(
                "RiveRenderBackend::create: window has no renderer interface");
        return nullptr;
    }
    switch (rif->graphicsApi())
    {
#if defined(__APPLE__)
    case QSGRendererInterface::Metal:
        return std::make_unique<RiveMetalBackend>();
#endif
#if defined(_WIN32) && !defined(__APPLE__)
    case QSGRendererInterface::Direct3D11:
        return std::make_unique<RiveD3D11Backend>();
    // Direct3D12 falls through to default — D3D12 backend is a follow-up.
    // Users who set QSG_RHI_BACKEND=d3d12 will see the "no backend" error
    // rather than a crash; switching back to the default RHI restores
    // rendering.
#endif
#if !defined(__APPLE__)
    case QSGRendererInterface::OpenGL:
        return std::make_unique<RiveGLBackend>();
#endif
    // Vulkan also falls through for now; the Vulkan backend is the
    // next planned phase.
    default:
        if (errorOut)
            *errorOut = QStringLiteral(
                "RiveRenderBackend::create: no backend for this RHI graphics API");
        return nullptr;
    }
}
