// luminaria/util/rect_fill.hpp — a positioned solid-color rectangle.
// The neutral currency between the scene graph (produces them) and the renderer
// (consumes them). TODO: solid fills only; textured surfaces join when the
// renderer can sample client buffers.
#pragma once

#include "luminaria/util/box.hpp"
#include "luminaria/util/color.hpp"

namespace luminaria {

struct RectFill {
    Box box;
    Color color;
};

} // namespace luminaria
