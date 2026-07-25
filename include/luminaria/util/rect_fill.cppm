// luminaria/util/rect_fill.cppm — a positioned solid-color rectangle.
// The neutral currency between the scene graph (produces them) and the renderer
// (consumes them). Solid fills only by design — client surfaces travel as
// GpuTextureFill instead, since they carry a texture, a transform and a crop.
// Kept because the CPU composite path (headless, nested) still speaks it.

module;

export module luminaria:util.rect_fill;

import :util.box;
import :util.color;

export namespace luminaria {

struct RectFill {
    Box box;
    Color color;
};

} // namespace luminaria
