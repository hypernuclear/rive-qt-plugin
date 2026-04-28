#include "rive_view_model.h"

#include "rive_vm_property.h"

#include <rive/viewmodel/viewmodel.hpp>
#include <rive/viewmodel/viewmodel_instance.hpp>
#include <rive/viewmodel/viewmodel_instance_artboard.hpp>
#include <rive/viewmodel/viewmodel_instance_asset_image.hpp>
#include <rive/viewmodel/viewmodel_instance_boolean.hpp>
#include <rive/viewmodel/viewmodel_instance_color.hpp>
#include <rive/viewmodel/viewmodel_instance_enum.hpp>
#include <rive/viewmodel/viewmodel_instance_list.hpp>
#include <rive/viewmodel/viewmodel_instance_number.hpp>
#include <rive/viewmodel/viewmodel_instance_string.hpp>
#include <rive/viewmodel/viewmodel_instance_trigger.hpp>
#include <rive/viewmodel/viewmodel_instance_value.hpp>
#include <rive/viewmodel/viewmodel_instance_viewmodel.hpp>
#include <rive/viewmodel/viewmodel_property.hpp>

// ---------------------------------------------------------------------------
// RiveViewModel
// ---------------------------------------------------------------------------

RiveViewModel::RiveViewModel(rive::ViewModel* vm, QObject* parent)
    : QObject(parent), m_vm(vm)
{}

QString RiveViewModel::name() const
{
    return m_vm ? QString::fromStdString(m_vm->name()) : QString{};
}

QStringList RiveViewModel::propertyNames() const
{
    QStringList out;
    if (!m_vm)
        return out;
    for (rive::ViewModelProperty* prop : m_vm->properties())
    {
        if (prop)
            out.append(QString::fromStdString(prop->name()));
    }
    return out;
}

QStringList RiveViewModel::instanceNames() const
{
    QStringList out;
    if (!m_vm)
        return out;
    const std::size_t n = m_vm->instanceCount();
    out.reserve(static_cast<int>(n));
    for (std::size_t i = 0; i < n; ++i)
    {
        if (rive::ViewModelInstance* inst = m_vm->instance(i))
            out.append(QString::fromStdString(inst->name()));
    }
    return out;
}

QString RiveViewModel::defaultInstanceName() const
{
    if (!m_vm)
        return {};
    rive::ViewModelInstance* def = m_vm->defaultInstance();
    return def ? QString::fromStdString(def->name()) : QString{};
}

// ---------------------------------------------------------------------------
// RiveViewModelInstance
// ---------------------------------------------------------------------------

RiveViewModelInstance::RiveViewModelInstance(rive::rcp<rive::ViewModelInstance> instance,
                                             QObject* parent)
    : QObject(parent), m_instance(std::move(instance)), m_props(new QQmlPropertyMap(this))
{
    buildPropsMap();
}

RiveViewModelInstance::RiveViewModelInstance(rive::rcp<rive::ViewModelInstance> instance,
                                             rive::Factory* factory, rive::File* file,
                                             QObject* parent)
    : QObject(parent), m_instance(std::move(instance)), m_factory(factory), m_file(file),
      m_props(new QQmlPropertyMap(this))
{
    buildPropsMap();
}

RiveViewModelInstance::~RiveViewModelInstance() = default;

rive::ViewModelInstance* RiveViewModelInstance::raw() const
{
    return m_instance.get();
}

QStringList RiveViewModelInstance::propertyNames() const
{
    QStringList out;
    if (!m_instance)
        return out;
    for (const rive::rcp<rive::ViewModelInstanceValue>& val : m_instance->propertyValues())
    {
        if (!val)
            continue;
        rive::ViewModelProperty* prop = val->viewModelProperty();
        if (prop)
            out.append(QString::fromStdString(prop->name()));
    }
    return out;
}

template <typename T>
T* RiveViewModelInstance::lookupOrCreate(const QString& name)
{
    if (auto cached = m_propertyCache.value(name))
        return qobject_cast<T*>(cached.data());
    return nullptr;
}

