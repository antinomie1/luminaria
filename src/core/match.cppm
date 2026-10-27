// luminaria/core/match.cppm — one handler per alternative of a variant.
//
// `InputEvent`, and the other variants the library hands out, are visited with
// one overload per alternative. Without a shared helper every consumer wrote
// the same three-line overload set above the call.
//
//     match(event,
//           [&](const KeyEvent& e) { ... },
//           [&](const PointerMotionEvent& e) { ... },
//           [](const auto&) {});          // everything else

module;

export module luminaria:match;

import std;
export namespace luminaria {

/// Several callables presented as one overload set.
template <class... F>
struct Overloaded : F... {
    using F::operator()...;
};
template <class... F>
Overloaded(F...) -> Overloaded<F...>;

/// `std::visit` with the handlers spelled out in place.
template <class Variant, class... F>
constexpr decltype(auto) match(Variant&& variant, F&&... handlers) {
    return std::visit(Overloaded{std::forward<F>(handlers)...}, std::forward<Variant>(variant));
}

} // namespace luminaria
