#include <cassert>
#include <cstdint>

import luminaria;
import std;

int main() {
    luminaria::SurfaceId stale;
    std::uint32_t reused_index = 0;

    {
        luminaria::Surface first{nullptr};
        stale = first.id();
        reused_index = stale.index;
        assert(stale.valid());
        assert(luminaria::surface_from_id(stale) == &first);
    }

    assert(luminaria::surface_from_id(stale) == nullptr);

    // The just-freed slot is reused first. Its new generation must keep the
    // old id null instead of resolving to this unrelated Surface (ABA).
    luminaria::Surface replacement{nullptr};
    const luminaria::SurfaceId current = replacement.id();
    assert(current.index == reused_index);
    assert(current.generation != stale.generation);
    assert(luminaria::surface_from_id(stale) == nullptr);
    assert(luminaria::surface_from_id(current) == &replacement);

    assert(luminaria::surface_from_id({}) == nullptr);
    return 0;
}
