#ifndef RIVE_STATE_MACHINE_H
#define RIVE_STATE_MACHINE_H

// RiveStateMachine — QObject wrapper around rive::StateMachineInstance.
//
// One of these is created per (artboard, SM name) pair, and lives under
// the RiveArtboard. Exposed to QML so user code can:
//   - access inputs as typed sub-objects: getBool/getNumber/getTrigger(name)
//   - inject pointer events (forwarded by RiveView by default, or manual)
//   - react to state transitions
//
// `advance()` is NOT Q_INVOKABLE — RiveView drives frame stepping. Every
// frame it also queries `stateChangedByIndex` into stateChanged signals.
//
// The legacy Rive Events system (runtime → host signals) is intentionally
// not wrapped — Rive itself has deprecated it in favor of Data Binding.
// For runtime → host notifications, use VM trigger properties via
// RiveViewModelInstance::trigger(name) and the typed wrapper's
// `triggered()` signal.

#include "rive_input.h"

#include <QHash>
#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QQmlPropertyMap>
#include <QStringList>

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
    // Dynamic name → value bag for state-machine inputs. Eager-populated
    // at construction so QML can `Object.keys(sm.inputs)` for discovery
    // and `sm.inputs.speed = 0.4` for assignment. Triggers appear in the
    // map for discoverability but cannot be fired through it — call
    // `sm.getTrigger("...").fire()` instead.
    Q_PROPERTY(QQmlPropertyMap* inputs READ inputs CONSTANT)
    Q_PROPERTY(QStringList inputNames READ inputNames CONSTANT)

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

    QQmlPropertyMap* inputs() const { return m_inputs; }
    QStringList inputNames() const { return m_inputCache.keys(); }

    // For RiveView's render path. Non-owning.
    rive::StateMachineInstance* raw() const;

    // Typed input access. Returns nullptr if no input exists with the
    // given name OR the input exists with a different type. Returned
    // pointer is owned by this state machine (parented here) — don't
    // delete. Same name always returns the same instance.
    Q_INVOKABLE RiveBoolInput* getBool(const QString& name);
    Q_INVOKABLE RiveNumberInput* getNumber(const QString& name);
    Q_INVOKABLE RiveTriggerInput* getTrigger(const QString& name);

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

signals:
    void stateChanged(const QString& layerName, const QString& stateName);

private:
    void drainStateChanges();
    void buildInputsMap();
    // Re-entrancy guard: when a typed wrapper's valueChanged fires we
    // write the new value back into m_inputs, which fires the map's
    // valueChanged, which would loop us back into the wrapper's setter.
    // Set true while we touch the map from the wrapper side.
    bool m_inputMapGuard = false;

    std::unique_ptr<rive::StateMachineInstance> m_sm;

    // Input wrapper cache keyed by name. Dedup so bindings stay stable.
    // QPointer so the cache entry auto-nulls if an input is deleted out
    // of band (shouldn't happen, but defensive).
    QHash<QString, QPointer<RiveInput>> m_inputCache;

    // Dynamic property bag exposed to QML — see the inputs Q_PROPERTY.
    // Parented to `this`; populated in buildInputsMap().
    QQmlPropertyMap* m_inputs = nullptr;
};

#endif // RIVE_STATE_MACHINE_H
