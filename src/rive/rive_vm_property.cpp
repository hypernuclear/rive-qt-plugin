#include "rive_vm_property.h"
#include "rive_view_model.h"

#include <QBuffer>
#include <QFile>
#include <QImage>
#include <QUrl>

#include <rive/artboard.hpp>

#include <rive/bindable_artboard.hpp>
#include <rive/file.hpp>
#include <rive/viewmodel/viewmodel_instance_artboard.hpp>
#include <rive/viewmodel/viewmodel_instance_asset_image.hpp>
#include <rive/viewmodel/viewmodel_instance_boolean.hpp>
#include <rive/viewmodel/viewmodel_instance_color.hpp>
#include <rive/viewmodel/viewmodel_instance_enum.hpp>
#include <rive/viewmodel/viewmodel_instance_list.hpp>
#include <rive/viewmodel/viewmodel_instance_list_item.hpp>
#include <rive/viewmodel/viewmodel_instance_number.hpp>
#include <rive/viewmodel/viewmodel_instance_string.hpp>
#include <rive/viewmodel/viewmodel_instance_trigger.hpp>
#include <rive/viewmodel/viewmodel_instance_value.hpp>
#include <rive/viewmodel/viewmodel_instance_viewmodel.hpp>
#include <rive/viewmodel/viewmodel_property.hpp>
#include <rive/viewmodel/viewmodel_property_enum.hpp>
#include <rive/viewmodel/viewmodel_property_enum_custom.hpp>
#include <rive/viewmodel/viewmodel_property_enum_system.hpp>
#include <rive/viewmodel/data_enum.hpp>
#include <rive/viewmodel/data_enum_value.hpp>

namespace {

// Walk up to the parent RiveViewModelInstance and tell it a property
// was mutated. Lets RiveView wake a settled state machine so the
// new value actually drives the artboard.
void notifyParent(QObject* property)
{
    if (auto* vmi = qobject_cast<RiveViewModelInstance*>(property->parent()))
        vmi->notifyMutated();
}

// Pack a QColor into rive's int representation. rive uses
// 0xAARRGGBB layout (uint32_t reinterpreted as int).
int packColor(const QColor& c)
{
    return (static_cast<uint32_t>(c.alpha()) << 24) |
           (static_cast<uint32_t>(c.red()) << 16) |
           (static_cast<uint32_t>(c.green()) << 8) |
           static_cast<uint32_t>(c.blue());
}

QColor unpackColor(int v)
{
    const uint32_t u = static_cast<uint32_t>(v);
    return QColor::fromRgb(
        static_cast<int>((u >> 16) & 0xff),
        static_cast<int>((u >> 8) & 0xff),
        static_cast<int>(u & 0xff),
        static_cast<int>((u >> 24) & 0xff));
}

} // namespace

RiveVMProperty::RiveVMProperty(QString name, QObject* parent)
    : QObject(parent), m_name(std::move(name))
{}

// ----- Number ---------------------------------------------------------------

RiveVMNumberProperty::RiveVMNumberProperty(QString name, rive::ViewModelInstanceNumber* v,
                                           QObject* parent)
    : RiveVMProperty(std::move(name), parent), m_v(v),
      m_cached(v ? v->propertyValue() : 0.0f)
{}

double RiveVMNumberProperty::value() const
{
    return m_v ? static_cast<double>(m_v->propertyValue()) : 0.0;
}

void RiveVMNumberProperty::setValue(double v)
{
    if (!m_v)
        return;
    const float f = static_cast<float>(v);
    if (m_v->propertyValue() == f)
        return;
    m_v->propertyValue(f);
    notifyParent(this);
    if (m_cached != f)
    {
        m_cached = f;
        emit valueChanged();
    }
}

void RiveVMNumberProperty::poll()
{
    if (!m_v)
        return;
    const float now = m_v->propertyValue();
    if (now != m_cached)
    {
        m_cached = now;
        emit valueChanged();
    }
}

// ----- Boolean --------------------------------------------------------------

RiveVMBooleanProperty::RiveVMBooleanProperty(QString name, rive::ViewModelInstanceBoolean* v,
                                             QObject* parent)
    : RiveVMProperty(std::move(name), parent), m_v(v),
      m_cached(v ? v->propertyValue() : false)
{}

bool RiveVMBooleanProperty::value() const
{
    return m_v ? m_v->propertyValue() : false;
}

void RiveVMBooleanProperty::setValue(bool v)
{
    if (!m_v || m_v->propertyValue() == v)
        return;
    m_v->propertyValue(v);
    notifyParent(this);
    if (m_cached != v)
    {
        m_cached = v;
        emit valueChanged();
    }
}

