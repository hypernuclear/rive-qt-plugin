#ifndef RIVE_EVENT_H
#define RIVE_EVENT_H

// RiveEvent — Q_GADGET value type for events reported by a state machine.
//
// Rive's editor lets designers fire named events from animations and state
// transitions; the runtime collects them per-frame and hands them back as
// EventReport entries. We marshal each into one of these gadgets and emit
// it on RiveStateMachine::eventReported.
//
// Phase 1 scope: name + delay only. Event custom properties (number /
// bool / string) aren't exposed yet — the C++ API for iterating them sits
// behind a protected member, so we'll need a helper or fork a small getter
// when we wire them in. Tracked as a follow-up.

#include <QMetaType>
#include <QObject>
#include <QString>
#include <QVariantMap>

class RiveEvent
{
    Q_GADGET
    Q_PROPERTY(QString name MEMBER m_name)
    Q_PROPERTY(float delay MEMBER m_delay)
    Q_PROPERTY(QVariantMap properties MEMBER m_properties)

public:
    RiveEvent() = default;
    RiveEvent(QString name, float delay) : m_name(std::move(name)), m_delay(delay) {}

    QString m_name;
    float m_delay = 0.0f;
    // Reserved — will hold unpacked CustomProperty values once we wire them.
    QVariantMap m_properties;
};

Q_DECLARE_METATYPE(RiveEvent)

#endif // RIVE_EVENT_H
