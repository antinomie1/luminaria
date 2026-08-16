// luminaria/core/handle.cppm — RAII for opaque C handles and file descriptors.
//
// Two wrappers, one job each. `CUnique<T, fn>` is `unique_ptr<T>` whose deleter
// calls `fn(ptr)` (fn may return void or int; both are accepted), for a C pointer
// whose lifetime ends with a single free-function call. `UniqueFd` is the same
// idea for an `int` fd, which `unique_ptr` cannot express because -1, not null,
// is the empty value.
//
// fds are where this library used to leak: a fence fd threaded through a
// function with four early returns needs four `close()` calls, and the fifth
// return added later gets none. Anything that owns an fd should hold a
// `UniqueFd`; a function that only borrows one takes a plain `int`.

module;


#include <unistd.h>

export module luminaria:handle;

import std;
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

/// Owning file descriptor; closes on destruction. Move-only, -1 when empty.
class UniqueFd {
    int fd_ = -1;

public:
    UniqueFd() noexcept = default;
    explicit UniqueFd(int fd) noexcept : fd_(fd) {}

    ~UniqueFd() { reset(); }

    UniqueFd(UniqueFd&& o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
    UniqueFd& operator=(UniqueFd&& o) noexcept {
        if (this != &o) {
            reset(std::exchange(o.fd_, -1));
        }
        return *this;
    }
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    /// The descriptor, or -1. Borrowed: still owned by this object.
    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
    explicit operator bool() const noexcept { return fd_ >= 0; }

    /// Hand ownership to the caller; this object becomes empty.
    [[nodiscard]] int release() noexcept { return std::exchange(fd_, -1); }

    /// Close what we hold (if anything) and take `fd` instead.
    void reset(int fd = -1) noexcept {
        const int old = std::exchange(fd_, fd);
        if (old >= 0 && old != fd) {
            ::close(old);
        }
    }

    /// A second owning handle on the same open file, or an empty one on failure.
    [[nodiscard]] UniqueFd duplicate() const noexcept {
        return fd_ < 0 ? UniqueFd{} : UniqueFd{::dup(fd_)};
    }
};

} // namespace luminaria
