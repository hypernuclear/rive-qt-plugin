#include "rive_qt_factory.h"

#include <QImage>
#include <QtMath>

#include <rive/math/raw_path.hpp>
#include <rive/renderer/render_context.hpp>
#include <rive/renderer/render_context_impl.hpp>
#include <rive/renderer/rive_render_image.hpp>
#include <rive/renderer/texture.hpp>

RiveQtFactory::RiveQtFactory(rive::gpu::RenderContext* context) : m_context(context)
{}

rive::rcp<rive::RenderBuffer> RiveQtFactory::makeRenderBuffer(rive::RenderBufferType type,
                                                              rive::RenderBufferFlags flags,
                                                              size_t sizeInBytes)
{
    return m_context->makeRenderBuffer(type, flags, sizeInBytes);
}

rive::rcp<rive::RenderShader> RiveQtFactory::makeLinearGradient(float sx, float sy,
                                                                float ex, float ey,
                                                                const rive::ColorInt colors[],
                                                                const float stops[],
                                                                size_t count)
{
    return m_context->makeLinearGradient(sx, sy, ex, ey, colors, stops, count);
}

rive::rcp<rive::RenderShader> RiveQtFactory::makeRadialGradient(float cx, float cy,
                                                                float radius,
                                                                const rive::ColorInt colors[],
                                                                const float stops[],
                                                                size_t count)
{
    return m_context->makeRadialGradient(cx, cy, radius, colors, stops, count);
}

rive::rcp<rive::RenderPath> RiveQtFactory::makeRenderPath(rive::RawPath& path,
                                                          rive::FillRule fillRule)
{
    return m_context->makeRenderPath(path, fillRule);
}

rive::rcp<rive::RenderPath> RiveQtFactory::makeEmptyRenderPath()
{
    return m_context->makeEmptyRenderPath();
}

rive::rcp<rive::RenderPaint> RiveQtFactory::makeRenderPaint()
{
    return m_context->makeRenderPaint();
}

rive::rcp<rive::RenderImage> RiveQtFactory::decodeImage(rive::Span<const uint8_t> bytes)
{
    // QImage handles PNG / JPEG / WebP / BMP / GIF (first frame) etc.
    // out of the box via QImageReader plugins.
    QImage img = QImage::fromData(bytes.data(), static_cast<int>(bytes.size()));
    if (img.isNull())
        return nullptr;

    // Rive's makeImageTexture wants premultiplied RGBA8 byte order.
    // QImage::Format_RGBA8888_Premultiplied stores in that order
    // regardless of host endianness.
    if (img.format() != QImage::Format_RGBA8888_Premultiplied)
        img = img.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    if (img.isNull())
        return nullptr;

    const uint32_t w = static_cast<uint32_t>(img.width());
    const uint32_t h = static_cast<uint32_t>(img.height());

    // Mip count: floor(log2(max(w,h))) + 1, matching what rive's own
    // decoder path does. msb-style; never zero.
    uint32_t maxDim = std::max(w, h);
    uint32_t mipLevelCount = 1;
    while (maxDim >>= 1)
        ++mipLevelCount;

    // QImage may pad rows for alignment. makeImageTexture wants tightly
    // packed RGBA8. Repack if there's any stride mismatch.
    QImage packed = img;
    if (packed.bytesPerLine() != static_cast<int>(w) * 4)
        packed = packed.copy(); // copy() returns a tightly-packed buffer

    // v0.1.x added a GPUTextureFormat + compressed-block params to
    // makeImageTexture (KTX2 / BC7 / ASTC support). We supply only the
    // uncompressed RGBA8-premultiplied base level and let the backend
    // generate the mip chain via GPU blits — mirrors the runtime's own
    // bitmap path in render_context.cpp.
    rive::rcp<rive::gpu::Texture> texture = m_context->impl()->makeImageTexture(
        w, h, mipLevelCount,
        rive::GPUTextureFormat::rgba32,
        packed.constBits(),
        /*blockWidth=*/1,
        /*blockHeight=*/1,
        /*srgb=*/false,
        /*generateRemainingMips=*/true);
    if (!texture)
        return nullptr;
    return rive::make_rcp<rive::RiveRenderImage>(std::move(texture));
}
