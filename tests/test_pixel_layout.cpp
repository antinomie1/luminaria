// Unit checks for the layout validator every client-memory access goes through.
// Compile-time where possible: these are constexpr, so a regression that makes
// a hostile layout "valid" fails the build rather than the run.
#include <cassert>
#include <cstdint>
#include <limits>

import luminaria;

using luminaria::kBytesPerPixel;
using luminaria::layout_fits;
using luminaria::layout_length;
using luminaria::min_stride;

namespace {

// --- the bug this exists for -------------------------------------------------
// libwayland accepts `stride == width` for a 4-byte format. We must not.
static_assert(!layout_fits(1024, 4, 1024));
static_assert(layout_fits(1024, 4, 1024 * 4));
static_assert(!layout_fits(1024, 4, 1024 * 4 - 1));
static_assert(layout_fits(1024, 4, 1024 * 4 + 7)); // padded rows are fine

// --- degenerate dimensions ---------------------------------------------------
static_assert(!layout_fits(0, 4, 0));
static_assert(!layout_fits(-1, 4, 64));
static_assert(!layout_fits(4, 0, 64));
static_assert(!layout_fits(4, -1, 64));
static_assert(min_stride(0) < 0);
static_assert(min_stride(-1) < 0);
static_assert(min_stride(3) == 3 * kBytesPerPixel);

// --- offsets -----------------------------------------------------------------
static_assert(layout_fits(16, 16, 64, 4096));
static_assert(!layout_fits(16, 16, 64, -1));

// --- overflow ----------------------------------------------------------------
// A huge-but-positive stride/height pair must not wrap into a small length.
constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
static_assert(!layout_fits(16, 1 << 20, kMax));
static_assert(!layout_fits(16, 16, kMax, kMax));
// Dimensions past anything a display could have are refused outright, which is
// what keeps width * 4 from overflowing in the first place.
static_assert(!layout_fits((1 << 24) + 1, 4, kMax));
static_assert(!layout_fits(16, (1 << 24) + 1, 64));

// --- layout_length -----------------------------------------------------------
static_assert(layout_length(16, 16, 64) == 64 * 16);
static_assert(layout_length(16, 16, 64, 128) == 128 + 64 * 16);
// A layout that does not validate must produce 0, so a caller that skipped the
// check maps nothing rather than mapping the wrong size.
static_assert(layout_length(1024, 4, 1024) == 0);
static_assert(layout_length(0, 0, 0) == 0);

} // namespace

int main() {
    // Same assertions again at runtime, so the file is not merely a
    // compile-time artefact and the test binary reports a real pass.
    assert(!layout_fits(1024, 4, 1024));
    assert(layout_fits(1024, 4, 1024 * 4));
    assert(layout_length(1024, 4, 1024) == 0);
    assert(layout_length(1024, 4, 1024 * 4) == 1024ULL * 4 * 4);
    return 0;
}