void RiveVMBooleanProperty::poll()
{
    if (!m_v)
        return;
    const bool now = m_v->propertyValue();
    if (now != m_cached)
    {
        m_cached = now;
        emit valueChanged();
    }
}

// ----- String ---------------------------------------------------------------

RiveVMStringProperty::RiveVMStringProperty(QString name, rive::ViewModelInstanceString* v,
                                           QObject* parent)
    : RiveVMProperty(std::move(name), parent), m_v(v),
      m_cached(v ? QString::fromStdString(v->propertyValue()) : QString{})
{}

QString RiveVMStringProperty::value() const
{
    return m_v ? QString::fromStdString(m_v->propertyValue()) : QString{};
}

void RiveVMStringProperty::setValue(const QString& v)
{
    if (!m_v)
        return;
    const std::string s = v.toStdString();
    if (m_v->propertyValue() == s)
        return;
    m_v->propertyValue(s);
    notifyParent(this);
    if (m_cached != v)
    {
        m_cached = v;
        emit valueChanged();
    }
}

void RiveVMStringProperty::poll()
{
    if (!m_v)
        return;
    const QString now = QString::fromStdString(m_v->propertyValue());
    if (now != m_cached)
    {
        m_cached = now;
        emit valueChanged();
    }
}

// ----- Color ----------------------------------------------------------------

RiveVMColorProperty::RiveVMColorProperty(QString name, rive::ViewModelInstanceColor* v,
                                         QObject* parent)
    : RiveVMProperty(std::move(name), parent), m_v(v),
      m_cached(v ? v->propertyValue() : 0)
{}

QColor RiveVMColorProperty::value() const
{
    return m_v ? unpackColor(m_v->propertyValue()) : QColor();
}

void RiveVMColorProperty::setValue(const QColor& v)
{
    if (!m_v)
        return;
    const int packed = packColor(v);
    if (m_v->propertyValue() == packed)
        return;
    m_v->propertyValue(packed);
    notifyParent(this);
    if (m_cached != packed)
    {
        m_cached = packed;
        emit valueChanged();
    }
}

void RiveVMColorProperty::poll()
{
    if (!m_v)
        return;
    const int now = m_v->propertyValue();
    if (now != m_cached)
    {
        m_cached = now;
        emit valueChanged();
    }
}

// ----- Enum -----------------------------------------------------------------

RiveVMEnumProperty::RiveVMEnumProperty(QString name, rive::ViewModelInstanceEnum* v,
                                       QObject* parent)
    : RiveVMProperty(std::move(name), parent), m_v(v),
      m_cached(v ? v->propertyValue() : 0)
{
    if (!v)
        return;
    // Discover the allowed enum values via the property definition.
    // Two flavors — system (built-in rive enums like fit/alignment)
    // and custom (user-defined). Both expose dataEnum().
    rive::ViewModelProperty* prop = v->viewModelProperty();
    if (!prop)
        return;
    rive::DataEnum* dataEnum = nullptr;
    if (prop->is<rive::ViewModelPropertyEnumCustom>())
        dataEnum = prop->as<rive::ViewModelPropertyEnumCustom>()->dataEnum();
    else if (prop->is<rive::ViewModelPropertyEnumSystem>())
        dataEnum = prop->as<rive::ViewModelPropertyEnumSystem>()->dataEnum();
    if (!dataEnum)
        return;
    for (const rive::DataEnumValue* dv : dataEnum->values())
    {
        if (dv)
            m_values.append(QString::fromStdString(dv->key()));
    }
}

int RiveVMEnumProperty::valueIndex() const
{
    return m_v ? static_cast<int>(m_v->propertyValue()) : -1;
}

void RiveVMEnumProperty::setValueIndex(int idx)
{
    if (!m_v || idx < 0)
        return;
    if (m_v->propertyValue() == static_cast<uint32_t>(idx))
        return;
    if (!m_v->value(static_cast<uint32_t>(idx)))
        return;
    notifyParent(this);
    if (m_cached != static_cast<uint32_t>(idx))
    {
        m_cached = static_cast<uint32_t>(idx);
        emit valueChanged();
    }
}

QString RiveVMEnumProperty::valueName() const
{
    if (!m_v)
        return {};
    const int idx = valueIndex();
    if (idx >= 0 && idx < m_values.size())
        return m_values.at(idx);
    return {};
}

void RiveVMEnumProperty::setValueName(const QString& v)
{
    if (!m_v)
        return;
    if (!m_v->value(v.toStdString()))
        return;
    notifyParent(this);
    const uint32_t now = m_v->propertyValue();
    if (m_cached != now)
    {
        m_cached = now;
        emit valueChanged();
    }
}

