#ifndef RIVE_KEY_MAP_H
#define RIVE_KEY_MAP_H

// Qt ↔ rive input conversions. Kept as free functions so both the view
// and any tests can reach them without coupling to RiveArtboard.

#include <rive/input/focusable.hpp>

class QKeyEvent;

namespace rive_key_map {

// Returns a valid rive::Key for the given Qt::Key, or std::nullopt for
// keys rive doesn't recognize (media keys, etc.). Caller should fall
// through to normal Qt handling when this returns nullopt.
struct MappedKey
{
    rive::Key key;
    bool valid = false;
};

MappedKey fromQtKey(int qtKey);

rive::KeyModifiers fromQtModifiers(int qtModifiers);

} // namespace rive_key_map

#endif // RIVE_KEY_MAP_H
