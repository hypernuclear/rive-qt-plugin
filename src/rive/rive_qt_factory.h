#ifndef RIVE_QT_FACTORY_H
#define RIVE_QT_FACTORY_H

// RiveQtFactory — thin rive::Factory wrapper that delegates everything
// to a backing rive::gpu::RenderContext EXCEPT image decoding, which
// goes through QImage.
//
// Why: the rive_decoders library (libpng/libjpeg/libwebp built from
// source) is disabled in this build to keep dependency surface small.
// Without it, RenderContext::decodeImage returns nullptr and any .riv
// containing embedded raster art renders those regions blank.
// Qt already ships PNG/JPEG/WebP decoding via QImage's QImageReader
// plugins, so we hijack that single virtual.
//
// This class does NOT own the RenderContext — it borrows the pointer
// the backend already keeps alive. Don't outlive the backend.

#include <rive/factory.hpp>

namespace rive {
namespace gpu {
class RenderContext;
}
}

class RiveQtFactory : public rive::Factory
{
public:
    explicit RiveQtFactory(rive::gpu::RenderContext* context);
    ~RiveQtFactory() override = default;

    // Forwarded to the wrapped RenderContext.
    rive::rcp<rive::RenderBuffer> makeRenderBuffer(rive::RenderBufferType,
                                                   rive::RenderBufferFlags,
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
    rive::rcp<rive::RenderPath> makeRenderPath(rive::RawPath& path,
                                               rive::FillRule fillRule) override;
    rive::rcp<rive::RenderPath> makeEmptyRenderPath() override;
    rive::rcp<rive::RenderPaint> makeRenderPaint() override;

    // Overridden to use QImage.
    rive::rcp<rive::RenderImage> decodeImage(rive::Span<const uint8_t> bytes) override;

private:
    rive::gpu::RenderContext* m_context;
};

#endif // RIVE_QT_FACTORY_H
