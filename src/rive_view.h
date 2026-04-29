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

namespace rive {
class LinearAnimationInstance;
}

class RiveView : public QQuickItem
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QUrl source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(QString artboard READ artboard WRITE setArtboard NOTIFY artboardChanged)
    Q_PROPERTY(QString stateMachineName READ stateMachineName WRITE setStateMachineName
                   NOTIFY stateMachineNameChanged)
    // Linear animation playback. Used as a fallback when the artboard
    // has no state machine (matches rive's web player). Empty name picks
    // the first animation. Ignored if a state machine is active.
    Q_PROPERTY(QString animationName READ animationName WRITE setAnimationName
                   NOTIFY animationNameChanged)
    Q_PROPERTY(QStringList animationNames READ animationNames NOTIFY animationNamesChanged)
    Q_PROPERTY(Fit fit READ fit WRITE setFit NOTIFY fitChanged)
    Q_PROPERTY(bool playing READ isPlaying WRITE setPlaying NOTIFY playingChanged)
    // Multiplier on the per-frame deltaSeconds passed to advance().
    // 1.0 = real time, 0.5 = half speed, 2.0 = double speed, 0.0 =
    // freeze (advance() called with delta=0; the SM stays in its
    // current state). Negative values clamp to 0.
    Q_PROPERTY(qreal speed READ speed WRITE setSpeed NOTIFY speedChanged)
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
    // Master switch for auto-binding the artboard's editor-default
    // view-model. Defaults to true — preserves the historic behavior
    // where an empty `viewModelName` follows whatever the editor wired
    // up. Set to false to suppress binding entirely (useful when an
    // artboard ships with multiple VMs and the host wants to pick one
    // imperatively via bindViewModelInstance(name) later).
    Q_PROPERTY(bool autoBindViewModel READ autoBindViewModel WRITE setAutoBindViewModel
                   NOTIFY autoBindViewModelChanged)
    Q_PROPERTY(RiveViewModelInstance* viewModel READ viewModel NOTIFY viewModelChanged)
    // Active state machine, exposed as a notifying property so QML can
    // bind to it and react when the SM is rebuilt (artboard or
    // stateMachineName change).
    Q_PROPERTY(RiveStateMachine* stateMachine READ stateMachine NOTIFY stateMachineChanged)
    // Drive the artboard's runtime layout. Invalid (default) = use the
    // editor-authored design-time size. Setting a valid QSizeF drives
    // the artboard size and re-runs layout. Useful for responsive
    // artwork authored with rive's layout system.
    Q_PROPERTY(QSizeF layoutSize READ layoutSize WRITE setLayoutSize
                   NOTIFY layoutSizeChanged)
    Q_PROPERTY(Alignment alignment READ alignment WRITE setAlignment
                   NOTIFY alignmentChanged)

public:
    // Mirrors rive::Fit. `Layout` defers entirely to Rive's responsive
    // layout system — pair with `layoutSize` to drive the artboard's
    // runtime width/height directly.
    enum class Fit
    {
        Fill,
        Contain,
        Cover,
        FitWidth,
        FitHeight,
        None,
        ScaleDown,
        Layout
    };
    Q_ENUM(Fit)

    // 9 named alignment positions matching rive's Alignment statics.
    // Default Center preserves the historic behavior.
    enum class Alignment
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
    Q_ENUM(Alignment)

    explicit RiveView(QQuickItem* parent = nullptr);
    ~RiveView() override;

    QUrl source() const { return m_source; }
    void setSource(const QUrl& url);

    QString artboard() const { return m_artboardName; }
    void setArtboard(const QString& name);

    QString stateMachineName() const { return m_stateMachineName; }
    void setStateMachineName(const QString& name);

    QString animationName() const { return m_animationName; }
    void setAnimationName(const QString& name);
    QStringList animationNames() const;

    Fit fit() const { return m_fit; }
    void setFit(Fit f);

    bool isPlaying() const { return m_playing; }
    void setPlaying(bool playing);

    qreal speed() const { return m_speed; }
    void setSpeed(qreal s);

    bool inputForwarding() const { return m_inputForwarding; }
    void setInputForwarding(bool b);

    QStringList artboardNames() const;
    QStringList stateMachineNames() const;
    QStringList viewModelNames() const;

    QString viewModelName() const { return m_viewModelName; }
    void setViewModelName(const QString& name);

    QString viewModelInstanceName() const { return m_viewModelInstanceName; }
    void setViewModelInstanceName(const QString& name);

    bool autoBindViewModel() const { return m_autoBindViewModel; }
    void setAutoBindViewModel(bool b);

    // Imperative bind by instance name. Re-enables autoBindViewModel if
    // it was off, then forwards to setViewModelInstanceName(name).
    // Reads better than the property setter in onCompleted handlers.
    // Returns true if a load was scheduled (file present); the actual
    // bind result is observable via the `viewModel` property.
    Q_INVOKABLE bool bindViewModelInstance(const QString& name);

    RiveViewModelInstance* viewModel() const { return m_viewModel.get(); }

    QSizeF layoutSize() const { return m_layoutSize; }
    void setLayoutSize(const QSizeF& size);

    Alignment alignment() const { return m_alignment; }
    void setAlignment(Alignment a);

    // Active state machine for QML binding. Returns nullptr before load
    // completes or if the named SM doesn't exist. The pointer is stable
    // until the SM is swapped (new artboard or stateMachineName).
    RiveStateMachine* stateMachine() const { return m_stateMachine.get(); }

    // QQuickItem
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) override;

