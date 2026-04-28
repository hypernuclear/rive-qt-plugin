#ifndef RIVE_VM_PROPERTY_H
#define RIVE_VM_PROPERTY_H

// View-model typed property wrappers.
//
// One QObject subclass per rive ViewModelInstanceValue type we
// support. Each holds a raw pointer to the rive value (borrowed from
// the parent ViewModelInstance) and exposes its underlying primitive
// as a Q_PROPERTY for QML binding plus a valueChanged / triggered
// signal that fires when polling detects a change.
//
// Creation: only via RiveViewModelInstance::number/boolean/etc., which
// caches per-name. Don't construct directly.
//
// Phase 2b scope: number, boolean, string, color, enum, trigger.
// Lists, image properties, and nested view-model references are
// tracked as follow-up work.

#include <QColor>
#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QtQmlIntegration/qqmlintegration.h>

class QImage;
class RiveViewModelInstance;
class RiveFile;

namespace rive {
class Factory;
class File;
class ViewModelInstanceArtboard;
class ViewModelInstanceAssetImage;
class ViewModelInstanceBoolean;
class ViewModelInstanceColor;
class ViewModelInstanceEnum;
class ViewModelInstanceList;
class ViewModelInstanceNumber;
class ViewModelInstanceString;
class ViewModelInstanceTrigger;
class ViewModelInstanceValue;
class ViewModelInstanceViewModel;
}

class RiveVMProperty : public QObject
{
    Q_OBJECT
    QML_NAMED_ELEMENT(RiveVMProperty)
    QML_UNCREATABLE("Acquired via RiveViewModelInstance")
    Q_PROPERTY(QString name READ name CONSTANT)
    Q_PROPERTY(Type type READ type CONSTANT)

public:
    enum class Type
    {
        Number,
        Boolean,
        String,
        Color,
        Enum,
        Trigger,
        ViewModel, // nested VM instance
        List,      // ordered collection of nested VM instances
        Image,     // ImageAsset reference
        Artboard,  // BindableArtboard reference
        Unknown
    };
    Q_ENUM(Type)

    RiveVMProperty(QString name, QObject* parent);

    QString name() const { return m_name; }
    virtual Type type() const = 0;

    // Compares the wrapper's cached value to the underlying rive value
    // and emits the appropriate signal if they differ. Called once per
    // frame by the parent ViewModelInstance.
    virtual void poll() = 0;

protected:
    QString m_name;
};

class RiveVMNumberProperty : public RiveVMProperty
{
    Q_OBJECT
    QML_NAMED_ELEMENT(RiveVMNumberProperty)
    QML_UNCREATABLE("Acquired via RiveViewModelInstance")
    Q_PROPERTY(double value READ value WRITE setValue NOTIFY valueChanged)

public:
    RiveVMNumberProperty(QString name, rive::ViewModelInstanceNumber* v, QObject* parent);

    Type type() const override { return Type::Number; }
    void poll() override;

    double value() const;
    void setValue(double v);

signals:
    void valueChanged();

private:
    rive::ViewModelInstanceNumber* m_v;
    float m_cached;
};

class RiveVMBooleanProperty : public RiveVMProperty
{
    Q_OBJECT
    QML_NAMED_ELEMENT(RiveVMBooleanProperty)
    QML_UNCREATABLE("Acquired via RiveViewModelInstance")
    Q_PROPERTY(bool value READ value WRITE setValue NOTIFY valueChanged)

public:
    RiveVMBooleanProperty(QString name, rive::ViewModelInstanceBoolean* v, QObject* parent);

    Type type() const override { return Type::Boolean; }
    void poll() override;

    bool value() const;
    void setValue(bool v);

signals:
    void valueChanged();

private:
    rive::ViewModelInstanceBoolean* m_v;
    bool m_cached;
};

class RiveVMStringProperty : public RiveVMProperty
{
    Q_OBJECT
    QML_NAMED_ELEMENT(RiveVMStringProperty)
    QML_UNCREATABLE("Acquired via RiveViewModelInstance")
    Q_PROPERTY(QString value READ value WRITE setValue NOTIFY valueChanged)

public:
    RiveVMStringProperty(QString name, rive::ViewModelInstanceString* v, QObject* parent);

