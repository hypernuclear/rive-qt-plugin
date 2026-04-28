#ifndef RIVE_INPUT_H
#define RIVE_INPUT_H

// RiveInput — typed wrappers for state-machine inputs.
//
// Rive exposes three input kinds at runtime (bool / number / trigger).
// We mirror that with three QObject subclasses so QML can bind to
// `.value` for bool/number and call `.fire()` on trigger without
// runtime type checks.
//
// Ownership: the underlying rive::SMI* is borrowed from the parent
// StateMachineInstance. RiveInput is parented to its owning
// RiveStateMachine so Qt tears them down together — any QML binding
// via QPointer auto-nulls on destruction.
//
// Inputs are cached per-name inside RiveStateMachine so repeated
// getBool("foo") calls return the same QObject. This makes QML
// bindings stable across frames.

#include <QObject>
#include <QString>

namespace rive {
class SMIBool;
class SMINumber;
class SMITrigger;
}

class RiveInput : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString name READ name CONSTANT)
    Q_PROPERTY(Type type READ type CONSTANT)

public:
    enum class Type
    {
        Bool,
        Number,
        Trigger
    };
    Q_ENUM(Type)

    RiveInput(QString name, QObject* parent);

    QString name() const { return m_name; }
    virtual Type type() const = 0;

protected:
    QString m_name;
};

class RiveBoolInput : public RiveInput
{
    Q_OBJECT
    Q_PROPERTY(bool value READ value WRITE setValue NOTIFY valueChanged)

public:
    RiveBoolInput(QString name, rive::SMIBool* smi, QObject* parent);

    Type type() const override { return Type::Bool; }

    bool value() const;
    void setValue(bool v);

signals:
    void valueChanged();

private:
    rive::SMIBool* m_smi;
};

class RiveNumberInput : public RiveInput
{
    Q_OBJECT
    Q_PROPERTY(double value READ value WRITE setValue NOTIFY valueChanged)

public:
    RiveNumberInput(QString name, rive::SMINumber* smi, QObject* parent);

    Type type() const override { return Type::Number; }

    double value() const;
    void setValue(double v);

signals:
    void valueChanged();

private:
    rive::SMINumber* m_smi;
};

class RiveTriggerInput : public RiveInput
{
    Q_OBJECT

public:
    RiveTriggerInput(QString name, rive::SMITrigger* smi, QObject* parent);

    Type type() const override { return Type::Trigger; }

    /// Fires this trigger. The state machine observes the trigger on the
    /// next advance() and clears it after consumption. Multiple fire()
    /// calls between advances collapse to a single activation; reading
    /// any "is fired" state before the next advance is undefined.
    Q_INVOKABLE void fire();

private:
    rive::SMITrigger* m_smi;
};

#endif // RIVE_INPUT_H
