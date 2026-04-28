#include "rive_state_machine.h"

#include <QString>

#include <rive/animation/layer_state.hpp>
#include <rive/animation/state_machine_instance.hpp>
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

RiveStateMachine::HitResult RiveStateMachine::pointerDown(const QPointF& pos, int pointerId)
{
    if (!m_sm)
        return HitResult::None;
    return toHitResult(m_sm->pointerDown(toRive(pos), pointerId));
}

RiveStateMachine::HitResult RiveStateMachine::pointerMove(const QPointF& pos, int pointerId)
{
    if (!m_sm)
        return HitResult::None;
    // pointerMove also accepts a timestamp; we leave it at default
    // since rive doesn't require monotonic timestamps for hit testing.
    return toHitResult(m_sm->pointerMove(toRive(pos), 0.0f, pointerId));
}

RiveStateMachine::HitResult RiveStateMachine::pointerUp(const QPointF& pos, int pointerId)
{
    if (!m_sm)
        return HitResult::None;
    return toHitResult(m_sm->pointerUp(toRive(pos), pointerId));
}

RiveStateMachine::HitResult RiveStateMachine::pointerExit(const QPointF& pos, int pointerId)
{
    if (!m_sm)
        return HitResult::None;
    return toHitResult(m_sm->pointerExit(toRive(pos), pointerId));
}

bool RiveStateMachine::advance(float deltaSeconds)
{
    if (!m_sm)
        return false;
    const bool needsMore = m_sm->advanceAndApply(deltaSeconds);
    drainStateChanges();
    return needsMore;
}

void RiveStateMachine::bindViewModelInstance(rive::rcp<rive::ViewModelInstance> instance)
{
    if (!m_sm || !instance)
        return;
    m_sm->bindViewModelInstance(std::move(instance));
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
