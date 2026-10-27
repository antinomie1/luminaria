// match() and to_optional(): the two small helpers a consumer reaches for
// every time it takes an event apart or builds something that may fail.
#include <cassert>

import luminaria;
import std;

namespace lm = luminaria;

int main() {
    lm::InputEvent key = lm::KeyEvent{.keycode = 1, .pressed = true};
    lm::InputEvent button = lm::PointerButtonEvent{272, false};
    int keys = 0, buttons = 0, others = 0;
    const auto count = [&](const lm::InputEvent& event) {
        lm::match(
            event, [&](const lm::KeyEvent&) { ++keys; },
            [&](const lm::PointerButtonEvent& e) { buttons += e.button == 272; },
            [&](const auto&) { ++others; });
    };
    count(key);
    count(button);
    count(lm::InputEvent{lm::PointerLeaveEvent{}});
    assert(keys == 1 && buttons == 1 && others == 1);

    // The handlers can return a value, like std::visit.
    const int code = lm::match(
        key, [](const lm::KeyEvent& e) { return static_cast<int>(e.keycode); },
        [](const auto&) { return -1; });
    assert(code == 1);

    assert(lm::to_optional(lm::Result<int>{5}) == std::optional<int>{5});
    assert(!lm::to_optional(lm::Result<int>{lm::fail("no")}).has_value());
    return 0;
}
