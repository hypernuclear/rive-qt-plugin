#ifndef RIVE_PAINTER_FACTORY_H
#define RIVE_PAINTER_FACTORY_H

// QPainter-backed Rive factory + renderer (spike implementation, HYP-208).
//
// Rive's rendering is abstracted behind a `rive::Factory` (creates paints,
// paths, images, gradients) and a `rive::Renderer` (consumes them, executes
// draw calls). This file provides minimal Qt-backed implementations so a
// `rive::Artboard` can be painted into any `QPainter` target — letting us
// drive animations from a `QQuickPaintedItem` without pulling in Skia or
// Rive's GPU renderer.
//
// Fidelity notes for the spike:
//   - Fill/stroke paths, transforms, save/restore, clip: fully supported.
//   - Linear + radial gradients: supported via QLinearGradient/QRadialGradient.
//   - drawImage: supported via QImage.
//   - drawImageMesh: stubbed — triangulated image meshes are rare in .riv
//     files and QPainter has no direct equivalent. Follow-up work if needed.
//   - Blend modes: mapped to the closest QPainter composition mode.

#include <rive/factory.hpp>
#include <rive/renderer.hpp>

#include <QImage>
#include <QPainter>
#include <QPainterPath>

class RivePainterRenderer;

// rive::Factory implementation. One per RiveView is fine — it's cheap.
class RivePainterFactory : public rive::Factory
{
public:
    rive::rcp<rive::RenderBuffer> makeRenderBuffer(rive::RenderBufferType type,
                                                   rive::RenderBufferFlags flags,
                                                   size_t sizeInBytes) override;

    rive::rcp<rive::RenderShader> makeLinearGradient(float sx, float sy,
                                                     float ex, float ey,
                                                     const rive::ColorInt colors[],
                                                     const float stops[],
                                                     size_t count) override;

    rive::rcp<rive::RenderShader> makeRadialGradient(float cx, float cy,
                                                     float radius,
                                                     const rive::ColorInt colors[],
                                                     const float stops[],
                                                     size_t count) override;

    rive::rcp<rive::RenderPath> makeRenderPath(rive::RawPath& path, rive::FillRule fillRule) override;
    rive::rcp<rive::RenderPath> makeEmptyRenderPath() override;

    rive::rcp<rive::RenderPaint> makeRenderPaint() override;

    rive::rcp<rive::RenderImage> decodeImage(rive::Span<const uint8_t> bytes) override;
};

#endif // RIVE_PAINTER_FACTORY_H
