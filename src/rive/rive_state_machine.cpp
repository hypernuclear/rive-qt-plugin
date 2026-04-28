#include "rive_state_machine.h"

#include <QString>

#include <rive/animation/layer_state.hpp>
#include <rive/animation/state_machine_instance.hpp>
#include <rive/animation/state_machine_input_instance.hpp>
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
    : QObject(parent), m_sm(std::move(instance)), m_inputs(new QQmlPropertyMap(this))
{
    buildInputsMap();
}

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

void RiveStateMachine::buildInputsMap()
{
    if (!m_sm)
        return;
    // Sentinel string for triggers — the map needs *something* per key
    // for Object.keys() discoverability; we use a tagged string so debug
    // overlays can render it sensibly. Writing through the map for a
    // trigger key is a no-op (handled below).
    static const QString kTriggerSentinel = QStringLiteral("<trigger>");

    const std::size_t n = m_sm->inputCount();
    for (std::size_t i = 0; i < n; ++i)
    {
        rive::SMIInput* smi = m_sm->input(i);
        if (!smi)
            continue;
        const std::string stdName = smi->name();
        const QString name = QString::fromStdString(stdName);
        if (name.isEmpty())
            continue;

        // Discriminate via the runtime's typed accessors — each returns
        // nullptr if the name resolves to a different type. The wrapper-
        // side getXxx() additionally caches under m_inputCache so the
        // wrapper outlives this call.
        if (m_sm->getBool(stdName))
        {
            RiveBoolInput* b = getBool(name);
            if (!b)
                continue;
            m_inputs->insert(name, b->value());
            connect(b, &RiveBoolInput::valueChanged, this, [this, name, b]() {
                if (m_inputMapGuard)
                    return;
                m_inputMapGuard = true;
                m_inputs->insert(name, b->value());
                m_inputMapGuard = false;
            });
        }
        else if (m_sm->getNumber(stdName))
        {
            RiveNumberInput* num = getNumber(name);
            if (!num)
                continue;
            m_inputs->insert(name, num->value());
            connect(num, &RiveNumberInput::valueChanged, this, [this, name, num]() {
                if (m_inputMapGuard)
                    return;
                m_inputMapGuard = true;
                m_inputs->insert(name, num->value());
                m_inputMapGuard = false;
            });
        }
        else if (m_sm->getTrigger(stdName))
        {
            // Materialize the wrapper so getTrigger(name) is cached.
            (void)getTrigger(name);
            m_inputs->insert(name, kTriggerSentinel);
        }
    }

    // Map → wrapper dispatch. QML assignments hit here; we route the
    // new value back through the typed setter, which (for bool/number)
    // re-emits valueChanged and re-enters the connections above —
    // m_inputMapGuard breaks the loop.
    connect(m_inputs, &QQmlPropertyMap::valueChanged, this,
            [this](const QString& key, const QVariant& v) {
                if (m_inputMapGuard)
                    return;
                QPointer<RiveInput> input = m_inputCache.value(key);
                if (!input)
                    return;
                m_inputMapGuard = true;
                if (auto* b = qobject_cast<RiveBoolInput*>(input.data()))
                    b->setValue(v.toBool());
                else if (auto* num = qobject_cast<RiveNumberInput*>(input.data()))
                    num->setValue(v.toDouble());
                // Triggers ignore writes — fire() must be called explicitly.
                m_inputMapGuard = false;
            });
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
