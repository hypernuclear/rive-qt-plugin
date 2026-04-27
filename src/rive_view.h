#ifndef RIVE_VIEW_H
#define RIVE_VIEW_H

// RiveView — QML-facing item that plays a .riv file.
//
// Splits responsibilities across three collaborators:
//
//  - A `RiveRenderBackend` (picked per RHI; only Metal today) owns the
//    graphics device interop and paints the artboard into a QSGTexture.
//  - A `RiveFile` (cached by URL, shared across views) owns the
//    decoded .riv data and mints artboard instances.
//  - A `RiveArtboard` + `RiveStateMachine` (child QObjects) own the
//    rive-side animation / state-machine instances and expose them to
//    QML for binding.
//
// Built-in input forwarding is on by default: mouse events get
// transformed from item-DIP coords to artboard coords via the fit
// matrix and dispatched to the active state machine. Keyboard events
// feed the artboard's FocusManager when the item has focus — see the
// follow-up commit wiring that up. Set `inputForwarding: false` from
// QML to handle input manually (e.g. to plug in a touch/XR source).

#include "rive/rive_artboard.h"
#include "rive/rive_event.h"
#include "rive/rive_state_machine.h"
#include "rive/rive_view_model.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QPointer>
#include <QQuickItem>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QtQmlIntegration/qqmlintegration.h>

#include <memory>

class RiveFile;
class RiveRenderBackend;
class QSGNode;

class RiveView : public QQuickItem
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QUrl source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(QString artboard READ artboard WRITE setArtboard NOTIFY artboardChanged)
    Q_PROPERTY(QString stateMachineName READ stateMachineName WRITE setStateMachineName
                   NOTIFY stateMachineNameChanged)
    Q_PROPERTY(Fit fit READ fit WRITE setFit NOTIFY fitChanged)
    Q_PROPERTY(bool playing READ isPlaying WRITE setPlaying NOTIFY playingChanged)
    Q_PROPERTY(bool inputForwarding READ inputForwarding WRITE setInputForwarding
                   NOTIFY inputForwardingChanged)
    Q_PROPERTY(QStringList artboardNames READ artboardNames NOTIFY artboardNamesChanged)
    Q_PROPERTY(QStringList stateMachineNames READ stateMachineNames NOTIFY stateMachineNamesChanged)
    Q_PROPERTY(QStringList viewModelNames READ viewModelNames NOTIFY viewModelNamesChanged)
    // The bound view model (or null). Defaults to the artboard's
    // editor-attached default. Override via `viewModelName` /
    // `viewModelInstanceName` properties.
    Q_PROPERTY(QString viewModelName READ viewModelName WRITE setViewModelName
                   NOTIFY viewModelNameChanged)
    Q_PROPERTY(QString viewModelInstanceName READ viewModelInstanceName
                   WRITE setViewModelInstanceName NOTIFY viewModelInstanceNameChanged)
    Q_PROPERTY(RiveViewModelInstance* viewModel READ viewModel NOTIFY viewModelChanged)
    // Drive the artboard's runtime layout. Invalid (default) = use the
    // editor-authored design-time size. Setting a valid QSizeF drives
    // the artboard size and re-runs layout. Useful for responsive
    // artwork authored with rive's layout system.
    Q_PROPERTY(QSizeF layoutSize READ layoutSize WRITE setLayoutSize
                   NOTIFY layoutSizeChanged)

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

    QString artboard() const { return m_artboardName; }
    void setArtboard(const QString& name);

    QString stateMachineName() const { return m_stateMachineName; }
    void setStateMachineName(const QString& name);

    Fit fit() const { return m_fit; }
    void setFit(Fit f);

    bool isPlaying() const { return m_playing; }
    void setPlaying(bool playing);

    bool inputForwarding() const { return m_inputForwarding; }
    void setInputForwarding(bool b);

    QStringList artboardNames() const;
    QStringList stateMachineNames() const;
    QStringList viewModelNames() const;

    QString viewModelName() const { return m_viewModelName; }
    void setViewModelName(const QString& name);

    QString viewModelInstanceName() const { return m_viewModelInstanceName; }
    void setViewModelInstanceName(const QString& name);

    RiveViewModelInstance* viewModel() const { return m_viewModel.get(); }

    QSizeF layoutSize() const { return m_layoutSize; }
    void setLayoutSize(const QSizeF& size);

    // Active state machine for QML binding. Returns nullptr before load
    // completes or if the named SM doesn't exist. The pointer is stable
    // until the SM is swapped (new artboard or stateMachineName).
    Q_INVOKABLE RiveStateMachine* stateMachine() const { return m_stateMachine; }

    // QQuickItem
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) override;

