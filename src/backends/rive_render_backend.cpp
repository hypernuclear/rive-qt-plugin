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
    default:
        if (errorOut)
            *errorOut = QStringLiteral(
                "RiveRenderBackend::create: no backend for this RHI graphics API");
        return nullptr;
    }
}
