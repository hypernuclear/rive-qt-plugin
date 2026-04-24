#ifndef RIVE_VIEW_H
#define RIVE_VIEW_H

// RiveView — QML-facing item that plays a .riv file.
//
// Rendering: zero-copy GPU. We pull Qt's Metal device + command queue
// out of the scene-graph RHI, drive rive's PLS Metal renderer with the
// same device, and surface the rive output as a QSGTexture that the
// scene graph composites without copying. See rive_metal_renderer.h
// for the Metal interop details.
//
// Threading: setSource()/setFit()/setPlaying() are called on the GUI
// thread. updatePaintNode() runs on the render thread during the sync
// phase (GUI thread blocked) — that's where rive's per-frame work
// happens, including .riv decoding (deferred from setSource).
//
// Known limitations:
//   - Pointer events aren't forwarded to state-machine inputs (TODO).
//   - No QML API for state-machine inputs / view-model props (TODO).
//   - .riv files containing embedded PNG/JPEG images won't render
//     those pixels — rive_decoders is disabled in this build to keep
//     the dependency surface small. Animations without raster art
//     work fine.
//   - macOS only for now. Other RHIs (D3D, Vulkan) need analogous
//     RenderContext*Impl wiring.

#include <QByteArray>
#include <QElapsedTimer>
#include <QQuickItem>
#include <QString>
#include <QUrl>
#include <QtQmlIntegration/qqmlintegration.h>

#include <memory>

class RiveMetalRenderer;
class QSGNode;

class RiveView : public QQuickItem
{
    Q_OBJECT
    QML_ELEMENT

    // URL of the .riv file. qrc:// or file:// accepted.
    Q_PROPERTY(QUrl source READ source WRITE setSource NOTIFY sourceChanged)

    // Fit behaviour — mirrors rive::Fit as a QML-friendly enum.
    Q_PROPERTY(Fit fit READ fit WRITE setFit NOTIFY fitChanged)

    // Play/pause toggle. Defaults to true.
    Q_PROPERTY(bool playing READ isPlaying WRITE setPlaying NOTIFY playingChanged)

public:
    enum class Fit
    {
        Contain,
        Cover,
        Fill,
        None,
        ScaleDown
    };
    Q_ENUM(Fit)

    explicit RiveView(QQuickItem* parent = nullptr);
    ~RiveView() override;

    QUrl source() const { return m_source; }
    void setSource(const QUrl& url);

    Fit fit() const { return m_fit; }
    void setFit(Fit f);

    bool isPlaying() const { return m_playing; }
    void setPlaying(bool playing);

    // QQuickItem
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) override;

signals:
    void sourceChanged();
    void fitChanged();
    void playingChanged();

    // Emitted when the .riv file fails to load. QML can bind a toast to this.
    void loadFailed(const QString& reason);

protected:
    void itemChange(ItemChange change, const ItemChangeData& data) override;

private slots:
    void onBeforeSynchronizing();
    void onSceneGraphInvalidated();

private:
    void readSourceBytes();

    QUrl m_source;
    Fit m_fit = Fit::Contain;
    bool m_playing = true;

    // Bytes read on the GUI thread by setSource(); consumed (decoded
    // into a rive::File) on the render thread during updatePaintNode.
    // QByteArray is implicitly shared / copy-on-write so passing a
    // reference across threads at the sync barrier is safe.
    QByteArray m_pendingBytes;
    bool m_loadPending = false;

    // True once rive's advance reports "nothing more to animate".
    // Gates the per-frame update request.
    bool m_settled = false;

    QElapsedTimer m_frameTimer;
    qint64 m_lastAdvanceNs = 0;

    // Pimpl — Metal/ObjC details stay out of this header.
    std::unique_ptr<RiveMetalRenderer> m_renderer;

    // Whether we've successfully initialized the renderer against the
    // current window. Reset on sceneGraphInvalidated.
    bool m_rendererInitialized = false;
};

#endif // RIVE_VIEW_H
