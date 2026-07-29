#include "rive_artboard.h"
#include "rive_key_map.h"
#include "rive_state_machine.h"

#include <QVariantList>

#include <rive/animation/animation_state.hpp>
#include <rive/animation/any_state.hpp>
#include <rive/animation/entry_state.hpp>
#include <rive/animation/layer_state.hpp>
#include <rive/animation/linear_animation.hpp>
#include <rive/animation/linear_animation_instance.hpp>
#include <rive/animation/state_machine.hpp>
#include <rive/animation/state_machine_instance.hpp>
#include <rive/animation/state_machine_layer.hpp>
#include <rive/animation/state_transition.hpp>
#include <rive/artboard.hpp>
#include <rive/input/focus_manager.hpp>
#include <rive/text/text_value_run.hpp>

namespace {

// The timeline name a state plays — "" for entry / exit / any / blend,
// which have no single timeline (same rule as RiveStateMachine's
// stateChanged signal).
QString stateTimelineName(const rive::LayerState* state)
{
    if (!state || !state->is<rive::AnimationState>())
        return {};
    const rive::LinearAnimation* anim =
        state->as<rive::AnimationState>()->animation();
    return anim ? QString::fromStdString(anim->name()) : QString();
}

} // namespace

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

QStringList RiveArtboard::animationNames() const
{
    QStringList out;
    if (!m_artboard)
        return out;
    const std::size_t n = m_artboard->animationCount();
    out.reserve(static_cast<int>(n));
    for (std::size_t i = 0; i < n; ++i)
        out.append(QString::fromStdString(m_artboard->animationNameAt(i)));
    return out;
}

qreal RiveArtboard::animationDuration(const QString& name) const
{
    if (!m_artboard || name.isEmpty())
        return 0.0;
    const std::size_t n = m_artboard->animationCount();
    const std::string needle = name.toStdString();
    for (std::size_t i = 0; i < n; ++i)
    {
        if (m_artboard->animationNameAt(i) != needle)
            continue;
        // animationAt() hands back an INSTANCE (a fresh playhead); the
        // duration lives on the animation it wraps and costs nothing to read.
        std::unique_ptr<rive::LinearAnimationInstance> inst = m_artboard->animationAt(i);
        if (inst && inst->animation())
            return static_cast<qreal>(inst->animation()->durationSeconds());
        return 0.0;
    }
    return 0.0;
}

bool RiveArtboard::animationLoops(const QString& name) const
{
    if (!m_artboard || name.isEmpty())
        return false;
    const std::size_t n = m_artboard->animationCount();
    const std::string needle = name.toStdString();
    for (std::size_t i = 0; i < n; ++i)
    {
        if (m_artboard->animationNameAt(i) != needle)
            continue;
        std::unique_ptr<rive::LinearAnimationInstance> inst = m_artboard->animationAt(i);
        if (!inst || !inst->animation())
            return false;
        const rive::Loop l = inst->animation()->loop();
        return l == rive::Loop::loop || l == rive::Loop::pingPong;
    }
    return false;
}

QVariantMap RiveArtboard::stateMachineGraph(const QString& name) const
{
    QVariantMap graph;
    if (!m_artboard)
        return graph;
    // Find the named machine's DEFINITION (not an instance) — the graph
    // is static data. Empty name = the artboard's default machine, which
    // is simply its first.
    const rive::StateMachine* machine = nullptr;
    const std::size_t n = m_artboard->stateMachineCount();
    for (std::size_t i = 0; i < n; ++i)
    {
        if (name.isEmpty() ||
            m_artboard->stateMachineNameAt(i) == name.toStdString())
        {
            machine = m_artboard->stateMachine(i);
            break;
        }
    }
    if (!machine || machine->layerCount() == 0)
        return graph;

    const rive::StateMachineLayer* layer = machine->layer(0);

    // Timeline names per state, entry first, then transitions resolved
    // from LayerState* back to names.
    QStringList states;
    for (std::size_t i = 0; i < layer->stateCount(); ++i)
        states.append(stateTimelineName(layer->state(i)));
    graph[QStringLiteral("states")] = states;

    QString entry;
    QVariantList transitions;
    auto addTransitions = [&](const rive::LayerState* from)
    {
        if (!from)
            return;
        const QString fromName = stateTimelineName(from);
        for (std::size_t t = 0; t < from->transitionCount(); ++t)
        {
            const rive::StateTransition* tr = from->transition(t);
            if (!tr || tr->isDisabled())
                continue;
            const QString toName = stateTimelineName(tr->stateTo());
            QVariantMap edge;
            edge[QStringLiteral("from")] = fromName;
            edge[QStringLiteral("to")] = toName;
            transitions.append(edge);
            if (from->is<rive::EntryState>())
                entry = toName;
        }
    };
    addTransitions(layer->entryState());
    addTransitions(layer->anyState());
    for (std::size_t i = 0; i < layer->stateCount(); ++i)
        addTransitions(layer->state(i));
    graph[QStringLiteral("entry")] = entry;
    graph[QStringLiteral("transitions")] = transitions;
    return graph;
}

std::unique_ptr<rive::LinearAnimationInstance>
RiveArtboard::createAnimation(const QString& name) const
{
    if (!m_artboard || m_artboard->animationCount() == 0)
        return nullptr;
    if (name.isEmpty())
        return m_artboard->animationAt(0);
    return m_artboard->animationNamed(name.toStdString());
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