void RiveVMEnumProperty::poll()
{
    if (!m_v)
        return;
    const uint32_t now = m_v->propertyValue();
    if (now != m_cached)
    {
        m_cached = now;
        emit valueChanged();
    }
}

// ----- Trigger --------------------------------------------------------------

RiveVMTriggerProperty::RiveVMTriggerProperty(QString name, rive::ViewModelInstanceTrigger* v,
                                             QObject* parent)
    : RiveVMProperty(std::move(name), parent), m_v(v),
      m_cached(v ? v->propertyValue() : 0)
{}

void RiveVMTriggerProperty::fire()
{
    if (!m_v)
        return;
    m_v->trigger();
    notifyParent(this);
    // Don't emit triggered() from here directly — let poll() catch
    // the counter increment and emit. That way we treat external
    // (rive-driven) trigger fires the same as our own.
}

void RiveVMTriggerProperty::poll()
{
    if (!m_v)
        return;
    const uint32_t now = m_v->propertyValue();
    if (now != m_cached)
    {
        m_cached = now;
        emit triggered();
    }
}

// ----- Nested ViewModel -----------------------------------------------------

RiveVMNestedProperty::RiveVMNestedProperty(QString name,
                                           rive::ViewModelInstanceViewModel* v,
                                           QObject* parent)
    : RiveVMProperty(std::move(name), parent), m_v(v)
{
    rebuildWrapper();
}

RiveVMNestedProperty::~RiveVMNestedProperty() = default;

RiveViewModelInstance* RiveVMNestedProperty::value() const
{
    return m_wrapper;
}

void RiveVMNestedProperty::rebuildWrapper()
{
    if (m_wrapper)
    {
        delete m_wrapper;
        m_wrapper = nullptr;
    }
    if (!m_v)
    {
        m_cachedRaw = nullptr;
        return;
    }
    rive::rcp<rive::ViewModelInstance> ref = m_v->referenceViewModelInstance();
    m_cachedRaw = ref.get();
    if (!ref)
        return;
    m_wrapper = new RiveViewModelInstance(std::move(ref), this);
    // Forward mutations from the nested instance up to our own
    // parent — RiveView only subscribes to the root VMI's signal, so
    // without this chain a nested-property edit wouldn't wake the
    // advance loop.
    QObject::connect(m_wrapper, &RiveViewModelInstance::propertyMutated,
                     this, [this]() { notifyParent(this); });
}

void RiveVMNestedProperty::poll()
{
    if (!m_v)
        return;
    // Reference identity is what matters — rive can swap a nested VM
    // out from under us at runtime.
    rive::ViewModelInstance* now = m_v->referenceViewModelInstance().get();
    if (now != m_cachedRaw)
    {
        rebuildWrapper();
        emit valueChanged();
    }
    else if (m_wrapper)
    {
        // Same instance, but its property values may have changed —
        // recurse into the nested wrapper so its own typed wrappers
        // poll their values.
        m_wrapper->advance();
    }
}

// ----- List -----------------------------------------------------------------

RiveVMListProperty::RiveVMListProperty(QString name, rive::ViewModelInstanceList* v,
                                       QObject* parent)
    : RiveVMProperty(std::move(name), parent), m_v(v),
      m_cachedCount(v ? static_cast<int>(v->listItems().size()) : 0)
{}

RiveVMListProperty::~RiveVMListProperty() = default;

int RiveVMListProperty::count() const
{
    return m_v ? static_cast<int>(m_v->listItems().size()) : 0;
}

RiveViewModelInstance* RiveVMListProperty::itemAt(int index)
{
    if (!m_v || index < 0 || index >= count())
        return nullptr;
    if (auto* cached = m_itemWrappers.value(index))
        return cached;

    rive::rcp<rive::ViewModelInstanceListItem> listItem = m_v->item(static_cast<uint32_t>(index));
    if (!listItem)
        return nullptr;
    rive::rcp<rive::ViewModelInstance> inst = listItem->viewModelInstance();
    if (!inst)
        return nullptr;
    auto* wrapper = new RiveViewModelInstance(std::move(inst), this);
    m_itemWrappers.insert(index, wrapper);
    // Forward item-side mutations up — same chain as the nested-VM
    // case so a deep edit reaches the root and wakes RiveView.
    QObject::connect(wrapper, &RiveViewModelInstance::propertyMutated,
                     this, [this]() { notifyParent(this); });
    return wrapper;
}

void RiveVMListProperty::removeAt(int index)
{
    if (!m_v || index < 0 || index >= count())
        return;
    m_v->removeItem(index);
    invalidateWrapperCache();
    notifyParent(this);
    poll();
}

