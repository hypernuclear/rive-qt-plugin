#ifndef RIVE_STATE_MACHINE_H
#define RIVE_STATE_MACHINE_H

// RiveStateMachine — QObject wrapper around rive::StateMachineInstance.
//
// One of these is created per (artboard, SM name) pair, and lives under
// the RiveArtboard. Exposed to QML so user code can:
//   - inject pointer events (forwarded by RiveView by default, or manual)
//   - react to state transitions
//   - bind a view-model instance that drives transition guards
//
// `advance()` is NOT Q_INVOKABLE — RiveView drives frame stepping. Every
// frame it queries `stateChangedByIndex` into stateChanged signals.
//
// What's intentionally NOT here:
//   - Legacy SM Inputs (Boolean / Number / Trigger). Rive deprecated them
//     in favor of Data Binding. Use RiveViewModelInstance::number /
//     boolean / trigger / etc. on the bound view model instead.
//   - Legacy Rive Events (runtime → host signals). Same story — use VM
//     trigger properties' `triggered()` signal for runtime → host
//     notifications.
//
// stateChanged() reports the TIMELINE name of the state that was entered (a
// LayerState has no name of its own; see drainStateChanges). States with no
// single timeline — entry / exit / any / blend — report an empty name.

#include <QObject>
#include <QPointF>

#include <memory>

#include <rive/refcnt.hpp>

namespace rive {
class StateMachineInstance;
class ViewModelInstance;
}

class RiveStateMachine : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString name READ name CONSTANT)
    // Playhead position within the state's timeline the machine is
    // CURRENTLY advancing, in seconds. Both 0 while nothing is playing
    // (entry / exit / blend / settled). Read-only — a state machine is a
    // graph, not a scrubbable timeline; these exist so a host progress
    // bar can measure playback, not drive it.
    Q_PROPERTY(qreal currentStateTime READ currentStateTime NOTIFY currentStateTimeChanged)
    Q_PROPERTY(QString currentStateTimeline READ currentStateTimeline NOTIFY currentStateTimelineChanged)

public:
    // Mirrors rive::HitResult so QML can branch on pointer outcomes.
    enum class HitResult
    {
        None,
        Hit,
        HitOpaque
    };
    Q_ENUM(HitResult)

    RiveStateMachine(std::unique_ptr<rive::StateMachineInstance> instance,
                     QObject* parent);
    ~RiveStateMachine() override;

    QString name() const;

    // For RiveView's render path. Non-owning.
    rive::StateMachineInstance* raw() const;

    // Pointer events. Coordinates are in artboard-local space. See
    // RiveView for the screen→artboard transform that the default
    // forwarder uses. HitResult tells the caller whether the event
    // landed on a listener (Hit) or a modal/opaque listener (HitOpaque
    // — don't propagate further).
    // Pointer ID lets multiple concurrent touches stay distinct.
    // Defaults to 0 so single-mouse callers don't have to think
    // about it.
    Q_INVOKABLE HitResult pointerDown(const QPointF& pos, int pointerId = 0);
    Q_INVOKABLE HitResult pointerMove(const QPointF& pos, int pointerId = 0);
    Q_INVOKABLE HitResult pointerUp(const QPointF& pos, int pointerId = 0);
    Q_INVOKABLE HitResult pointerExit(const QPointF& pos, int pointerId = 0);

    // Framework-only: step the machine. Returns true if the SM reports
    // more work pending (keep requesting frames). Emits stateChanged
    // for anything raised during the step.
    bool advance(float deltaSeconds);

    // Bind a view-model instance — drives any data-bound transitions
    // authored in the editor against this state machine.
    void bindViewModelInstance(rive::rcp<rive::ViewModelInstance> instance);

    qreal currentStateTime() const { return m_currentStateTime; }
    QString currentStateTimeline() const { return m_currentStateTimeline; }

signals:
    void stateChanged(const QString& layerName, const QString& stateName);
    void currentStateTimeChanged();
    void currentStateTimelineChanged();

private:
    void drainStateChanges();
    // Refresh the current-state playhead accessors from the machine's
    // active animation; emits the change signals. Called from advance().
    void publishCurrentState();

    std::unique_ptr<rive::StateMachineInstance> m_sm;
    qreal m_currentStateTime = 0.0;
    QString m_currentStateTimeline;
};

#endif // RIVE_STATE_MACHINE_H
