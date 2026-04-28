#ifndef RIVE_RENDER_BACKEND_H
#define RIVE_RENDER_BACKEND_H

// RiveRenderBackend — abstract graphics sink for a rive::ArtboardInstance.
//
// Implementations are per-RHI (Metal, D3D11, Vulkan, GL). Each one:
//   - pulls the graphics device out of Qt's QRhi so rive shares the
//     compositor's GPU resources,
//   - owns rive's RenderContext (also serves as the rive::Factory used
//     to decode .riv files and allocate GPU-backed images),
//   - allocates a QSGTexture-backed render target at the item's pixel
//     size,
//   - on each frame, encodes rive's commands into a command buffer
//     pulled from Qt's queue so ordering is naturally serialized
//     before the compositor reads the texture.
//
// The backend does NOT own any rive domain state (File, Artboard,
// StateMachine) — those live in RiveView and its child wrappers.
// `renderFrame()` takes an ArtboardInstance* pointer each call.
//
// Threading: every method must be called from Qt's scene-graph render
// thread (i.e. inside updatePaintNode / beforeRendering).

#include <QSize>
#include <QString>

#include <memory>

class QQuickWindow;
class QSGTexture;

namespace rive {
class ArtboardInstance;
class Factory;
}

class RiveRenderBackend
{
public:
    enum class FitMode
    {
        Fill,
        Contain,
        Cover,
        FitWidth,
        FitHeight,
        None,
        ScaleDown,
        // Defer to Rive's responsive layout system entirely — the
        // renderer applies no scaling. Pair with `layoutSize` on
        // RiveView to drive the artboard's runtime width/height.
        Layout
    };

    // 9 named alignment positions matching rive::Alignment statics.
    enum class AlignmentMode
    {
        TopLeft,
        TopCenter,
        TopRight,
        CenterLeft,
        Center,
        CenterRight,
        BottomLeft,
        BottomCenter,
        BottomRight
    };

    virtual ~RiveRenderBackend() = default;

    // Resolve the graphics device from the window's RHI and spin up
    // rive's backend-specific render context. Idempotent — re-calling
    // with the same window returns true without re-initializing.
    // Returns false with errorOut populated on failure.
    virtual bool initialize(QQuickWindow* window, QString* errorOut) = 0;

    virtual bool isInitialized() const = 0;

    // rive::Factory bound to this backend's device. Pass to
    // rive::File::import(). Valid only after initialize() succeeds.
    virtual rive::Factory* factory() const = 0;

    // Allocate (or return cached) a QSGTexture whose GPU-side storage
    // is the render target we'll draw into. Backend owns the texture's
    // lifetime — callers must NOT setOwnsTexture(true) on the node.
    //
    // Scheduling: when the size changes, cleanup of the previous
    // texture is deferred to AfterSwapStage so the compositor's
    // in-flight batches don't trip over a freed QRhiTexture.
    virtual QSGTexture* ensureTexture(const QSize& pixelSize) = 0;

    // Encode a frame for `artboard` into the texture most recently
    // returned by ensureTexture(). No-op if artboard is null or the
    // texture isn't ready. `fit` controls how the artboard is scaled,
    // `alignment` where it's anchored within the render target.
    virtual void renderFrame(rive::ArtboardInstance* artboard,
                             FitMode fit,
                             AlignmentMode alignment) = 0;

    // Drop references to RHI-bound resources WITHOUT deleting them.
    // Call from sceneGraphInvalidated so we don't touch the QRhi after
    // it tears down.
    virtual void abandonGraphicsResources() = 0;

    // True when the texture returned by ensureTexture() has its origin
    // at the bottom-left (OpenGL convention). Qt's compositor samples
    // textures with origin top-left, so consumers must mirror the V
    // axis when this is true. Default false matches Metal/D3D11/Vulkan.
    virtual bool textureOriginIsBottomLeft() const { return false; }

    // Pick a backend matching the window's RHI. Returns nullptr with
    // errorOut populated if no match (e.g. non-Metal RHI on macOS).
    static std::unique_ptr<RiveRenderBackend> create(QQuickWindow* window,
                                                     QString* errorOut);
};

#endif // RIVE_RENDER_BACKEND_H
