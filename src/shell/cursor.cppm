// luminaria/shell/cursor.cppm — the pointer and its picture, end to end.
//
// Every compositor used to write this the same way, three times over in this
// repository alone: load a theme, follow cursor-shape requests and the client's
// own cursor surface, forget the shape when the pointer moves to another
// client, move the KMS cursor plane or repaint wherever there is none, re-upload
// an animated client sprite on each of its commits, and describe the pointer to
// screen recorders. None of it is policy. All of it went subtly wrong in at
// least one copy — the plane showing a sprite after it was hidden, a text
// field's I-beam following the pointer across the screen, an animated cursor
// frozen on the plane because nothing watched its commits.
//
// Where the pointer goes stays the compositor's: input routing, pointer
// constraints and which output a device maps to are decisions, so `move_to()`
// and `move_by()` take the position from the caller, and the bounds only when a
// pointer constraint narrows them. What the
// pointer looks like, and getting that onto every screen, is this type's.
//
// It draws through `SceneRenderer`: it installs itself as the renderer's cursor
// source, so `SceneRenderer::present()` composites it wherever the plane does
// not carry it, and calls `update_cursor()` whenever it moved or changed.
export module luminaria.shell:cursor;

import std;

import luminaria;
import luminaria.render;

import :cursor_manager;
import :cursor_theme;
import :pointer;
import :scene;
import :scene_renderer;

export namespace luminaria {

class Cursor {
public:
    /// Load the cursor theme and start following `seat`'s pointer picture.
    ///
    /// Everything passed in is borrowed for the cursor's whole life, so declare
    /// it after all of them. `shapes` (cursor-shape-v1) and `capture`
    /// (screencopy, told whenever the pointer changes) may be null. No theme
    /// installed is not an error: the pointer is then whatever clients set, or
    /// nothing.
    ///
    /// It follows `layout` by itself: an output that arrives, leaves, moves or
    /// changes mode gets the pointer pushed to it again, and the first output
    /// to arrive finds it in its middle until a device says otherwise.
    Cursor(SceneRenderer& renderer, OutputLayout& layout, Seat& seat,
           CursorShapeManager* shapes = nullptr, ScreencopyManager* capture = nullptr);
    ~Cursor();
    Cursor(Cursor&&) noexcept;
    Cursor& operator=(Cursor&&) noexcept;
    Cursor(const Cursor&) = delete;
    Cursor& operator=(const Cursor&) = delete;

    /// Where the pointer is, in global logical coordinates.
    [[nodiscard]] const Pointer& pointer() const noexcept;
    /// The loaded theme, or null when there is none.
    [[nodiscard]] const CursorTheme* theme() const noexcept;

    /// Put the pointer at (`x`, `y`), clamped into `bounds` — the whole layout
    /// unless a pointer constraint says otherwise. Returns whether it moved.
    bool move_to(double x, double y);
    bool move_to(double x, double y, const Box& bounds);
    /// Move it by a relative delta (libinput), starting from the middle of
    /// `bounds` the first time. Returns whether it moved.
    bool move_by(double dx, double dy);
    bool move_by(double dx, double dy, const Box& bounds);
    /// The same from the backend's event. Absolute motion is in the coordinates
    /// of the output the event names, or of the layout's primary output when it
    /// names none.
    bool move_to(const PointerMotionAbsEvent& event);
    bool move_by(const PointerMotionEvent& event) { return move_by(event.dx, event.dy); }
    /// The pointer left every output (the nested window, say). Draws nothing
    /// until the next move.
    void leave();

    /// Push the pointer to every output again, for a change the cursor cannot
    /// see. Layout changes it sees by itself.
    void refresh();

    /// The sprite as `output` shows it, in that output's logical coordinates;
    /// empty when the pointer is hidden or nowhere near this output.
    [[nodiscard]] SceneCursor on(const Output& output) const;

    /// What ext-image-copy-capture's cursor sessions want: the pixels and the
    /// hotspot's position in `output`'s pixels. False when the pointer is not on
    /// `output` or shows nothing. The compositor resolves which output a
    /// session's wl_output names and calls this from
    /// `ScreencopyManager::set_cursor_source()`.
    [[nodiscard]] bool capture(const Output& output, CursorCapture& out) const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace luminaria

// --------------------------------------------------------------- implementation
namespace luminaria {

struct Cursor::Impl {
    SceneRenderer* renderer = nullptr;
    const OutputLayout* layout = nullptr;
    Seat* seat = nullptr;
    ScreencopyManager* capture = nullptr;
    CursorManager images;
    Pointer pointer;
    // Whether anything has put the pointer somewhere yet, so a pointer that
    // left the window is not pulled back to the middle by a mode change.
    bool placed = false;

    // The client sprite whose commits are being watched: on a cursor plane no
    // frame draws it, so nothing else would notice a new animation frame.
    SurfaceId sprite;
    Signal<SurfaceCommit>::Connection on_sprite_commit;
    Signal<CursorShapeRequest>::Connection on_shape;
    Signal<SeatCursorChange>::Connection on_cursor;
    Signal<SeatPointerFocus>::Connection on_focus;
    Signal<OutputLayoutChange>::Connection on_layout;

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    ~Impl() {
        if (renderer != nullptr) {
            renderer->set_cursor_source({});
        }
    }

    /// The picture or the position changed: every screen, and every recorder.
    void update() {
        renderer->update_cursor();
        if (capture != nullptr) {
            capture->notify_cursor_changed();
        }
    }

    void watch_sprite() {
        const SurfaceId now = seat->cursor_surface();
        if (now == sprite) {
            return;
        }
        sprite = now;
        on_sprite_commit = {};
        if (Surface* surface = surface_from_id(now); surface != nullptr) {
            on_sprite_commit = surface->commit.connect([this](SurfaceCommit&) { update(); });
        }
    }

