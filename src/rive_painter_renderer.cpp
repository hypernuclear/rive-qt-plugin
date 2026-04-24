#include "rive_painter_renderer.h"

#include <QTransform>

namespace {

QTransform toQTransform(const rive::Mat2D& m)
{
    // rive::Mat2D layout [x1, y1, x2, y2, tx, ty] maps 1:1 to QTransform's
    // affine 6-arg constructor (m11, m12, m21, m22, dx, dy).
    const float* v = m.values();
    return QTransform(v[0], v[1], v[2], v[3], v[4], v[5]);
}

QColor toQColor(rive::ColorInt color)
{
    // rive::ColorInt is 0xAARRGGBB.
    return QColor::fromRgb(
        (color >> 16) & 0xff,
        (color >> 8) & 0xff,
        color & 0xff,
        (color >> 24) & 0xff);
}

QPainter::CompositionMode toCompositionMode(rive::BlendMode blend)
{
    // Map rive blend modes to QPainter composition modes. Qt exposes the
    // SVG/Porter-Duff set, which covers every rive mode. The few that don't
    // have an exact analog fall back to SourceOver.
    switch (blend)
    {
    case rive::BlendMode::srcOver:   return QPainter::CompositionMode_SourceOver;
    case rive::BlendMode::screen:    return QPainter::CompositionMode_Screen;
    case rive::BlendMode::overlay:   return QPainter::CompositionMode_Overlay;
    case rive::BlendMode::darken:    return QPainter::CompositionMode_Darken;
    case rive::BlendMode::lighten:   return QPainter::CompositionMode_Lighten;
    case rive::BlendMode::colorDodge: return QPainter::CompositionMode_ColorDodge;
    case rive::BlendMode::colorBurn: return QPainter::CompositionMode_ColorBurn;
    case rive::BlendMode::hardLight: return QPainter::CompositionMode_HardLight;
    case rive::BlendMode::softLight: return QPainter::CompositionMode_SoftLight;
    case rive::BlendMode::difference: return QPainter::CompositionMode_Difference;
    case rive::BlendMode::exclusion: return QPainter::CompositionMode_Exclusion;
    case rive::BlendMode::multiply:  return QPainter::CompositionMode_Multiply;
    default:                         return QPainter::CompositionMode_SourceOver;
    }
}

Qt::PenJoinStyle toJoin(rive::StrokeJoin j)
{
    switch (j)
    {
    case rive::StrokeJoin::miter: return Qt::MiterJoin;
    case rive::StrokeJoin::round: return Qt::RoundJoin;
    case rive::StrokeJoin::bevel: return Qt::BevelJoin;
    }
    return Qt::MiterJoin;
}

Qt::PenCapStyle toCap(rive::StrokeCap c)
{
    switch (c)
    {
    case rive::StrokeCap::butt:   return Qt::FlatCap;
    case rive::StrokeCap::round:  return Qt::RoundCap;
    case rive::StrokeCap::square: return Qt::SquareCap;
    }
    return Qt::FlatCap;
}

} // namespace

// ---------------------------------------------------------------------------
// RivePainterPath
// ---------------------------------------------------------------------------

RivePainterPath::RivePainterPath(const rive::RawPath& rawPath, rive::FillRule rule)
{
    fillRule(rule);
    appendRawPath(rawPath);
}

void RivePainterPath::appendRawPath(const rive::RawPath& rawPath)
{
    // Stream rive path verbs → QPainterPath primitives. PathVerb only covers
    // move/line/quad/cubic/close; rive does not emit conics.
    for (auto [verb, pts] : rawPath)
    {
        switch (verb)
        {
        case rive::PathVerb::move:
            m_path.moveTo(pts[0].x, pts[0].y);
            break;
        case rive::PathVerb::line:
            m_path.lineTo(pts[1].x, pts[1].y);
            break;
        case rive::PathVerb::quad:
            m_path.quadTo(pts[1].x, pts[1].y, pts[2].x, pts[2].y);
            break;
        case rive::PathVerb::cubic:
            m_path.cubicTo(pts[1].x, pts[1].y, pts[2].x, pts[2].y, pts[3].x, pts[3].y);
            break;
        case rive::PathVerb::close:
            m_path.closeSubpath();
            break;
        }
    }
}

void RivePainterPath::rewind() { m_path = QPainterPath{}; }

void RivePainterPath::fillRule(rive::FillRule rule)
{
    m_path.setFillRule(rule == rive::FillRule::evenOdd
                           ? Qt::OddEvenFill
                           : Qt::WindingFill);
}

void RivePainterPath::moveTo(float x, float y) { m_path.moveTo(x, y); }
void RivePainterPath::lineTo(float x, float y) { m_path.lineTo(x, y); }

void RivePainterPath::cubicTo(float ox, float oy, float ix, float iy, float x, float y)
{
    m_path.cubicTo(ox, oy, ix, iy, x, y);
}

void RivePainterPath::close() { m_path.closeSubpath(); }

void RivePainterPath::addRenderPath(const rive::RenderPath* path, const rive::Mat2D& transform)
{
    const auto* other = static_cast<const RivePainterPath*>(path);
    const QTransform t = toQTransform(transform);
    m_path.addPath(t.map(other->m_path));
}

void RivePainterPath::addRawPath(const rive::RawPath& path) { appendRawPath(path); }

// ---------------------------------------------------------------------------
// RivePainterShader
// ---------------------------------------------------------------------------

RivePainterShader::RivePainterShader(const QLinearGradient& g)
    : m_type(Type::Linear), m_linear(g), m_radial()
{
}

