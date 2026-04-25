#include "rive_input.h"

#include <rive/animation/state_machine_input_instance.hpp>

RiveInput::RiveInput(QString name, QObject* parent)
    : QObject(parent), m_name(std::move(name))
{}

RiveBoolInput::RiveBoolInput(QString name, rive::SMIBool* smi, QObject* parent)
    : RiveInput(std::move(name), parent), m_smi(smi)
{}

bool RiveBoolInput::value() const
{
    return m_smi ? m_smi->value() : false;
}

void RiveBoolInput::setValue(bool v)
{
    if (!m_smi || m_smi->value() == v)
        return;
    m_smi->value(v);
    emit valueChanged();
}

RiveNumberInput::RiveNumberInput(QString name, rive::SMINumber* smi, QObject* parent)
    : RiveInput(std::move(name), parent), m_smi(smi)
{}

double RiveNumberInput::value() const
{
    return m_smi ? static_cast<double>(m_smi->value()) : 0.0;
}

void RiveNumberInput::setValue(double v)
{
    if (!m_smi)
        return;
    const float f = static_cast<float>(v);
    if (m_smi->value() == f)
        return;
    m_smi->value(f);
    emit valueChanged();
}

RiveTriggerInput::RiveTriggerInput(QString name, rive::SMITrigger* smi, QObject* parent)
    : RiveInput(std::move(name), parent), m_smi(smi)
{}

void RiveTriggerInput::fire()
{
    if (m_smi)
        m_smi->fire();
}