void RiveVMListProperty::swap(int a, int b)
{
    if (!m_v || a < 0 || b < 0 || a >= count() || b >= count() || a == b)
        return;
    m_v->swap(static_cast<uint32_t>(a), static_cast<uint32_t>(b));
    invalidateWrapperCache();
    notifyParent(this);
    // Count unchanged — no countChanged emit, but item identities
    // shifted so QML should re-fetch wrappers.
    emit countChanged();
}

void RiveVMListProperty::clear()
{
    if (!m_v)
        return;
    while (count() > 0)
        m_v->removeItem(0);
    invalidateWrapperCache();
    notifyParent(this);
    poll();
}

void RiveVMListProperty::poll()
{
    if (!m_v)
        return;
    const int now = count();
    if (now != m_cachedCount)
    {
        m_cachedCount = now;
        emit countChanged();
    }
    // Recurse into existing item wrappers so their typed properties
    // pick up rive-side changes.
    for (auto it = m_itemWrappers.begin(); it != m_itemWrappers.end(); ++it)
    {
        if (it.value())
            it.value()->advance();
    }
}

void RiveVMListProperty::invalidateWrapperCache()
{
    for (auto it = m_itemWrappers.begin(); it != m_itemWrappers.end(); ++it)
    {
        if (it.value())
            it.value()->deleteLater();
    }
    m_itemWrappers.clear();
}

// ----- Image ----------------------------------------------------------------

RiveVMImageProperty::RiveVMImageProperty(QString name,
                                         rive::ViewModelInstanceAssetImage* v,
                                         rive::Factory* factory, QObject* parent)
    : RiveVMProperty(std::move(name), parent), m_v(v), m_factory(factory)
{}

bool RiveVMImageProperty::setSource(const QUrl& url)
{
    if (!m_v || !m_factory)
        return false;

    QString path;
    if (url.scheme() == QStringLiteral("qrc"))
        path = QStringLiteral(":") + url.path();
    else if (url.isLocalFile())
        path = url.toLocalFile();
    else
        return false;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QByteArray bytes = f.readAll();
    if (bytes.isEmpty())
        return false;

    rive::rcp<rive::RenderImage> img = m_factory->decodeImage(
        rive::Span<const uint8_t>(reinterpret_cast<const uint8_t*>(bytes.constData()),
                                  static_cast<std::size_t>(bytes.size())));
    if (!img)
        return false;
    m_v->value(img.get());
    notifyParent(this);
    emit valueChanged();
    return true;
}

bool RiveVMImageProperty::setImage(const QImage& image)
{
    if (!m_v || !m_factory || image.isNull())
        return false;
    // Round-trip through PNG so the existing factory decode path
    // handles format conversion + texture upload. Suboptimal vs. a
    // direct QImage→Texture path, but generic across factory impls.
    QByteArray bytes;
    {
        QBuffer buf(&bytes);
        buf.open(QIODevice::WriteOnly);
        if (!image.save(&buf, "PNG"))
            return false;
    }
    rive::rcp<rive::RenderImage> img = m_factory->decodeImage(
        rive::Span<const uint8_t>(reinterpret_cast<const uint8_t*>(bytes.constData()),
                                  static_cast<std::size_t>(bytes.size())));
    if (!img)
        return false;
    m_v->value(img.get());
    notifyParent(this);
    emit valueChanged();
    return true;
}

// ----- Artboard ref ---------------------------------------------------------

RiveVMArtboardProperty::RiveVMArtboardProperty(QString name,
                                               rive::ViewModelInstanceArtboard* v,
                                               rive::File* file, QObject* parent)
    : RiveVMProperty(std::move(name), parent), m_v(v), m_file(file),
      m_cachedRaw(v ? v->asset().get() : nullptr)
{}

void RiveVMArtboardProperty::poll()
{
    if (!m_v)
        return;
    void* now = m_v->asset().get();
    if (now != m_cachedRaw)
    {
        m_cachedRaw = now;
        emit valueChanged();
    }
}

QString RiveVMArtboardProperty::value() const
{
    if (!m_v)
        return {};
    rive::rcp<rive::BindableArtboard> ba = m_v->asset();
    if (!ba || !ba->artboard())
        return {};
    // BindableArtboard wraps an ArtboardInstance — the name lives on
    // the inner Component.
    return QString::fromStdString(ba->artboard()->name());
}

void RiveVMArtboardProperty::setValue(const QString& artboardName)
{
    if (!m_v || !m_file)
        return;
    rive::rcp<rive::BindableArtboard> ba = artboardName.isEmpty()
        ? rive::rcp<rive::BindableArtboard>(nullptr)
        : m_file->bindableArtboardNamed(artboardName.toStdString());
    if (!ba && !artboardName.isEmpty())
        return; // unknown name; leave current value unchanged
    m_v->asset(std::move(ba));
    notifyParent(this);
    emit valueChanged();
}
