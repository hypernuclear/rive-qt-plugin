#ifndef RIVE_PAINTER_RENDERER_H
#define RIVE_PAINTER_RENDERER_H

// Renderer + render-object subclasses. See rive_painter_factory.h for the
// overall integration rationale.

#include <rive/factory.hpp>
#include <rive/renderer.hpp>
#include <rive/math/raw_path.hpp>

#include <QColor>
#include <QGradient>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QPen>

// ---------------------------------------------------------------------------
// RenderPath — QPainterPath holder. Fill rule (even-odd / non-zero) lives on
// the path itself in Qt, so we store it here and apply at draw time.
// ---------------------------------------------------------------------------
class RivePainterPath final : public rive::RenderPath
{
public:
    RivePainterPath() = default;
    RivePainterPath(const rive::RawPath& rawPath, rive::FillRule fillRule);

    // rive::CommandPath
    void rewind() override;
    void fillRule(rive::FillRule rule) override;
    void moveTo(float x, float y) override;
    void lineTo(float x, float y) override;
    void cubicTo(float ox, float oy, float ix, float iy, float x, float y) override;
    void close() override;

    // rive::RenderPath
    void addRenderPath(const rive::RenderPath* path, const rive::Mat2D& transform) override;
    void addRawPath(const rive::RawPath& path) override;

    const QPainterPath& qPath() const { return m_path; }

private:
    void appendRawPath(const rive::RawPath& rawPath);

    QPainterPath m_path;
};

// ---------------------------------------------------------------------------
// RenderShader — holds a QGradient. Radius/matrix set at construction; the
// Renderer resolves the Qt coordinate system when applying to a QBrush.
// ---------------------------------------------------------------------------
class RivePainterShader final : public rive::RenderShader
{
public:
    enum class Type { Linear, Radial };

    RivePainterShader(const QLinearGradient& gradient);
    RivePainterShader(const QRadialGradient& gradient);

    Type type() const { return m_type; }
    const QGradient& gradient() const;

private:
    Type m_type;
    QLinearGradient m_linear;
    QRadialGradient m_radial;
};

// ---------------------------------------------------------------------------
// RenderPaint — mirrors Rive's paint state (style, color, stroke, shader,
// blend) into a pair we can translate to QPen/QBrush at draw time.
// ---------------------------------------------------------------------------
class RivePainterPaint final : public rive::RenderPaint
{
public:
    RivePainterPaint() = default;

    void style(rive::RenderPaintStyle style) override;
    void color(rive::ColorInt value) override;
    void thickness(float value) override;
    void join(rive::StrokeJoin value) override;
    void cap(rive::StrokeCap value) override;
    void blendMode(rive::BlendMode value) override;
    void shader(rive::rcp<rive::RenderShader> shader) override;
    void invalidateStroke() override {} // no caching to invalidate

    rive::RenderPaintStyle style() const { return m_style; }
    QColor color() const { return m_color; }
    float thickness() const { return m_thickness; }
    Qt::PenJoinStyle joinStyle() const { return m_join; }
    Qt::PenCapStyle capStyle() const { return m_cap; }
    QPainter::CompositionMode compositionMode() const { return m_blend; }
    const RivePainterShader* shader() const { return m_shader.get(); }

private:
    rive::RenderPaintStyle m_style = rive::RenderPaintStyle::fill;
    QColor m_color = Qt::black;
    float m_thickness = 1.0f;
    Qt::PenJoinStyle m_join = Qt::MiterJoin;
    Qt::PenCapStyle m_cap = Qt::FlatCap;
    QPainter::CompositionMode m_blend = QPainter::CompositionMode_SourceOver;
    rive::rcp<RivePainterShader> m_shader;
};

// ---------------------------------------------------------------------------
// RenderImage — QImage wrapper. Decoding is handled by the factory.
// ---------------------------------------------------------------------------
class RivePainterImage final : public rive::RenderImage
{
public:
    explicit RivePainterImage(QImage image);

    const QImage& qImage() const { return m_image; }

private:
    QImage m_image;
};

// ---------------------------------------------------------------------------
// RenderBuffer — stub. Only needed for drawImageMesh, which we don't yet
// support. Having the type exist keeps rive's factory contract happy.
// ---------------------------------------------------------------------------
class RivePainterBuffer final : public rive::RenderBuffer
{
public:
    RivePainterBuffer(rive::RenderBufferType type,
                      rive::RenderBufferFlags flags,
                      size_t sizeInBytes);

    void* onMap() override;
    void onUnmap() override;

private:
    std::vector<uint8_t> m_data;
};

// ---------------------------------------------------------------------------
// Renderer — drives a QPainter through rive's draw commands. Rive's matrix
// stack maps 1:1 to QPainter::save/restore + setTransform.
// ---------------------------------------------------------------------------
class RivePainterRenderer final : public rive::Renderer
{
public:
    explicit RivePainterRenderer(QPainter* painter);

    void save() override;
    void restore() override;
    void transform(const rive::Mat2D& transform) override;
    void drawPath(rive::RenderPath* path, rive::RenderPaint* paint) override;
    void clipPath(rive::RenderPath* path) override;
    void drawImage(const rive::RenderImage* image,
                   rive::ImageSampler,
                   rive::BlendMode blend,
                   float opacity) override;
    void drawImageMesh(const rive::RenderImage*,
                       rive::ImageSampler,
                       rive::rcp<rive::RenderBuffer> vertices_f32,
                       rive::rcp<rive::RenderBuffer> uvCoords_f32,
                       rive::rcp<rive::RenderBuffer> indices_u16,
                       uint32_t vertexCount,
                       uint32_t indexCount,
                       rive::BlendMode blend,
                       float opacity) override;
    void modulateOpacity(float opacity) override;

private:
    // Opacity stack: each save() captures the current, restore() pops.
    // Rive expects modulateOpacity to be stacked multiplicatively.
    float currentOpacity() const;

    QPainter* m_painter;
    std::vector<float> m_opacityStack;
};

#endif // RIVE_PAINTER_RENDERER_H