signals:
    void sourceChanged();
    void artboardChanged();
    void stateMachineNameChanged();
    void fitChanged();
    void playingChanged();
    void inputForwardingChanged();
    void artboardNamesChanged();
    void stateMachineNamesChanged();
    void viewModelNamesChanged();
    void viewModelNameChanged();
    void viewModelInstanceNameChanged();
    void viewModelChanged();
    void layoutSizeChanged();

    void loadFailed(const QString& reason);
    void eventReported(const RiveEvent& event);
    void stateMachineStateChanged(const QString& layerName, const QString& stateName);

protected:
    void itemChange(ItemChange change, const ItemChangeData& data) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void hoverMoveEvent(QHoverEvent* event) override;
    void hoverLeaveEvent(QHoverEvent* event) override;
    void touchEvent(QTouchEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;

private slots:
    void onBeforeSynchronizing();
    void onSceneGraphInvalidated();

private:
    void requestLoad();
    void tryLoad();           // on render thread; creates artboard + SM
    void rebuildArtboard();   // called when `artboard` prop changes
    void rebuildStateMachine(); // called when `stateMachineName` prop changes
    void rebuildViewModel();    // called after artboard/SM rebuild or VM-name prop change
    QPointF mapToArtboard(const QPointF& localPos) const;
    void dispatchPointer(QEvent::Type type, const QPointF& localPos, int pointerId = 0);

    QUrl m_source;
    QString m_artboardName;          // "" = default
    QString m_stateMachineName;      // "" = default
    QString m_viewModelName;         // "" = follow the artboard's editor binding
    QString m_viewModelInstanceName; // "" = blank instance / default preset
    QSizeF m_layoutSize;             // invalid = use design-time size
    Fit m_fit = Fit::Contain;
    bool m_playing = true;
    bool m_inputForwarding = true;
    bool m_settled = false;
    bool m_loadRequested = false;

    QElapsedTimer m_frameTimer;
    qint64 m_lastAdvanceNs = 0;

    std::unique_ptr<RiveRenderBackend> m_backend;
    bool m_backendReady = false;

    std::shared_ptr<RiveFile> m_file;

    // The (url, artboard, sm) tuple the current m_file/m_artboard/
    // m_stateMachine were built for. tryLoad() compares these against
    // the public properties to decide which layer to rebuild — file,
    // artboard, or just SM — instead of conflating them.
    QUrl m_loadedUrl;
    QString m_loadedArtboardName;
    QString m_loadedStateMachineName;

    // Created on the render thread, so we can't use Qt parenting to
    // RiveView (GUI thread) — Qt refuses cross-thread setParent. Own
    // via unique_ptr and let QObject destructors handle teardown. The
    // active SM (if any) is Qt-parented to m_artboard, so m_stateMachine
    // QPointer auto-nulls when the artboard swaps.
    std::unique_ptr<RiveArtboard> m_artboard;
    QPointer<RiveStateMachine> m_stateMachine;

    // View model: own as unique_ptr (created on render thread, never
    // re-parented across threads). RiveView holds the canonical
    // pointer; QML reads via the `viewModel` Q_PROPERTY.
    std::unique_ptr<RiveViewModelInstance> m_viewModel;
    QString m_loadedViewModelName;
    QString m_loadedViewModelInstanceName;
};

#endif // RIVE_VIEW_H
