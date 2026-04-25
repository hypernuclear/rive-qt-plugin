#include "rive_key_map.h"

#include <QtCore/Qt>

namespace rive_key_map {

MappedKey fromQtKey(int qtKey)
{
    MappedKey out;
    out.valid = true;

    // ASCII-range letters and digits line up 1:1 between Qt::Key_* and
    // rive::Key. Handle that range first, then named keys.
    if (qtKey >= Qt::Key_A && qtKey <= Qt::Key_Z)
    {
        out.key = static_cast<rive::Key>(
            static_cast<uint16_t>(rive::Key::a) + (qtKey - Qt::Key_A));
        return out;
    }
    if (qtKey >= Qt::Key_0 && qtKey <= Qt::Key_9)
    {
        out.key = static_cast<rive::Key>(
            static_cast<uint16_t>(rive::Key::key0) + (qtKey - Qt::Key_0));
        return out;
    }
    if (qtKey >= Qt::Key_F1 && qtKey <= Qt::Key_F25)
    {
        out.key = static_cast<rive::Key>(
            static_cast<uint16_t>(rive::Key::f1) + (qtKey - Qt::Key_F1));
        return out;
    }

    switch (qtKey)
    {
    case Qt::Key_Space:       out.key = rive::Key::space; return out;
    case Qt::Key_Apostrophe:  out.key = rive::Key::apostrophe; return out;
    case Qt::Key_Comma:       out.key = rive::Key::comma; return out;
    case Qt::Key_Minus:       out.key = rive::Key::minus; return out;
    case Qt::Key_Period:      out.key = rive::Key::period; return out;
    case Qt::Key_Slash:       out.key = rive::Key::slash; return out;
    case Qt::Key_Semicolon:   out.key = rive::Key::semicolon; return out;
    case Qt::Key_Equal:       out.key = rive::Key::equal; return out;
    case Qt::Key_BracketLeft: out.key = rive::Key::leftBracket; return out;
    case Qt::Key_Backslash:   out.key = rive::Key::backslash; return out;
    case Qt::Key_BracketRight: out.key = rive::Key::rightBracket; return out;
    case Qt::Key_QuoteLeft:   out.key = rive::Key::graveAccent; return out;

    case Qt::Key_Escape:    out.key = rive::Key::escape; return out;
    case Qt::Key_Return:
    case Qt::Key_Enter:     out.key = rive::Key::enter; return out;
    case Qt::Key_Tab:       out.key = rive::Key::tab; return out;
    case Qt::Key_Backspace: out.key = rive::Key::backspace; return out;
    case Qt::Key_Insert:    out.key = rive::Key::insert; return out;
    case Qt::Key_Delete:    out.key = rive::Key::deleteKey; return out;
    case Qt::Key_Right:     out.key = rive::Key::right; return out;
    case Qt::Key_Left:      out.key = rive::Key::left; return out;
    case Qt::Key_Down:      out.key = rive::Key::down; return out;
    case Qt::Key_Up:        out.key = rive::Key::up; return out;
    case Qt::Key_PageUp:    out.key = rive::Key::pageUp; return out;
    case Qt::Key_PageDown:  out.key = rive::Key::pageDown; return out;
    case Qt::Key_Home:      out.key = rive::Key::home; return out;
    case Qt::Key_End:       out.key = rive::Key::end; return out;
    case Qt::Key_CapsLock:  out.key = rive::Key::capsLock; return out;
    case Qt::Key_ScrollLock: out.key = rive::Key::scrollLock; return out;
    case Qt::Key_NumLock:   out.key = rive::Key::numLock; return out;
    case Qt::Key_Print:     out.key = rive::Key::printScreen; return out;
    case Qt::Key_Pause:     out.key = rive::Key::pause; return out;

    case Qt::Key_Shift:     out.key = rive::Key::leftShift; return out;
    case Qt::Key_Control:   out.key = rive::Key::leftControl; return out;
    case Qt::Key_Alt:       out.key = rive::Key::leftAlt; return out;
    case Qt::Key_Meta:      out.key = rive::Key::leftSuper; return out;
    case Qt::Key_Menu:      out.key = rive::Key::menu; return out;

    default:
        out.valid = false;
        return out;
    }
}

rive::KeyModifiers fromQtModifiers(int qtModifiers)
{
    uint8_t m = 0;
    if (qtModifiers & Qt::ShiftModifier)
        m |= static_cast<uint8_t>(rive::KeyModifiers::shift);
    if (qtModifiers & Qt::ControlModifier)
        m |= static_cast<uint8_t>(rive::KeyModifiers::ctrl);
    if (qtModifiers & Qt::AltModifier)
        m |= static_cast<uint8_t>(rive::KeyModifiers::alt);
    if (qtModifiers & Qt::MetaModifier)
        m |= static_cast<uint8_t>(rive::KeyModifiers::meta);
    return static_cast<rive::KeyModifiers>(m);
}

} // namespace rive_key_map