    Type type() const override { return Type::String; }
    void poll() override;

    QString value() const;
    void setValue(const QString& v);

signals:
    void valueChanged();

private:
    rive::ViewModelInstanceString* m_v;
    QString m_cached;
};

class RiveVMColorProperty : public RiveVMProperty
{
    Q_OBJECT
    QML_NAMED_ELEMENT(RiveVMColorProperty)
    QML_UNCREATABLE("Acquired via RiveViewModelInstance")
    Q_PROPERTY(QColor value READ value WRITE setValue NOTIFY valueChanged)

public:
    RiveVMColorProperty(QString name, rive::ViewModelInstanceColor* v, QObject* parent);

    Type type() const override { return Type::Color; }
    void poll() override;

    QColor value() const;
    void setValue(const QColor& v);

signals:
    void valueChanged();

private:
    rive::ViewModelInstanceColor* m_v;
    int m_cached; // packed ARGB int — same representation rive uses
};

class RiveVMEnumProperty : public RiveVMProperty
{
    Q_OBJECT
    QML_NAMED_ELEMENT(RiveVMEnumProperty)
    QML_UNCREATABLE("Acquired via RiveViewModelInstance")
    Q_PROPERTY(int valueIndex READ valueIndex WRITE setValueIndex NOTIFY valueChanged)
    Q_PROPERTY(QString valueName READ valueName WRITE setValueName NOTIFY valueChanged)
    Q_PROPERTY(QStringList values READ values CONSTANT)

public:
    RiveVMEnumProperty(QString name, rive::ViewModelInstanceEnum* v, QObject* parent);

    Type type() const override { return Type::Enum; }
    void poll() override;

    int valueIndex() const;
    void setValueIndex(int idx);

    QString valueName() const;
    void setValueName(const QString& v);

    // The enum's allowed labels, in declaration order. Reflects the
    // DataEnum the property was authored against.
    QStringList values() const { return m_values; }

signals:
    void valueChanged();

private:
    rive::ViewModelInstanceEnum* m_v;
    QStringList m_values;
    uint32_t m_cached;
};

class RiveVMTriggerProperty : public RiveVMProperty
{
    Q_OBJECT
    QML_NAMED_ELEMENT(RiveVMTriggerProperty)
    QML_UNCREATABLE("Acquired via RiveViewModelInstance")

public:
    RiveVMTriggerProperty(QString name, rive::ViewModelInstanceTrigger* v, QObject* parent);

    Type type() const override { return Type::Trigger; }
    void poll() override;

    /// Fires this trigger. The state machine observes the trigger on the
    /// next advance() and clears it after consumption. Multiple fire()
    /// calls between advances collapse to a single activation; reading
    /// any "is fired" state before the next advance is undefined.
    Q_INVOKABLE void fire();

signals:
    // Fires whenever the underlying counter ticks — both from our
    // own fire() and from rive-internal sources (e.g. an SM
    // transition setting the trigger). QML can bind to this directly.
    void triggered();

private:
    rive::ViewModelInstanceTrigger* m_v;
    uint32_t m_cached;
};

// Nested view model property. The `value` is a recursive
// RiveViewModelInstance — point QML at it to render another property
// panel inside the parent's. Owned by `this`.
class RiveVMNestedProperty : public RiveVMProperty
{
    Q_OBJECT
    QML_NAMED_ELEMENT(RiveVMNestedProperty)
    QML_UNCREATABLE("Acquired via RiveViewModelInstance")
    Q_PROPERTY(RiveViewModelInstance* value READ value NOTIFY valueChanged)

public:
    RiveVMNestedProperty(QString name, rive::ViewModelInstanceViewModel* v, QObject* parent);
    ~RiveVMNestedProperty() override;

    Type type() const override { return Type::ViewModel; }
    void poll() override;

