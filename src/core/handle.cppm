// luminaria/core/handle.cppm — RAII for opaque C handles.
//
// Wraps a C pointer whose lifetime ends with a single free-function call, so no
// luminaria code ever leaks a C handle. `CUnique<T, fn>` is `unique_ptr<T>` whose
// deleter calls `fn(ptr)` (fn may return void or int; both are accepted).

module;

#include <memory>

export module luminaria:handle;
export namespace luminaria {

/// Deleter that invokes the free function `Fn` on a non-null pointer.
template <auto Fn>
struct FnDeleter {
    template <class T>
    void operator()(T* p) const noexcept {
        if (p) {
            Fn(p);
        }
    }
};

/// `unique_ptr<T>` that frees via the C function `Fn`.
/// e.g. `using UniqueDisplay = CUnique<wl_display, wl_display_destroy>;`
template <class T, auto Fn>
using CUnique = std::unique_ptr<T, FnDeleter<Fn>>;

} // namespace luminaria
