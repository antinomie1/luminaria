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

    // Modifier state can be injected wholesale — a nested compositor whose
    // host changed focus while a modifier was held never saw the key event,
    // so the host's masks are the only truth. The observed shift mask round-
    // trips and the keysym follows it exactly as if the key were still down.
    km->update_key(42, true);
    const luminaria::ModifiersEvent held_shift = km->modifiers();
    assert(held_shift.depressed != 0);
    assert(km->keysym(30) == XKB_KEY_A);
    km->update_key(42, false);          // the local view releases it...
    assert(km->keysym(30) == XKB_KEY_a);
    km->update_modifiers(held_shift);   // ...the host says it is still held
    assert(km->modifiers().depressed == held_shift.depressed);
    assert(km->keysym(30) == XKB_KEY_A);
    km->update_modifiers(luminaria::ModifiersEvent{0, 0, 0, 0});
    assert(km->modifiers().depressed == 0);
    assert(km->keysym(30) == XKB_KEY_a);

    // The layout group is part of the injected state too: a two-group layout
    // answers the same key in the other group once told, and the group sticks
    // until changed again.
    auto ru = luminaria::KeymapState::from_layout("us,ru");
    assert(ru.has_value());
    assert(ru->modifiers().group == 0);
    assert(ru->keysym(16) == XKB_KEY_q);
    ru->update_modifiers(luminaria::ModifiersEvent{0, 0, 0, 1});
    assert(ru->modifiers().group == 1);
    assert(ru->keysym(16) != XKB_KEY_q); // the same key is now the Cyrillic one

    // Garbage is refused, not crashed on.
    assert(!luminaria::KeymapState::from_text("not a keymap").has_value());
    return 0;
}
