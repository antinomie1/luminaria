// luminaria/core/expected.cppm — error type + Result<T>.
//
// Design rule: fallible operations return Result<T>; we never throw across the
// libwayland C boundary (C can't unwind). Chain with and_then / or_else / value.

module;

export module luminaria:expected;

import std;
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

/// "Fine if it can't be built": the value if there is one, nothing otherwise.
/// An optional global is `if (auto d = to_optional(X::create(...))) ...` instead
/// of an `if (auto r = ...; r)` with the value moved out by hand.
template <class T>
[[nodiscard]] std::optional<T> to_optional(Result<T> result) {
    if (!result) {
        return std::nullopt;
    }
    return std::move(*result);
}

/// For a `main()` with nothing better to do with a failure: the value, or
/// "`what`: message" on stderr and exit(1). Never from library code, and never
/// from anything that runs after startup.
template <class T>
T must(Result<T> result, std::string_view what) {
    if (!result) {
        std::println(std::cerr, "{}: {}", what, result.error().message);
        std::exit(1);
    }
    if constexpr (!std::is_void_v<T>) {
        return std::move(*result);
    }
}

/// Success sentinel for Status-returning functions: `return ok();`
[[nodiscard]] inline Status ok() {
    return Status{};
}

} // namespace luminaria
