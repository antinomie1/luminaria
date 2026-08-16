// KeymapState: the shared xkb wrapper. A compositor (or a test) drives it with
// raw evdev keycodes and reads keysyms and modifier masks back — the exact
// state a shortcut handler needs, with none of the xkb bookkeeping.
#include <cassert>

#include <xkbcommon/xkbcommon.h>

import luminaria;
import std;

int main() {
    auto km = luminaria::KeymapState::from_layout("us");
    assert(km.has_value());
    assert(!km->text().empty());

    // KEY_A (evdev 30) alone is lowercase a.
    assert(km->keysym(30) == XKB_KEY_a);
    // Left shift (evdev 42) down: the keysym follows, and the mask moves.
    km->update_key(42, true);
    assert(km->keysym(30) == XKB_KEY_A);
    assert(km->modifiers().depressed != 0);
    // Released: back to lowercase, masks empty again.
    km->update_key(42, false);
    assert(km->keysym(30) == XKB_KEY_a);
    assert(km->modifiers().depressed == 0);

    // The text form round-trips — what a compositor forwards to clients and a
    // nested compositor receives from its parent compiles to the same state.
    auto clone = luminaria::KeymapState::from_text(km->text());
    assert(clone.has_value());
    assert(clone->keysym(30) == XKB_KEY_a);

    // Garbage is refused, not crashed on.
    assert(!luminaria::KeymapState::from_text("not a keymap").has_value());
    return 0;
}
