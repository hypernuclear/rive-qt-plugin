#include "rive_painter_factory.h"
#include "rive_painter_renderer.h"

#include <QBuffer>
#include <QImage>

namespace {

QColor toQColor(rive::ColorInt color)
{
    return QColor::fromRgb(
        (color >> 16) & 0xff,
        (color >> 8) & 0xff,
        color & 0xff,
        (color >> 24) & 0xff);
}

void fillGradientStops(QGradient& gradient,
                       const rive::ColorInt colors[],
                       const float stops[],
                       size_t count)
{
    QGradientStops qstops;
    qstops.reserve(static_cast<int>(count));
    for (size_t i = 0; i < count; ++i)
    {
        qstops.append({stops[i], toQColor(colors[i])});
    }
    gradient.setStops(qstops);
}

} // namespace

rive::rcp<rive::RenderBuffer> RivePainterFactory::makeRenderBuffer(
    rive::RenderBufferType type, rive::RenderBufferFlags flags, size_t sizeInBytes)
{
    return rive::make_rcp<RivePainterBuffer>(type, flags, sizeInBytes);
}

rive::rcp<rive::RenderShader> RivePainterFactory::makeLinearGradient(
    float sx, float sy, float ex, float ey,
    const rive::ColorInt colors[], const float stops[], size_t count)
{
    QLinearGradient g(QPointF(sx, sy), QPointF(ex, ey));
    // Gradients are expressed in the path's local coordinate system; the
    // world transform on the QPainter handles placement at draw time.
    g.setCoordinateMode(QGradient::LogicalMode);
    fillGradientStops(g, colors, stops, count);
    return rive::make_rcp<RivePainterShader>(g);
}

rive::rcp<rive::RenderShader> RivePainterFactory::makeRadialGradient(
    float cx, float cy, float radius,
    const rive::ColorInt colors[], const float stops[], size_t count)
{
    QRadialGradient g(QPointF(cx, cy), radius);
    g.setCoordinateMode(QGradient::LogicalMode);
    fillGradientStops(g, colors, stops, count);
    return rive::make_rcp<RivePainterShader>(g);
}

rive::rcp<rive::RenderPath> RivePainterFactory::makeRenderPath(
    rive::RawPath& path, rive::FillRule fillRule)
{
    return rive::make_rcp<RivePainterPath>(path, fillRule);
}

rive::rcp<rive::RenderPath> RivePainterFactory::makeEmptyRenderPath()
{
    return rive::make_rcp<RivePainterPath>();
}

rive::rcp<rive::RenderPaint> RivePainterFactory::makeRenderPaint()
{
    return rive::make_rcp<RivePainterPaint>();
}

rive::rcp<rive::RenderImage> RivePainterFactory::decodeImage(rive::Span<const uint8_t> bytes)
{
    // Qt's QImage reader handles PNG/JPEG/WebP/etc out of the box — no need
    // to pull in rive_decoders + libpng for the spike.
    QImage image = QImage::fromData(bytes.data(), static_cast<int>(bytes.size()));
    if (image.isNull())
        return nullptr;
    // Convert to a premultiplied format so QPainter's default blend math
    // matches rive's color expectations.
    if (image.format() != QImage::Format_ARGB32_Premultiplied)
        image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    return rive::make_rcp<RivePainterImage>(std::move(image));
}