namespace {

// Resolve a property by name to its rive::ViewModelInstanceValue*, or
// nullptr. ViewModelInstance::propertyValue(std::string) handles the
// lookup; we just defensively reject missing instances and empty names.
rive::ViewModelInstanceValue* findValue(rive::ViewModelInstance* inst,
                                        const QString& name)
{
    if (!inst || name.isEmpty())
        return nullptr;
    return inst->propertyValue(name.toStdString());
}

} // namespace

RiveVMNumberProperty* RiveViewModelInstance::number(const QString& name)
{
    if (auto* p = lookupOrCreate<RiveVMNumberProperty>(name))
        return p;
    rive::ViewModelInstanceValue* v = findValue(m_instance.get(), name);
    if (!v || !v->is<rive::ViewModelInstanceNumber>())
        return nullptr;
    auto* prop = new RiveVMNumberProperty(
        name, v->as<rive::ViewModelInstanceNumber>(), this);
    m_propertyCache.insert(name, prop);
    return prop;
}

RiveVMBooleanProperty* RiveViewModelInstance::boolean(const QString& name)
{
    if (auto* p = lookupOrCreate<RiveVMBooleanProperty>(name))
        return p;
    rive::ViewModelInstanceValue* v = findValue(m_instance.get(), name);
    if (!v || !v->is<rive::ViewModelInstanceBoolean>())
        return nullptr;
    auto* prop = new RiveVMBooleanProperty(
        name, v->as<rive::ViewModelInstanceBoolean>(), this);
    m_propertyCache.insert(name, prop);
    return prop;
}

RiveVMStringProperty* RiveViewModelInstance::string(const QString& name)
{
    if (auto* p = lookupOrCreate<RiveVMStringProperty>(name))
        return p;
    rive::ViewModelInstanceValue* v = findValue(m_instance.get(), name);
    if (!v || !v->is<rive::ViewModelInstanceString>())
        return nullptr;
    auto* prop = new RiveVMStringProperty(
        name, v->as<rive::ViewModelInstanceString>(), this);
    m_propertyCache.insert(name, prop);
    return prop;
}

RiveVMColorProperty* RiveViewModelInstance::color(const QString& name)
{
    if (auto* p = lookupOrCreate<RiveVMColorProperty>(name))
        return p;
    rive::ViewModelInstanceValue* v = findValue(m_instance.get(), name);
    if (!v || !v->is<rive::ViewModelInstanceColor>())
        return nullptr;
    auto* prop = new RiveVMColorProperty(
        name, v->as<rive::ViewModelInstanceColor>(), this);
    m_propertyCache.insert(name, prop);
    return prop;
}

RiveVMEnumProperty* RiveViewModelInstance::enumProperty(const QString& name)
{
    if (auto* p = lookupOrCreate<RiveVMEnumProperty>(name))
        return p;
    rive::ViewModelInstanceValue* v = findValue(m_instance.get(), name);
    if (!v || !v->is<rive::ViewModelInstanceEnum>())
        return nullptr;
    auto* prop = new RiveVMEnumProperty(
        name, v->as<rive::ViewModelInstanceEnum>(), this);
    m_propertyCache.insert(name, prop);
    return prop;
}

RiveVMTriggerProperty* RiveViewModelInstance::trigger(const QString& name)
{
    if (auto* p = lookupOrCreate<RiveVMTriggerProperty>(name))
        return p;
    rive::ViewModelInstanceValue* v = findValue(m_instance.get(), name);
    if (!v || !v->is<rive::ViewModelInstanceTrigger>())
        return nullptr;
    auto* prop = new RiveVMTriggerProperty(
        name, v->as<rive::ViewModelInstanceTrigger>(), this);
    m_propertyCache.insert(name, prop);
    return prop;
}

RiveVMNestedProperty* RiveViewModelInstance::viewModel(const QString& name)
{
    if (auto* p = lookupOrCreate<RiveVMNestedProperty>(name))
        return p;
    rive::ViewModelInstanceValue* v = findValue(m_instance.get(), name);
    if (!v || !v->is<rive::ViewModelInstanceViewModel>())
        return nullptr;
    auto* prop = new RiveVMNestedProperty(
        name, v->as<rive::ViewModelInstanceViewModel>(), this);
    m_propertyCache.insert(name, prop);
    return prop;
}

