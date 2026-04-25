#include "rive_artboard.h"
#include "rive_key_map.h"
#include "rive_state_machine.h"

#include <rive/animation/state_machine_instance.hpp>
#include <rive/artboard.hpp>
#include <rive/input/focus_manager.hpp>

RiveArtboard::RiveArtboard(std::unique_ptr<rive::ArtboardInstance> instance,
                           QString name,
                           QObject* parent)
    : QObject(parent), m_artboard(std::move(instance)), m_name(std::move(name))
{}

RiveArtboard::~RiveArtboard() = default;

qreal RiveArtboard::width() const
{
    return m_artboard ? static_cast<qreal>(m_artboard->width()) : 0.0;
}

qreal RiveArtboard::height() const
{
    return m_artboard ? static_cast<qreal>(m_artboard->height()) : 0.0;
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
    return new RiveStateMachine(std::move(instance), this);
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
