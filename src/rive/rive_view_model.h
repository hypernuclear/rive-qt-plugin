#ifndef RIVE_VIEW_MODEL_H
#define RIVE_VIEW_MODEL_H

// View-model wrappers. Two layers:
//
//  - RiveViewModel: definition handle. Reflects metadata authored in
//    the Rive editor (property names, instance preset names). Read-
//    only, lightweight, lifetime tied to RiveFile.
//
//  - RiveViewModelInstance: per-instance live state. QObject so QML
//    can bind to its property children. Created via RiveView's
//    factory methods; bound to an artboard / state machine to drive
//    visual properties via rive's data-bind system.
//
// Change notification: rive's runtime build doesn't expose per-
// property callbacks, so the wrapper polls. Each frame, after the
// state machine advances, RiveView calls
// RiveViewModelInstance::advance(), which:
//   1. invokes rive::ViewModelInstance::advanced() so internal
//      delegates fire,
//   2. iterates every cached typed property wrapper and checks
//      whether its underlying value changed. If yes, the wrapper
//      emits its valueChanged / triggered signal.
//
// Per-pointer dedup cache: a single rive::ViewModelInstance* maps to
// exactly one RiveViewModelInstance, regardless of how many code
// paths request it. Same pattern Unity adopted; prevents double
// signal emission and stale wrappers.

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QQmlPropertyMap>
#include <QString>
#include <QStringList>
#include <QtQmlIntegration/qqmlintegration.h>

// rcp<ViewModelInstance> as a member needs the full type at the point
// where the rcp's destructor is instantiated, which happens in MOC-
// generated TUs that include this header. Pull the full rive header
// in here rather than push the cost onto every consumer.
#include <rive/refcnt.hpp>
#include <rive/viewmodel/viewmodel_instance.hpp>

// MOC registers pointer meta-types for the Q_INVOKABLE returns below;
// it needs the full property declarations to do that, so include the
// header rather than forward-declaring.
#include "rive_vm_property.h"

namespace rive {
class File;
class ViewModel;
}

// ---------------------------------------------------------------------------
// RiveViewModel — definition / metadata only. Doesn't own anything; the
// underlying rive::ViewModel* is owned by rive::File.
// ---------------------------------------------------------------------------
class RiveViewModel : public QObject
{
    Q_OBJECT
    QML_NAMED_ELEMENT(RiveViewModel)
    QML_UNCREATABLE("Discovered via RiveView")
    Q_PROPERTY(QString name READ name CONSTANT)
    Q_PROPERTY(QStringList propertyNames READ propertyNames CONSTANT)
    Q_PROPERTY(QStringList instanceNames READ instanceNames CONSTANT)
    Q_PROPERTY(QString defaultInstanceName READ defaultInstanceName CONSTANT)

public:
    RiveViewModel(rive::ViewModel* vm, QObject* parent = nullptr);

    QString name() const;
    QStringList propertyNames() const;
    // Names of instance presets authored in the editor.
    QStringList instanceNames() const;
    QString defaultInstanceName() const;

    rive::ViewModel* raw() const { return m_vm; }

private:
    rive::ViewModel* m_vm; // borrowed; lifetime tied to rive::File
};

// ---------------------------------------------------------------------------
// RiveViewModelInstance — live property bag bound to an artboard / SM.
// ---------------------------------------------------------------------------
class RiveViewModelInstance : public QObject
{
    Q_OBJECT
    QML_NAMED_ELEMENT(RiveViewModelInstance)
    QML_UNCREATABLE("Created via RiveView")
    Q_PROPERTY(QStringList propertyNames READ propertyNames CONSTANT)
    // Dynamic name → value bag. Eager-populated at construction so QML
    // can `Object.keys(vmi.props)` and bind declaratively via
    // `Text { text: vmi.props.title }` / `vmi.props.title = "..."`.
    // Only primitive-valued properties (number / boolean / string /
    // color / enum) participate. Triggers appear with a sentinel for
    // discoverability but cannot be fired through the map — call
    // `vmi.trigger("...").fire()`. Lists / nested VMs / image assets /
    // artboard refs stay typed-only via the explicit accessors.
    Q_PROPERTY(QQmlPropertyMap* props READ props CONSTANT)

public:
    RiveViewModelInstance(rive::rcp<rive::ViewModelInstance> instance,
                          QObject* parent = nullptr);
    // Variant that records a factory + file pointer — needed by the
    // image-asset and artboard-reference property types. The root VM
    // instance gets these from RiveView's backend; nested VMs inherit
    // from their parent.
    RiveViewModelInstance(rive::rcp<rive::ViewModelInstance> instance,
                          rive::Factory* factory, rive::File* file,
                          QObject* parent = nullptr);
    ~RiveViewModelInstance() override;