RiveVMListProperty* RiveViewModelInstance::list(const QString& name)
{
    if (auto* p = lookupOrCreate<RiveVMListProperty>(name))
        return p;
    rive::ViewModelInstanceValue* v = findValue(m_instance.get(), name);
    if (!v || !v->is<rive::ViewModelInstanceList>())
        return nullptr;
    auto* prop = new RiveVMListProperty(
        name, v->as<rive::ViewModelInstanceList>(), this);
    m_propertyCache.insert(name, prop);
    return prop;
}

RiveVMImageProperty* RiveViewModelInstance::image(const QString& name)
{
    if (auto* p = lookupOrCreate<RiveVMImageProperty>(name))
        return p;
    rive::ViewModelInstanceValue* v = findValue(m_instance.get(), name);
    if (!v || !v->is<rive::ViewModelInstanceAssetImage>())
        return nullptr;
    auto* prop = new RiveVMImageProperty(
        name, v->as<rive::ViewModelInstanceAssetImage>(), m_factory, this);
    m_propertyCache.insert(name, prop);
    return prop;
}

RiveVMArtboardProperty* RiveViewModelInstance::artboard(const QString& name)
{
    if (auto* p = lookupOrCreate<RiveVMArtboardProperty>(name))
        return p;
    rive::ViewModelInstanceValue* v = findValue(m_instance.get(), name);
    if (!v || !v->is<rive::ViewModelInstanceArtboard>())
        return nullptr;
    auto* prop = new RiveVMArtboardProperty(
        name, v->as<rive::ViewModelInstanceArtboard>(), m_file, this);
    m_propertyCache.insert(name, prop);
    return prop;
}

RiveVMProperty* RiveViewModelInstance::property(const QString& name)
{
    if (auto cached = m_propertyCache.value(name))
        return cached.data();
    rive::ViewModelInstanceValue* v = findValue(m_instance.get(), name);
    if (!v)
        return nullptr;
    if (v->is<rive::ViewModelInstanceNumber>())     return number(name);
    if (v->is<rive::ViewModelInstanceBoolean>())    return boolean(name);
    if (v->is<rive::ViewModelInstanceString>())     return string(name);
    if (v->is<rive::ViewModelInstanceColor>())      return color(name);
    if (v->is<rive::ViewModelInstanceEnum>())       return enumProperty(name);
    if (v->is<rive::ViewModelInstanceTrigger>())    return trigger(name);
    if (v->is<rive::ViewModelInstanceViewModel>())  return viewModel(name);
    if (v->is<rive::ViewModelInstanceList>())       return list(name);
    if (v->is<rive::ViewModelInstanceAssetImage>()) return image(name);
    if (v->is<rive::ViewModelInstanceArtboard>())   return artboard(name);
    return nullptr;
}

void RiveViewModelInstance::advance()
{
    if (!m_instance)
        return;
    // Drain rive's internal data-bind delegates first — this picks up
    // any side-effects from the state machine pushing values into the
    // view model.
    m_instance->advanced();
    // Then poll our cached typed wrappers so QML bindings see the
    // resulting changes.
    for (auto it = m_propertyCache.begin(); it != m_propertyCache.end(); ++it)
    {
        if (RiveVMProperty* p = it.value().data())
            p->poll();
    }
}

RiveViewModelInstance* RiveViewModelInstance::wrap(rive::rcp<rive::ViewModelInstance> instance,
                                                   QObject* parent)
{
    if (!instance)
        return nullptr;
    return new RiveViewModelInstance(std::move(instance), parent);
}

void RiveViewModelInstance::notifyMutated()
{
    emit propertyMutated();
}