    RiveViewModelInstance* value() const;

signals:
    void valueChanged();

private:
    void rebuildWrapper();

    rive::ViewModelInstanceViewModel* m_v;
    // The wrapped nested instance. Owned via Qt parent (this), so it
    // dies with us. Recreated when the underlying rcp identity
    // changes (rive can swap a nested VM at runtime).
    RiveViewModelInstance* m_wrapper = nullptr;
    // Cache the native pointer so we can detect identity changes
    // cheaply.
    void* m_cachedRaw = nullptr;
};

// Forward declare so the list property can return RiveViewModelInstance*.
class RiveViewModelInstance;

// List of nested view-model instances. count + itemAt(i) read access,
// plus mutation (removeAt / swap / clear). "Append new" requires the
// element-VM type which isn't trivially exposed by rive's runtime; we
// leave that to a follow-up.
class RiveVMListProperty : public RiveVMProperty
{
    Q_OBJECT
    QML_NAMED_ELEMENT(RiveVMListProperty)
    QML_UNCREATABLE("Acquired via RiveViewModelInstance")
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    RiveVMListProperty(QString name, rive::ViewModelInstanceList* v, QObject* parent);
    ~RiveVMListProperty() override;

    Type type() const override { return Type::List; }
    void poll() override;

    int count() const;

    // Item access. Wrappers are cached per-index, dropped when the
    // list is mutated. Returns nullptr if index is out of range.
    Q_INVOKABLE RiveViewModelInstance* itemAt(int index);

    Q_INVOKABLE void removeAt(int index);
    Q_INVOKABLE void swap(int a, int b);
    Q_INVOKABLE void clear();

signals:
    void countChanged();

private:
    void invalidateWrapperCache();

    rive::ViewModelInstanceList* m_v;
    int m_cachedCount = 0;
    // index -> wrapper. Wrappers are children of `this` so they go
    // away when the property does. Cleared on any mutation since
    // indices shift.
    QHash<int, RiveViewModelInstance*> m_itemWrappers;
};

// Image asset property. Setter only — accepts a QUrl (file/qrc) or a
// QImage; decodes via QImage and pushes the resulting RenderImage to
// rive. No getter today: the path back from rive::ImageAsset to a
// QImage isn't on rive's public API.
class RiveVMImageProperty : public RiveVMProperty
{
    Q_OBJECT
    QML_NAMED_ELEMENT(RiveVMImageProperty)
    QML_UNCREATABLE("Acquired via RiveViewModelInstance")

public:
    RiveVMImageProperty(QString name, rive::ViewModelInstanceAssetImage* v,
                        rive::Factory* factory, QObject* parent);

    Type type() const override { return Type::Image; }
    void poll() override {}

    Q_INVOKABLE bool setSource(const QUrl& url);
    bool setImage(const QImage& image);

signals:
    void valueChanged();

private:
    rive::ViewModelInstanceAssetImage* m_v;
    rive::Factory* m_factory; // borrowed; we use it to mint RenderImages
};

// Artboard reference. Value is a string (the artboard's name in the
// .riv file). Setter resolves via the file's bindableArtboardNamed().
class RiveVMArtboardProperty : public RiveVMProperty
{
    Q_OBJECT
    QML_NAMED_ELEMENT(RiveVMArtboardProperty)
    QML_UNCREATABLE("Acquired via RiveViewModelInstance")
    Q_PROPERTY(QString value READ value WRITE setValue NOTIFY valueChanged)

public:
    RiveVMArtboardProperty(QString name, rive::ViewModelInstanceArtboard* v,
                           rive::File* file, QObject* parent);

    Type type() const override { return Type::Artboard; }
    void poll() override;

    QString value() const;
    void setValue(const QString& artboardName);

signals:
    void valueChanged();

private:
    rive::ViewModelInstanceArtboard* m_v;
    rive::File* m_file;
    // Cached identity of the bound BindableArtboard so we can detect
    // rive-side changes during poll() and emit valueChanged.
    void* m_cachedRaw = nullptr;
};

#endif // RIVE_VM_PROPERTY_H