    /// The sprite in global coordinates, and its size; nothing to draw when
    /// the box is empty.
    [[nodiscard]] std::pair<SceneCursor, Box> global() const {
        const SceneCursor sprite_now = images.sprite(pointer, *seat);
        int width = 0;
        int height = 0;
        if (const Surface* surface = surface_from_id(sprite_now.surface); surface != nullptr) {
            width = surface->surface_width();
            height = surface->surface_height();
        } else if (sprite_now.image != nullptr) {
            width = sprite_now.image->width;
            height = sprite_now.image->height;
        }
        return {sprite_now, {sprite_now.x, sprite_now.y, width, height}};
    }

    /// The sprite in `output`'s logical coordinates, or nothing when it does
    /// not touch that output — so a cursor plane on the other monitor is taken
    /// down rather than parked at a coordinate off its edge.
    [[nodiscard]] SceneCursor on(const Output& output) const {
        const auto [sprite_now, box] = global();
        const Box view = layout->box_of(output);
        if (box.intersection(view).empty()) {
            return {};
        }
        SceneCursor local = sprite_now;
        local.x -= view.x;
        local.y -= view.y;
        return local;
    }
};

Cursor::Cursor(SceneRenderer& renderer, OutputLayout& layout, Seat& seat,
               CursorShapeManager* shapes, ScreencopyManager* capture)
    : impl_(std::make_unique<Impl>()) {
    Impl& self = *impl_;
    self.on_layout = layout.changed.connect([&self] {
        if (!self.placed && !self.layout->empty()) {
            self.placed = true;
            const Point middle = self.layout->primary_box().center();
            self.pointer.move_to(middle.x, middle.y, self.layout->bounds());
        }
        self.update();
    });
    self.renderer = &renderer;
    self.layout = &layout;
    self.seat = &seat;
    self.capture = capture;
    if (Result<CursorManager> images = CursorManager::create(); images) {
        self.images = std::move(*images);
    }
    // A named shape belongs to the client that asked for it, and a client
    // sprite replaces the shape; leaving the client that set either goes back
    // to the theme's arrow, or a text field's I-beam follows the pointer across
    // the screen.
    if (shapes != nullptr) {
        self.on_shape = shapes->request().connect([&self](CursorShapeRequest& request) {
            self.images.set_shape(request.name);
            self.update();
        });
    }
    self.on_cursor = seat.cursor_changed().connect([&self](SeatCursorChange&) {
        self.images.set_surface_cursor();
        self.watch_sprite();
        self.update();
    });
    self.on_focus = seat.pointer_focus_changed().connect([&self](SeatPointerFocus&) {
        self.images.set_shape("default");
        self.update();
    });
    renderer.set_cursor_source([&self](const Output& output) { return self.on(output); });
}

Cursor::~Cursor() = default;
Cursor::Cursor(Cursor&&) noexcept = default;
Cursor& Cursor::operator=(Cursor&&) noexcept = default;

const Pointer& Cursor::pointer() const noexcept { return impl_->pointer; }

const CursorTheme* Cursor::theme() const noexcept {
    const std::optional<CursorTheme>& theme = impl_->images.theme();
    return theme.has_value() ? &*theme : nullptr;
}

bool Cursor::move_to(double x, double y) { return move_to(x, y, impl_->layout->bounds()); }

bool Cursor::move_to(double x, double y, const Box& bounds) {
    impl_->placed = true;
    if (!impl_->pointer.move_to(x, y, bounds)) {
        return false;
    }
    impl_->update();
    return true;
}

bool Cursor::move_to(const PointerMotionAbsEvent& event) {
    const Output* on = event.output != nullptr ? event.output : impl_->layout->primary();
    const Box origin = on != nullptr ? impl_->layout->box_of(*on) : Box{};
    return move_to(origin.x + event.x, origin.y + event.y);
}

bool Cursor::move_by(double dx, double dy) { return move_by(dx, dy, impl_->layout->bounds()); }

bool Cursor::move_by(double dx, double dy, const Box& bounds) {
    impl_->placed = true;
    if (!impl_->pointer.move_by(dx, dy, bounds)) {
        return false;
    }
    impl_->update();
    return true;
}

void Cursor::leave() {
    impl_->placed = true;
    if (impl_->pointer.leave()) {
        impl_->update();
    }
}

void Cursor::refresh() { impl_->update(); }

SceneCursor Cursor::on(const Output& output) const { return impl_->on(output); }

bool Cursor::capture(const Output& output, CursorCapture& out) const {
    const Impl& self = *impl_;
    const Pointer& pointer = self.pointer;
    const Box view = self.layout->box_of(output);
    if (!pointer.present() ||
        !view.contains(static_cast<int>(pointer.x()), static_cast<int>(pointer.y()))) {
        return false;
    }
    const SceneCursor sprite = self.images.sprite(pointer, *self.seat);
    if (Surface* client = surface_from_id(sprite.surface); client != nullptr) {
        if (!client->current_buffer_rgba(out.rgba, out.width, out.height)) {
            return false;
        }
        out.hotspot_x = self.seat->cursor_hotspot_x();
        out.hotspot_y = self.seat->cursor_hotspot_y();
    } else if (sprite.image != nullptr) {
        out.rgba = sprite.image->rgba;
        out.width = sprite.image->width;
        out.height = sprite.image->height;
        out.hotspot_x = sprite.image->hotspot_x;
        out.hotspot_y = sprite.image->hotspot_y;
    } else {
        return false;
    }
    const int scale = std::max(1, output.scale());
    out.x = static_cast<int>(pointer.x() - view.x) * scale;
    out.y = static_cast<int>(pointer.y() - view.y) * scale;
    return true;
}

} // namespace luminaria
