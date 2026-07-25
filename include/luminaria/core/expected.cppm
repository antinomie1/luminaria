// luminaria/core/expected.cppm — error type + Result<T>.
//
// Design rule: fallible operations return Result<T>; we never throw across the
// libwayland C boundary (C can't unwind). Chain with and_then / or_else / value.

module;

#include <expected>
#include <string>
#include <string_view>
#include <utility>

export module luminaria:core.expected;

export namespace luminaria {

/// A failure value. `code` is an optional errno-style hint (0 == none).
struct Error {
    std::string message;
    int code = 0;
};

/// The result of any fallible luminaria operation.
template <class T>
using Result = std::expected<T, Error>;

/// Result of a fallible operation that yields nothing.
using Status = Result<void>;

/// Build a failure. Usage: `return fail("no vulkan device");`
[[nodiscard]] inline std::unexpected<Error> fail(std::string message, int code = 0) {
    return std::unexpected<Error>(Error{std::move(message), code});
}

/// Success sentinel for Status-returning functions: `return ok();`
[[nodiscard]] inline Status ok() {
    return Status{};
}

} // namespace luminaria