signals:
    void sourceChanged();
    void artboardChanged();
    void stateMachineNameChanged();
    void animationNameChanged();
    void animationNamesChanged();
    void fitChanged();
    void playingChanged();
    void speedChanged();
    void inputForwardingChanged();
    void artboardNamesChanged();
    void stateMachineNamesChanged();
    void viewModelNamesChanged();
    void viewModelNameChanged();
    void viewModelInstanceNameChanged();
    void autoBindViewModelChanged();
    void viewModelChanged();
    void stateMachineChanged();
    void layoutSizeChanged();
    void alignmentChanged();

    void loadFailed(const QString& reason);
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
    // Build a LinearAnimationInstance for the current animationName.
    // Only invoked when there's no SM — playback fallback path. Resets
    // any existing animation. Idempotent.
    void rebuildAnimation();
    void rebuildViewModel();    // called after artboard/SM rebuild or VM-name prop change
    QPointF mapToArtboard(const QPointF& localPos) const;
    void dispatchPointer(QEvent::Type type, const QPointF& localPos, int pointerId = 0);

    QUrl m_source;
    QString m_artboardName;          // "" = default
    QString m_stateMachineName;      // "" = default
    QString m_animationName;         // "" = first animation (fallback path)
    QString m_viewModelName;         // "" = follow the artboard's editor binding
    QString m_viewModelInstanceName; // "" = blank instance / default preset
    QSizeF m_layoutSize;             // invalid = use design-time size
    Fit m_fit = Fit::Contain;
    Alignment m_alignment = Alignment::Center;
    qreal m_speed = 1.0;
    bool m_playing = true;
    bool m_inputForwarding = true;
    bool m_autoBindViewModel = true;
    bool m_settled = false;
    bool m_loadRequested = false;

    QElapsedTimer m_frameTimer;
    qint64 m_lastAdvanceNs = 0;

    // Drag detection (mouse only). When a mouse button is pressed we
    // capture the position and mark `pending`. Once the cursor moves
    // beyond Qt's startDragDistance threshold we fire dragStart on the
    // SM and switch to `started`. mouse-release fires dragEnd if the
    // drag had started. Touch drags aren't currently mapped — the user
    // would call dragStart/dragEnd via the runtime directly.
    QPointF m_dragStartPos;
    bool m_dragPending = false;
    bool m_dragStarted = false;

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
    QString m_loadedAnimationName;

    // Created on the render thread, so we can't use Qt parenting to
    // RiveView (GUI thread) — Qt refuses cross-thread setParent. Own
    // via unique_ptr and let QObject destructors handle teardown.
    //
    // Declaration order matters: m_stateMachine must come AFTER
    // m_artboard so the unique_ptr destructors run in reverse order
    // and the SM dies before the artboard. The SM's underlying
    // rive::StateMachineInstance touches its owning rive::Artboard
    // during destruction (cleanupFocusTree).
    std::unique_ptr<RiveArtboard> m_artboard;
    // Owning. The SM is parent-less (see RiveArtboard::createStateMachine)
    // and moveToThread'd to RiveView's (GUI) thread so its
    // QQmlPropertyMap inputs map is reachable from QML.
    std::unique_ptr<RiveStateMachine> m_stateMachine;

    // Animation fallback (used only when m_stateMachine is null). Owned
    // by RiveView; outlives the artboard only as long as we hold the
    // artboard, since LinearAnimationInstance references it. Declared
    // after m_stateMachine and before m_artboard's reverse-order dtor —
    // but we tear it down explicitly whenever the artboard changes, so
    // the order here only matters for the final RiveView destructor.
    std::unique_ptr<rive::LinearAnimationInstance> m_animation;

    // View model: own as unique_ptr (created on render thread, never
    // re-parented across threads). RiveView holds the canonical
    // pointer; QML reads via the `viewModel` Q_PROPERTY.
    std::unique_ptr<RiveViewModelInstance> m_viewModel;
    QString m_loadedViewModelName;
    QString m_loadedViewModelInstanceName;
};

#endif // RIVE_VIEW_H
