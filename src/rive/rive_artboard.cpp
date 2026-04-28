#include "rive_artboard.h"
#include "rive_key_map.h"
#include "rive_state_machine.h"

#include <rive/animation/state_machine_instance.hpp>
#include <rive/artboard.hpp>
#include <rive/input/focus_manager.hpp>
#include <rive/text/text_value_run.hpp>

RiveArtboard::RiveArtboard(std::unique_ptr<rive::ArtboardInstance> instance,
                           QString name,
                           QObject* parent)
    : QObject(parent), m_artboard(std::move(instance)), m_name(std::move(name))
{}

RiveArtboard::~RiveArtboard()
{
    // rive::StateMachineInstance::~StateMachineInstance calls back into
    // its rive::Artboard (Artboard::cleanupFocusTree). Our SMIs live
    // inside RiveStateMachine wrappers that we made QObject children
    // of this RiveArtboard. Default destruction order would tear down
    // m_artboard first (member dtor), then run ~QObject which deletes
    // child wrappers — SMI dtors then dereference a freed Artboard.
    //
    // Tear down children explicitly here so they observe m_artboard
    // still live; m_artboard auto-destructs after this body returns.
    //
    // Latent everywhere — macOS / Windows release silently UB; Windows
    // debug crashes in std::vector iterator adoption (_Container_proxy*
    // dangling) when cleanupFocusTree iterates the Artboard's m_Objects.
    qDeleteAll(findChildren<QObject*>(Qt::FindDirectChildrenOnly));
}

qreal RiveArtboard::width() const
{
    return m_artboard ? static_cast<qreal>(m_artboard->width()) : 0.0;
}

qreal RiveArtboard::height() const
{
    return m_artboard ? static_cast<qreal>(m_artboard->height()) : 0.0;
}

qreal RiveArtboard::originalWidth() const
{
    return m_artboard ? static_cast<qreal>(m_artboard->originalWidth()) : 0.0;
}

qreal RiveArtboard::originalHeight() const
{
    return m_artboard ? static_cast<qreal>(m_artboard->originalHeight()) : 0.0;
}

void RiveArtboard::setSize(const QSizeF& size)
{
    if (!m_artboard || !size.isValid())
        return;
    m_artboard->width(static_cast<float>(size.width()));
    m_artboard->height(static_cast<float>(size.height()));
}

void RiveArtboard::resetSize()
{
    if (!m_artboard)
        return;
    m_artboard->width(m_artboard->originalWidth());
    m_artboard->height(m_artboard->originalHeight());
}

rive::ArtboardInstance* RiveArtboard::raw() const
{
    return m_artboard.get();
}

QStringList RiveArtboard::stateMachineNames() const
{
    QStringList out;
    if (!m_artboard)
        return out;
    const std::size_t n = m_artboard->stateMachineCount();
    out.reserve(static_cast<int>(n));
    for (std::size_t i = 0; i < n; ++i)
        out.append(QString::fromStdString(m_artboard->stateMachineNameAt(i)));
    return out;
}

bool RiveArtboard::setTextRun(const QString& name, const QString& value)
{
    return setTextRunAtPath(name, value, QString());
}

bool RiveArtboard::setTextRunAtPath(const QString& name,
                                    const QString& value,
                                    const QString& path)
{
    if (!m_artboard || name.isEmpty())
        return false;
    rive::TextValueRun* run =
        m_artboard->getTextRun(name.toStdString(), path.toStdString());
    if (!run)
        return false;
    run->text(value.toStdString());
    return true;
}

RiveStateMachine* RiveArtboard::createStateMachine(const QString& name)
{
    if (!m_artboard)
        return nullptr;
    std::unique_ptr<rive::StateMachineInstance> instance;
    if (name.isEmpty())
        instance = m_artboard->defaultStateMachine();
    else
        instance = m_artboard->stateMachineNamed(name.toStdString());
    if (!instance)
        return nullptr;
    // Caller owns. The SM cannot be a Qt child of `this` because Qt
    // requires moveToThread()'d objects to be parent-less; RiveView
    // moves the SM to the GUI thread so its QQmlPropertyMap children
    // are reachable from QML. Caller must destroy the SM BEFORE
    // m_artboard goes — rive::StateMachineInstance::~ touches the
    // owning rive::Artboard during cleanupFocusTree.
    return new RiveStateMachine(std::move(instance), nullptr);
}

bool RiveArtboard::keyInput(int qtKey, int qtModifiers, bool pressed, bool isAutoRepeat)
{
    if (!m_artboard)
        return false;
    rive::FocusManager* fm = m_artboard->focusManager();
    if (!fm)
        return false;
    const auto mapped = rive_key_map::fromQtKey(qtKey);
    if (!mapped.valid)
        return false;
    const rive::KeyModifiers mods = rive_key_map::fromQtModifiers(qtModifiers);
    return fm->keyInput(mapped.key, mods, pressed, isAutoRepeat);
}

bool RiveArtboard::textInput(const QString& text)
{
    if (!m_artboard || text.isEmpty())
        return false;
    rive::FocusManager* fm = m_artboard->focusManager();
    if (!fm)
        return false;
    return fm->textInput(text.toStdString());
}

bool RiveArtboard::focusNext()
{
    auto* fm = m_artboard ? m_artboard->focusManager() : nullptr;
    return fm ? fm->focusNext() : false;
}

bool RiveArtboard::focusPrevious()
{
    auto* fm = m_artboard ? m_artboard->focusManager() : nullptr;
    return fm ? fm->focusPrevious() : false;
}

bool RiveArtboard::focusLeft()
{
    auto* fm = m_artboard ? m_artboard->focusManager() : nullptr;
    return fm ? fm->focusLeft() : false;
}

bool RiveArtboard::focusRight()
{
    auto* fm = m_artboard ? m_artboard->focusManager() : nullptr;
    return fm ? fm->focusRight() : false;
}

bool RiveArtboard::focusUp()
{
    auto* fm = m_artboard ? m_artboard->focusManager() : nullptr;
    return fm ? fm->focusUp() : false;
}

bool RiveArtboard::focusDown()
{
    auto* fm = m_artboard ? m_artboard->focusManager() : nullptr;
    return fm ? fm->focusDown() : false;
}

void RiveArtboard::bindViewModelInstance(rive::rcp<rive::ViewModelInstance> instance)
{
    if (!m_artboard || !instance)
        return;
    m_artboard->bindViewModelInstance(std::move(instance));
}
