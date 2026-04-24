#ifndef RIVE_VIEW_H
#define RIVE_VIEW_H

// RiveView — QML-facing item that plays a .riv file.
//
// Owns a rive::File / ArtboardInstance / StateMachineInstance and drives
// them off the scene graph's frame clock. Paints via a QPainter-backed
// rive::Renderer (see rive_painter_renderer.h) — CPU rasterization into
// the QQuickPaintedItem's backing texture.
//
// Known limitations:
//   - No pointer-event forwarding to state-machine inputs (TODO).
//   - No QML API for reading/writing state-machine inputs or view-model
//     properties (TODO).
//   - CPU rasterization only. A future implementation should render Rive
//     on Metal/D3D/Vulkan directly into a QSGTexture for zero-copy
//     compositing with the rest of the scene graph.
//   - drawImageMesh is stubbed; .riv files using skinned / warped
//     images will have those regions render blank.

#include "rive_painter_factory.h"

#include <QElapsedTimer>
#include <QQuickPaintedItem>
#include <QString>
#include <QUrl>
#include <QtQmlIntegration/qqmlintegration.h>

#include <memory>

namespace rive {
class Artboard;
class ArtboardInstance;
class File;
class StateMachineInstance;
}

class RiveView : public QQuickPaintedItem
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
        Contain,  // rive::Fit::contain
        Cover,    // rive::Fit::cover
        Fill,     // rive::Fit::fill
        None,     // rive::Fit::none
        ScaleDown // rive::Fit::scaleDown
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

    // QQuickPaintedItem
    void paint(QPainter* painter) override;

signals:
    void sourceChanged();
    void fitChanged();
    void playingChanged();

    // Emitted when the .riv file fails to load. QML can bind a toast to this.
    void loadFailed(const QString& reason);

protected:
    void itemChange(ItemChange change, const ItemChangeData& data) override;

private slots:
    void advanceFrame();

private:
    void reload();
    void resetAnimationClock();

    RivePainterFactory m_factory;

    QUrl m_source;
    Fit m_fit = Fit::Contain;
    bool m_playing = true;

    // True once rive's advance* reports "nothing more to animate" — state
    // machine has no pending transitions and no animations are active.
    // Gates out the per-vsync advance/repaint loop so a static final frame
    // doesn't burn CPU redrawing itself. Reset on source/playing changes.
    bool m_settled = false;

    // Held in unique_ptr because rive types aren't copyable and the file
    // owns the artboard's source data.
    std::unique_ptr<rive::File> m_file;
    std::unique_ptr<rive::ArtboardInstance> m_artboard;
    std::unique_ptr<rive::StateMachineInstance> m_stateMachine;

    // Wall-clock tracking for the frame advance. Rive's advance() takes a
    // delta in seconds since the last advance.
    QElapsedTimer m_frameTimer;
    qint64 m_lastAdvanceNs = 0;
};

#endif // RIVE_VIEW_H