void RiveViewModelInstance::buildPropsMap()
{
    if (!m_instance || !m_props)
        return;
    static const QString kTriggerSentinel = QStringLiteral("<trigger>");

    for (const rive::rcp<rive::ViewModelInstanceValue>& val : m_instance->propertyValues())
    {
        if (!val)
            continue;
        rive::ViewModelProperty* prop = val->viewModelProperty();
        if (!prop)
            continue;
        const QString name = QString::fromStdString(prop->name());
        if (name.isEmpty())
            continue;

        if (val->is<rive::ViewModelInstanceNumber>())
        {
            RiveVMNumberProperty* p = number(name);
            if (!p)
                continue;
            m_props->insert(name, p->value());
            connect(p, &RiveVMNumberProperty::valueChanged, this, [this, name, p]() {
                if (m_propsGuard)
                    return;
                m_propsGuard = true;
                m_props->insert(name, p->value());
                m_propsGuard = false;
            });
        }
        else if (val->is<rive::ViewModelInstanceBoolean>())
        {
            RiveVMBooleanProperty* p = boolean(name);
            if (!p)
                continue;
            m_props->insert(name, p->value());
            connect(p, &RiveVMBooleanProperty::valueChanged, this, [this, name, p]() {
                if (m_propsGuard)
                    return;
                m_propsGuard = true;
                m_props->insert(name, p->value());
                m_propsGuard = false;
            });
        }
        else if (val->is<rive::ViewModelInstanceString>())
        {
            RiveVMStringProperty* p = string(name);
            if (!p)
                continue;
            m_props->insert(name, p->value());
            connect(p, &RiveVMStringProperty::valueChanged, this, [this, name, p]() {
                if (m_propsGuard)
                    return;
                m_propsGuard = true;
                m_props->insert(name, p->value());
                m_propsGuard = false;
            });
        }
        else if (val->is<rive::ViewModelInstanceColor>())
        {
            RiveVMColorProperty* p = color(name);
            if (!p)
                continue;
            m_props->insert(name, p->value());
            connect(p, &RiveVMColorProperty::valueChanged, this, [this, name, p]() {
                if (m_propsGuard)
                    return;
                m_propsGuard = true;
                m_props->insert(name, p->value());
                m_propsGuard = false;
            });
        }
        else if (val->is<rive::ViewModelInstanceEnum>())
        {
            RiveVMEnumProperty* p = enumProperty(name);
            if (!p)
                continue;
            // Use the string form — most natural for QML / template
            // strings. Writes accept either a string (route through
            // setValueName) or an int (route through setValueIndex).
            m_props->insert(name, p->valueName());
            connect(p, &RiveVMEnumProperty::valueChanged, this, [this, name, p]() {
                if (m_propsGuard)
                    return;
                m_propsGuard = true;
                m_props->insert(name, p->valueName());
                m_propsGuard = false;
            });
        }
        else if (val->is<rive::ViewModelInstanceTrigger>())
        {
            (void)trigger(name);
            m_props->insert(name, kTriggerSentinel);
        }
        // Lists / nested VMs / image assets / artboard refs intentionally
        // skipped — their wrappers don't reduce to a primitive value and
        // are still reachable via the explicit typed accessors.
    }

    connect(m_props, &QQmlPropertyMap::valueChanged, this,
            [this](const QString& key, const QVariant& v) {
                if (m_propsGuard)
                    return;
                QPointer<RiveVMProperty> base = m_propertyCache.value(key);
                if (!base)
                    return;
                m_propsGuard = true;
                if (auto* p = qobject_cast<RiveVMNumberProperty*>(base.data()))
                    p->setValue(v.toDouble());
                else if (auto* p = qobject_cast<RiveVMBooleanProperty*>(base.data()))
                    p->setValue(v.toBool());
                else if (auto* p = qobject_cast<RiveVMStringProperty*>(base.data()))
                    p->setValue(v.toString());
                else if (auto* p = qobject_cast<RiveVMColorProperty*>(base.data()))
                    p->setValue(v.value<QColor>());
                else if (auto* p = qobject_cast<RiveVMEnumProperty*>(base.data()))
                {
                    // Accept either index or name from QML.
                    if (v.typeId() == QMetaType::QString)
                        p->setValueName(v.toString());
                    else
                        p->setValueIndex(v.toInt());
                }
                // Triggers ignore writes — fire() must be called explicitly.
                m_propsGuard = false;
            });
}