    QStringList propertyNames() const;

    QQmlPropertyMap* props() const { return m_props; }

    // Typed accessors. Return nullptr if the property doesn't exist or
    // exists with a different type. Cached per-name so repeated calls
    // return the same QObject (stable bindings).
    Q_INVOKABLE RiveVMNumberProperty* number(const QString& name);
    Q_INVOKABLE RiveVMBooleanProperty* boolean(const QString& name);
    Q_INVOKABLE RiveVMStringProperty* string(const QString& name);
    Q_INVOKABLE RiveVMColorProperty* color(const QString& name);
    Q_INVOKABLE RiveVMEnumProperty* enumProperty(const QString& name);
    Q_INVOKABLE RiveVMTriggerProperty* trigger(const QString& name);
    Q_INVOKABLE RiveVMNestedProperty* viewModel(const QString& name);
    Q_INVOKABLE RiveVMListProperty* list(const QString& name);
    Q_INVOKABLE RiveVMImageProperty* image(const QString& name);
    Q_INVOKABLE RiveVMArtboardProperty* artboard(const QString& name);

    // Untyped accessor — returns whatever wrapper matches the property
    // type. Useful for generic enumeration. Caller must qobject_cast
    // to the typed subclass to read/write.
    Q_INVOKABLE RiveVMProperty* property(const QString& name);

    // Per-frame: fire rive's internal delegates and poll cached typed
    // wrappers for value changes. Called by RiveView from updatePaintNode.
    void advance();

    // Called by typed property setters when the user mutates a value.
    // Emits `propertyMutated` so RiveView can wake its advance loop —
    // a settled state machine otherwise wouldn't pick up the change.
    void notifyMutated();

signals:
    // Fires whenever any property on this instance has been written
    // to. RiveView observes this to clear `settled` and request an
    // update so data-bound visuals re-render immediately.
    void propertyMutated();

public:
    rive::ViewModelInstance* raw() const;
    rive::rcp<rive::ViewModelInstance> sharedRaw() const { return m_instance; }

    // Per-pointer dedup. Returns the wrapper for `instance`, creating
    // it on demand. The wrapper is parented to `parent` only on first
    // creation. On nullptr returns nullptr.
    static RiveViewModelInstance* wrap(rive::rcp<rive::ViewModelInstance> instance,
                                       QObject* parent);

private:
    template <typename T>
    T* lookupOrCreate(const QString& name);

    void buildPropsMap();

    // Re-entrancy guard for the props map ↔ typed-wrapper sync. See
    // RiveStateMachine::m_inputMapGuard for the same pattern.
    bool m_propsGuard = false;

    rive::rcp<rive::ViewModelInstance> m_instance;

    // Borrowed; provided by RiveView so the image / artboard-ref
    // properties can decode/resolve. May be nullptr if the instance
    // was constructed via the basic ctor (used for nested VMs created
    // by RiveVMNestedProperty — those inherit the factory/file from
    // their parent in a follow-up; for now their image/artboard
    // properties no-op).
    rive::Factory* m_factory = nullptr;
    rive::File* m_file = nullptr;

    // Cache keyed by property name. QPointer so deleted children
    // auto-null (defensive — children share `this` as parent so they
    // die with us, but a stray deleteLater() shouldn't break us).
    QHash<QString, QPointer<RiveVMProperty>> m_propertyCache;

    // Dynamic property bag exposed to QML — see the props Q_PROPERTY.
    // Parented to `this`; populated by buildPropsMap() in the ctor.
    QQmlPropertyMap* m_props = nullptr;
};

#endif // RIVE_VIEW_MODEL_H
