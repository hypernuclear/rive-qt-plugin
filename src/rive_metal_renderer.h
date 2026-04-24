#ifndef RIVE_METAL_RENDERER_H
#define RIVE_METAL_RENDERER_H

// Encapsulates the Metal/rive interop. Pure C++ header so it can be
// included from non-ObjC translation units; the implementation lives in
// rive_metal_renderer.mm.
//
// Responsibilities:
//   - Pull the MTLDevice + MTLCommandQueue out of Qt's scene-graph RHI
//     so rive's GPU work shares the same device as Qt's compositor.
//   - Drive a rive::gpu::RenderContext (the PLS Metal backend) as both
//     the rive::Factory and the renderer.
//   - Keep an item-sized MTLTexture as the render target and wrap it as
//     a QSGTexture so the scene graph composites it without a copy.
//   - Decode .riv bytes (on demand) and own the resulting File/Artboard
//     /StateMachine.
//
// Threading: every method must be called from the scene-graph render
// thread (i.e. inside updatePaintNode or beforeRendering callbacks).

#include <QByteArray>
#include <QSize>
#include <QString>

#include <memory>

class QQuickWindow;
class QSGTexture;

class RiveMetalRenderer
{
public:
    // Mirrors RiveView::Fit so callers don't need to leak rive headers.
    enum class FitMode
    {
        Contain,
        Cover,
        Fill,
        None,
        ScaleDown
    };

    RiveMetalRenderer();
    ~RiveMetalRenderer();

    RiveMetalRenderer(const RiveMetalRenderer&) = delete;
    RiveMetalRenderer& operator=(const RiveMetalRenderer&) = delete;

    // Resolves the Metal device from the window's RHI and constructs the
    // rive PLS RenderContext. Returns false (with errorOut populated) if
    // the window's RHI isn't Metal or the context could not be built.
    // Idempotent — subsequent calls with the same window are no-ops.
    bool initialize(QQuickWindow* window, QString* errorOut);

    // Replaces any currently-loaded artboard with one decoded from
    // `bytes`. Returns false on parse failure / unsupported version.
    // Must be called after initialize().
    bool loadFile(const QByteArray& bytes, QString* errorOut);

    // Drops the current artboard / state-machine / file. Safe to call
    // before initialize().
    void unloadFile();

    // True if a successfully-loaded artboard is currently held.
    bool hasArtboard() const;

    // Steps the rive scene graph forward. Returns true if the artboard
    // reports more work pending (i.e. caller should keep advancing
    // every frame); false once the animation has settled.
    bool advance(float deltaSeconds);

    // Re-allocates the target MTLTexture if `pixelSize` differs from the
    // current size. The matching QSGTexture is returned and remains
    // owned by this renderer; the caller (typically a QSGSimpleTexture-
    // Node) must NOT take ownership.
    QSGTexture* ensureTexture(const QSize& pixelSize);

    // Encodes a frame: rive draws the artboard into the texture using a
    // command buffer pulled from the same MTLCommandQueue Qt uses, so
    // GPU-queue ordering naturally serializes our writes before Qt's
    // composite reads. No-op if `hasArtboard()` is false or the texture
    // hasn't been allocated yet.
    void renderFrame(FitMode fit);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

#endif // RIVE_METAL_RENDERER_H
