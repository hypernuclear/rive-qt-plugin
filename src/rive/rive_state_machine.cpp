#include "rive_state_machine.h"

#include <QString>

#include <rive/animation/layer_state.hpp>
#include <rive/animation/state_machine_instance.hpp>
#include <rive/animation/state_machine_input_instance.hpp>
#include <rive/event.hpp>
#include <rive/event_report.hpp>
#include <rive/hit_result.hpp>
#include <rive/math/vec2d.hpp>

namespace {

RiveStateMachine::HitResult toHitResult(rive::HitResult r)
{
    switch (r)
    {
    case rive::HitResult::none:      return RiveStateMachine::HitResult::None;
    case rive::HitResult::hit:       return RiveStateMachine::HitResult::Hit;
    case rive::HitResult::hitOpaque: return RiveStateMachine::HitResult::HitOpaque;
    }
    return RiveStateMachine::HitResult::None;
}

rive::Vec2D toRive(const QPointF& p)
{
    return rive::Vec2D(static_cast<float>(p.x()), static_cast<float>(p.y()));
}

} // namespace

RiveStateMachine::RiveStateMachine(std::unique_ptr<rive::StateMachineInstance> instance,
                                   QObject* parent)
    : QObject(parent), m_sm(std::move(instance))
{}

RiveStateMachine::~RiveStateMachine() = default;

QString RiveStateMachine::name() const
{
    if (!m_sm)
        return {};
    return QString::fromStdString(m_sm->name());
}

rive::StateMachineInstance* RiveStateMachine::raw() const
{
    return m_sm.get();
}

RiveBoolInput* RiveStateMachine::getBool(const QString& name)
{
    if (!m_sm)
        return nullptr;
    if (auto cached = m_inputCache.value(name))
    {
        return qobject_cast<RiveBoolInput*>(cached.data());
    }
    rive::SMIBool* smi = m_sm->getBool(name.toStdString());
    if (!smi)
        return nullptr;
    auto* input = new RiveBoolInput(name, smi, this);
    m_inputCache.insert(name, input);
    return input;
}

RiveNumberInput* RiveStateMachine::getNumber(const QString& name)
{
    if (!m_sm)
        return nullptr;
    if (auto cached = m_inputCache.value(name))
    {
        return qobject_cast<RiveNumberInput*>(cached.data());
    }
    rive::SMINumber* smi = m_sm->getNumber(name.toStdString());
    if (!smi)
        return nullptr;
    auto* input = new RiveNumberInput(name, smi, this);
    m_inputCache.insert(name, input);
    return input;
}

RiveTriggerInput* RiveStateMachine::getTrigger(const QString& name)
{
    if (!m_sm)
        return nullptr;
    if (auto cached = m_inputCache.value(name))
    {
        return qobject_cast<RiveTriggerInput*>(cached.data());
    }
    rive::SMITrigger* smi = m_sm->getTrigger(name.toStdString());
    if (!smi)
        return nullptr;
    auto* input = new RiveTriggerInput(name, smi, this);
    m_inputCache.insert(name, input);
    return input;
}

RiveStateMachine::HitResult RiveStateMachine::pointerDown(const QPointF& pos)
{
    if (!m_sm)
        return HitResult::None;
    return toHitResult(m_sm->pointerDown(toRive(pos)));
}

RiveStateMachine::HitResult RiveStateMachine::pointerMove(const QPointF& pos)
{
    if (!m_sm)
        return HitResult::None;
    return toHitResult(m_sm->pointerMove(toRive(pos)));
}

RiveStateMachine::HitResult RiveStateMachine::pointerUp(const QPointF& pos)
{
    if (!m_sm)
        return HitResult::None;
    return toHitResult(m_sm->pointerUp(toRive(pos)));
}

RiveStateMachine::HitResult RiveStateMachine::pointerExit(const QPointF& pos)
{
    if (!m_sm)
        return HitResult::None;
    return toHitResult(m_sm->pointerExit(toRive(pos)));
}

bool RiveStateMachine::advance(float deltaSeconds)
{
    if (!m_sm)
        return false;
    const bool needsMore = m_sm->advanceAndApply(deltaSeconds);
    drainStateChanges();
    drainEvents();
    return needsMore;
}

void RiveStateMachine::drainEvents()
{
    const std::size_t n = m_sm->reportedEventCount();
    for (std::size_t i = 0; i < n; ++i)
    {
        const rive::EventReport report = m_sm->reportedEventAt(i);
        rive::Event* e = report.event();
        if (!e)
            continue;
        RiveEvent qe(QString::fromStdString(e->name()), report.secondsDelay());
        emit eventReported(qe);
    }
}

void RiveStateMachine::drainStateChanges()
{
    const std::size_t n = m_sm->stateChangedCount();
    for (std::size_t i = 0; i < n; ++i)
    {
        const rive::LayerState* state = m_sm->stateChangedByIndex(i);
        if (!state)
            continue;
        // LayerState's name comes from its underlying animation/entry;
        // we'd need a public accessor to get it reliably. For now emit
        // the layer index (as string) + an empty state name. Revisit
        // when we add state-name support (rive has the data, just needs
        // a small accessor).
        emit stateChanged(QString::number(i), QString());
    }
}
