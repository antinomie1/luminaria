// Compile-time + runtime checks for the value-semantic Box.
#include <cassert>

import luminaria;
import std;

using luminaria::Box;

static_assert(Box{}.empty());
static_assert(!Box{0, 0, 2, 2}.empty());
static_assert(Box{0, 0, 4, 4}.contains(3, 3));
static_assert(!Box{0, 0, 4, 4}.contains(4, 0));
static_assert(Box{0, 0, 4, 4}.intersection(Box{2, 2, 4, 4}) == Box{2, 2, 2, 2});
static_assert(Box{0, 0, 2, 2}.intersection(Box{5, 5, 1, 1}).empty());

int main() {
    Box a{1, 1, 10, 10};
    assert(a.contains(1, 1));
    assert(!a.contains(11, 11));
    assert(a.intersection(a) == a);
    return 0;
}
