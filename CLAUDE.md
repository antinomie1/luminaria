# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Luminaria: a minimal Wayland compositor **library** in modern C++23, built on top of
`libwayland-server` (the wire protocol is never reimplemented), rendering with Vulkan-Hpp,
built with xmake. Roughly wlroots-shaped but far smaller (20 protocol types vs wlroots' 73).

It ships as a **C++20 named module**. `import luminaria;` is the entire interface — there are
no public headers.
`README.md` (Chinese) holds the current feature matrix and the roadmap of missing protocols;
`TODO-CORNERS-CUT.md` (Chinese) is an audit of every TODO and every deliberate no-op stub.

## Build / test / run

```sh
xmake f -y                # configure (also runs wayland-scanner + glslangValidator)
xmake                     # build the library and the examples
xmake build -a            # ...and every test binary
xmake test                # run all tests
xmake test test_dmabuf/*  # run one
xmake build tinyluminaria # one target
xmake f -m release        # optimised build; debug is the default
```

Warnings are fatal (`all`, `extra`, `pedantic`, `error`), minus
`-Wno-missing-field-initializers` for the libwayland vtable idiom. Requires **gcc ≥ 16** —
older compilers cannot build the module partitions — plus `glslangValidator` and `libseat`.

**Why xmake and not Meson.** Meson's module dependency scanner hardcodes MSVC-shaped `.ifc`
output names (`mesonbuild/scripts/depscan.py`), so ninja dies with "inputs may not also have
inputs" on GCC. xmake scans and orders module units correctly for GCC; that is the whole
reason for the switch.

Generated code (`wayland-scanner` glue, SPIR-V arrays) lands in `build/generated/` at
configure time and is regenerated only when its input is newer.

## Modules

- `include/luminaria.cppm` is the primary interface unit. It does nothing but
  `export import` every partition.
- `include/luminaria/**.cppm` is one **interface partition** per former public header, named
  after its path: `util/box.cppm` is `luminaria:util.box`. Consumers never name a partition.
- `src/**.cpp` are **implementation units** (`module luminaria;`). They implicitly import the
  primary interface, so they need no imports of their own — only their C and std includes,
  which must sit in the global module fragment above `module luminaria;`.
- Anything a unit `#include`s belongs in its **global module fragment**: `module;` first, then
  the includes, then `export module …`. Declaring a C type after `export module` attaches it
  to module luminaria and makes it a *different type* from the one the C headers declare.
  That is why the opaque forward declarations live in
  `include/luminaria/detail/wayland_fwd.h` and are `#include`d — declaring them inline in the
  fragment trips `-Wglobal-module`.
- Two gcc-16 quirks worth knowing before you hit them: a defaulted **hidden-friend**
  `operator==` in an interface unit ICEs (use the member form), and `std::function` /
  `std::make_shared` want `<typeinfo>` visible at every point of instantiation rather than
  inheriting it the way a header did.
- Consumers that also talk to libwayland directly (a test acting as a client) include those C
  headers themselves and get the same types — the forward declarations are attached to the
  global module precisely so that works.

Running the example compositors:

```sh
WAYLAND_DISPLAY=wayland-0 ./build/examples/tinyluminaria   # nested; prints its own socket name
LUMINARIA_BACKEND=headless ./build/examples/tinyluminaria  # force headless
LUMINARIA_EXIT_MS=200 ...                                  # auto-terminate (used by smoke test)
./build/examples/luminaria-tty                             # bare-metal DRM+libinput, needs a free VT
```

Test environment notes: most tests drive real protocol traffic through an **in-process
`libwayland-client` on a socketpair** (no GPU, no parent compositor) — see `tests/test_compositor.cpp`
for the pattern: client thread on `wl_display_connect_to_fd`, server terminates on client
disconnect. `vulkan`/`composite`/`texture` need a real GPU; `wayland-nested` needs `WAYLAND_DISPLAY`;
`drm` needs a tty and skips otherwise.

## Architecture

Layers, bottom-up (`src/` mirrors `include/luminaria/`):

- **core** — `Display` (owns `wl_display` + socket + main loop), `EventLoop`/`EventSource`
  (non-owning view + RAII timer/fd sources), `Signal<Event>`, `Result<T>`, `CUnique`.
- **backend** — `Backend` emits `new_output`; `Output` emits `frame` and accepts
  `commit(Color)` / `commit_frame(rgba)`. Implementations: `HeadlessBackend` (software frame
  pump), `WaylandBackend` (nested window in a parent compositor, forwards parent input AND
  its keymap), `DrmBackend` (KMS atomic modeset, one Output per connected connector kept live
  by a udev hotplug monitor, `import_scanout`/`commit_scanout(id, in_fence)` for GPU dmabuf
  framebuffers, plus an optional hardware cursor plane), `LibinputBackend` (bare-metal input
  signals). Every `Output` carries a `scale` and a `Transform`; `Output::destroy` fires when a
  monitor goes away. `Session` (libseat, `backend/session.cpp`) owns the device fds and tells
  both bare-metal backends when the VT is taken away.
- **types/** — one file per Wayland global: `compositor` (`wl_compositor`/`wl_surface`,
  including the subsurface tree), `subcompositor`, `xdg_shell` (toplevels + popups +
  positioners), `seat` (keyboard/pointer/touch/cursor/DnD hooks), `output_global`,
  `linux_dmabuf`, `screencopy`, `data_device` (clipboard + DnD + primary selection),
  `drm_syncobj` (explicit sync), `single_pixel_buffer`, `presentation_time`,
  `tearing_control`, `cursor_shape`, `workspace` (ext-workspace), `xdg_decoration`,
  `viewporter`, `fractional_scale`, `layer_shell` (wlr-layer-shell, plus the
  `arrange_layer_surface()` anchor/exclusive-zone solver), `foreign_toplevel`
  (wlr-foreign-toplevel-management, mirrors an `XdgShell` on its own via
  `track()`), `xdg_activation`.
- **render/vulkan** — two paths. GPU: `GpuTexture` (client dmabuf imported with no copy, or
  shm pixels uploaded once) drawn by `render_to()` as textured quads into a `ScanoutTarget`,
  a render target allocated with a DRM format modifier and exported as a dmabuf. Its inputs
  are in the output's *logical* coordinates; the `OutputMapping` (scale + transform) turns
  them into pixels, which is where rotation actually happens (per-corner UVs). Shaders live
  in `src/render/quad.{vert,frag}` and are compiled to SPIR-V arrays at build time by
  `glslangValidator --vn` — a hard build dependency. CPU: the older `composite()` returning
  an RGBA `std::vector<Pixel>`, kept for headless/screencopy; it cannot scale or rotate.
  Both rest on `VK_EXT_external_memory_dma_buf` + `VK_EXT_image_drm_format_modifier`.
  `render_to` scissors each *disjoint* damage box separately (a `Region`, not a bounding
  box), skips anything hidden behind a `GpuTextureFill::opaque` rect, and — given a
  `RenderSync` — waits on client acquire fences as `VkSemaphore`s and hands back a
  sync_file instead of stalling. Unfinished submits live in `Impl::in_flight` until their
  fence clears.
- **scene** — retained tree (Tree/Rect/Surface), positioning, hit-testing, flattening to
  `RectFill`s and `GpuTextureFill`s, and damage (`scene_damage()` →
  `scene_rects(root, damage)` / `scene_textures(root, renderer, damage)` →
  `scene_clear_damage()`); `OutputLayout` (`scene/output_layout.cpp`) is the one place
  that knows where each output sits in the global coordinate space, in *logical* units
  (`util/transform.hpp` holds the logical↔device mapping and nothing else does).
  `util/region.hpp` is the disjoint-box set both damage and wl_region are built on.
- **xwayland** — spawns Xwayland plus a minimal XWM over xcb.

Per-frame flow in a compositor built on this (see `examples/tinyluminaria.cpp`,
`examples/tty_compositor.cpp`): `Output::frame` fires → walk mapped toplevels, each expanded
via `Surface::surface_tree()` (the surface plus its subsurfaces, back-to-front), then popups
anchored to their parents, then the cursor → `Surface::current_buffer_texture()` puts each
client buffer on the GPU → `VulkanRenderer::render_to(ScanoutTarget)` → `Output::commit_scanout()`.
Backends without dmabuf scanout (headless, nested) take the CPU variant instead:
`current_buffer_rgba()` → `composite()` → `commit_frame()`. Input goes
the other way: backend input signal → scene/manual hit test → `Seat` focus + event routing.
`tinyluminaria` builds one z-ordered layer list per frame and uses it for BOTH rendering and
hit-testing, so clicks can't disagree with pixels.

Two bridges out of client buffers, and new code should prefer the first:
`Surface::current_buffer_texture()` (dmabuf → `VulkanRenderer::import_texture`, everything else
uploaded) never reads pixels back; `Surface::current_buffer_rgba()` (shm, then single-pixel, then
`dmabuf_buffer_to_rgba()`) does, and exists for screencopy and the non-dmabuf backends.
What is still missing on the GPU path: direct scanout of a *client* buffer, a hardware cursor
plane, and passing fences instead of blocking on them (`render_to` waits on a fence today).

**Surface coordinates are not buffer pixels.** A client can hand over a denser buffer
(`set_buffer_scale`), a rotated one (`set_buffer_transform`), or crop and stretch it
(`wp_viewporter`). Layout, hit-testing and subsurface offsets all use
`Surface::surface_width()/surface_height()`; only code that touches pixels wants
`buffer_width()`. Hit-testing goes through `Surface::accepts_input()` so the client's input
region decides, not the buffer rectangle.

**The renderer must outlive the Display.** `Surface::buffer_texture()` caches a `GpuTexture`
owned by the `VulkanRenderer`, so the renderer has to be declared *before* the `Display`
(locals are destroyed in reverse order). Both examples do this with a comment; getting it
wrong aborts inside `vk::raii` at shutdown.

**Frame callbacks are the compositor's job.** `wl_surface.frame` is NOT answered on commit —
answering there makes clients render frames the display never shows. They queue until the
compositor calls `Surface::send_frame_done(time_ms)`, which belongs in the `Output::present`
handler (alongside `Presentation::notify_presented`). Forget it and every client freezes after
one frame. Damage works the same way: `Surface::damage()` accumulates until whoever rendered it
calls `clear_damage()`.

## Conventions that matter here

- **No exceptions across the C boundary.** Fallible operations return `Result<T>` /
  `Status` (`fail("msg")`, `ok()`). Vulkan-Hpp throws internally; every `VulkanRenderer`
  method catches at its boundary and converts to `Result`.
- **Public headers are C-header-free.** `wl_display`, `wl_resource`, etc. are forward-declared
  only; all libwayland/Vulkan/xcb includes live in `.cpp`. Protocol globals use a pimpl
  (`struct Impl` declared public, defined in the `.cpp`) so their address is stable for the
  `wl_global` user-data pointer, while the wrapper stays move-only.
- **Lifetime via RAII, never manual list surgery.** `Signal<E>::Connection` disconnects on
  destruction and survives either destruction order; C handles are wrapped in `CUnique<T, fn>`.
  There should be no `wl_list_remove` calls in compositor code.
- **Null request slots abort libwayland.** Real GTK/Qt clients call requests we haven't
  implemented, and libwayland aborts on a null slot — so unimplemented requests are wired to
  explicit no-op functions (`surface_noop_*` in `src/types/compositor.cpp`, `tl_set_parent` /
  `tl_show_window_menu`). When adding a protocol, fill every slot in the interface vtable.
  `-Wno-missing-field-initializers` is set project-wide for exactly this idiom.
- **Raw `Surface*` needs a `Surface::destroy` subscription.** Anything caching a surface
  pointer (seat focus, cursor, drag focus, scene nodes) connects to `Surface::destroy` and
  clears the pointer there; the `Signal::Connection` is RAII so it can't outlive the holder.
  `src/types/seat.cpp` is the reference for the pattern.
- **Any retained `wl_resource*` owned by a CLIENT needs a destroy listener.** Buffers are
  the sharp edge: toolkits drop their whole swapchain on resize / hide / re-show, so a
  committed `wl_buffer` dies under you and the next `wl_buffer.release` or readback
  segfaults. `Surface::BufferWatch` (`src/types/compositor.cpp`) and `ExtFrame`'s
  `buffer_destroy` (`src/types/screencopy.cpp`) show the shape:
  `wl_resource_add_destroy_listener` + null the slot. `tests/test_buffer_destroy.cpp`
  guards it. This is the one place raw libwayland listeners are correct — the C signal is
  on the client's resource, not on one of our `Signal`s.

## Adding a protocol

1. Stable/unstable protocols come from `wayland-protocols` (path resolved via pkgdata);
   ones not shipped there live as XML in `protocol/`.
2. Add a row to the `protocols` (or `local_protocols`) table in `xmake.lua`: name, path, and
   which halves to generate — `s` server header, `c` private code, `l` client header for
   tests that act as clients.
3. Write `src/types/<name>.cpp` as an implementation unit and
   `include/luminaria/<name>.cppm` as an interface partition, following the pimpl + `Result`
   pattern above. Both are picked up by the existing globs; nothing else to register.
4. Add `export import :<name>;` to `include/luminaria.cppm`.
5. Drop a test in `tests/` — `xmake.lua` turns every `tests/test_*.cpp` into its own binary
   automatically. Exit 77 to skip when the machine cannot run it.

An XML argument name becomes a C parameter name verbatim, and everything here that
consumes the generated headers is C++ — so an argument called `namespace`, `class`,
`new`, `template`… simply does not compile. The name never appears on the wire, so the
fix for a protocol whose XML we vendor is to rename the argument in `protocol/` with a
comment saying why: `wlr-layer-shell-unstable-v1.xml` has `namespace` renamed to `scope`.