RivePainterShader::RivePainterShader(const QRadialGradient& g)
    : m_type(Type::Radial), m_linear(), m_radial(g)
{
}

const QGradient& RivePainterShader::gradient() const
{
    return m_type == Type::Linear ? static_cast<const QGradient&>(m_linear)
                                  : static_cast<const QGradient&>(m_radial);
}

// ---------------------------------------------------------------------------
// RivePainterPaint
// ---------------------------------------------------------------------------

void RivePainterPaint::style(rive::RenderPaintStyle style) { m_style = style; }
void RivePainterPaint::color(rive::ColorInt value) { m_color = toQColor(value); }
void RivePainterPaint::thickness(float value) { m_thickness = value; }
void RivePainterPaint::join(rive::StrokeJoin value) { m_join = toJoin(value); }
void RivePainterPaint::cap(rive::StrokeCap value) { m_cap = toCap(value); }
void RivePainterPaint::blendMode(rive::BlendMode value) { m_blend = toCompositionMode(value); }

void RivePainterPaint::shader(rive::rcp<rive::RenderShader> s)
{
    m_shader = rive::static_rcp_cast<RivePainterShader>(s);
}

// ---------------------------------------------------------------------------
// RivePainterImage
// ---------------------------------------------------------------------------

RivePainterImage::RivePainterImage(QImage image) : m_image(std::move(image))
{
    m_Width = m_image.width();
    m_Height = m_image.height();
}

// ---------------------------------------------------------------------------
// RivePainterBuffer
// ---------------------------------------------------------------------------

RivePainterBuffer::RivePainterBuffer(rive::RenderBufferType type,
                                     rive::RenderBufferFlags flags,
                                     size_t sizeInBytes)
    : rive::RenderBuffer(type, flags, sizeInBytes), m_data(sizeInBytes)
{
}

void* RivePainterBuffer::onMap() { return m_data.data(); }
void RivePainterBuffer::onUnmap() {}

// ---------------------------------------------------------------------------
// RivePainterRenderer
// ---------------------------------------------------------------------------

RivePainterRenderer::RivePainterRenderer(QPainter* painter)
    : m_painter(painter), m_opacityStack{1.0f}
{
}

float RivePainterRenderer::currentOpacity() const
{
    return m_opacityStack.empty() ? 1.0f : m_opacityStack.back();
}

void RivePainterRenderer::save()
{
    m_painter->save();
    m_opacityStack.push_back(currentOpacity());
}

void RivePainterRenderer::restore()
{
    m_painter->restore();
    if (!m_opacityStack.empty())
        m_opacityStack.pop_back();
    if (m_opacityStack.empty())
        m_opacityStack.push_back(1.0f); // safety net — rive shouldn't over-pop
}

void RivePainterRenderer::transform(const rive::Mat2D& transform)
{
    m_painter->setWorldTransform(toQTransform(transform), /*combine=*/true);
}

void RivePainterRenderer::drawPath(rive::RenderPath* path, rive::RenderPaint* paint)
{
    const auto* rp = static_cast<const RivePainterPath*>(path);
    const auto* pp = static_cast<const RivePainterPaint*>(paint);

    m_painter->setCompositionMode(pp->compositionMode());
    m_painter->setOpacity(currentOpacity());

    // Build the paint source: either a solid color or a gradient brush. The
    // gradient coords are in the path's local space — QPainter's world
    // transform handles the final placement.
    QBrush brush;
    if (pp->shader())
    {
        brush = QBrush(pp->shader()->gradient());
    }
    else
    {
        brush = QBrush(pp->color());
    }

    if (pp->style() == rive::RenderPaintStyle::fill)
    {
        m_painter->fillPath(rp->qPath(), brush);
    }
    else // stroke
    {
        QPen pen(brush, pp->thickness());
        pen.setJoinStyle(pp->joinStyle());
        pen.setCapStyle(pp->capStyle());
        m_painter->strokePath(rp->qPath(), pen);
    }
}

void RivePainterRenderer::clipPath(rive::RenderPath* path)
{
    const auto* rp = static_cast<const RivePainterPath*>(path);
    m_painter->setClipPath(rp->qPath(), Qt::IntersectClip);
}

void RivePainterRenderer::drawImage(const rive::RenderImage* image,
                                    rive::ImageSampler,
                                    rive::BlendMode blend,
                                    float opacity)
{
    const auto* ri = static_cast<const RivePainterImage*>(image);
    m_painter->setCompositionMode(toCompositionMode(blend));
    m_painter->setOpacity(currentOpacity() * opacity);
    m_painter->drawImage(QPointF(0, 0), ri->qImage());
}

void RivePainterRenderer::drawImageMesh(const rive::RenderImage*,
                                        rive::ImageSampler,
                                        rive::rcp<rive::RenderBuffer>,
                                        rive::rcp<rive::RenderBuffer>,
                                        rive::rcp<rive::RenderBuffer>,
                                        uint32_t,
                                        uint32_t,
                                        rive::BlendMode,
                                        float)
{
    // Intentionally empty. Triangulated image meshes don't have a direct
    // QPainter equivalent and are rare in practice. Follow-up ticket if a
    // real .riv needs them — would rasterize onto a QImage via custom mesh
    // warp and then drawImage the result.
}

void RivePainterRenderer::modulateOpacity(float opacity)
{
    // Stacked multiplicatively per rive's contract — modulateOpacity(0.5)
    // then modulateOpacity(0.2) == 0.1 effective.
    const float next = currentOpacity() * opacity;
    if (m_opacityStack.empty())
        m_opacityStack.push_back(next);
    else
        m_opacityStack.back() = next;
}
