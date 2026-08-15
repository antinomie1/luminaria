# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Luminaria: a minimal Wayland compositor **library** in modern C++23, built on top of
`libwayland-server` (the wire protocol is never reimplemented), rendering with Vulkan-Hpp,
built with xmake. Roughly wlroots-shaped but far smaller (25 protocol types vs wlroots' 73).

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
`-Wno-missing-field-initializers` for the libwayland vtable idiom. Needs **clang ≥ 22**
(`xmake f --toolchain=clang`) or **gcc ≥ 16** — older compilers cannot build the module
partitions — plus `glslangValidator` and `libseat`. clang is the toolchain the tree is
currently verified against: the whole suite passes under clang 22.

**Why xmake and not Meson.** Meson's module dependency scanner hardcodes MSVC-shaped `.ifc`
output names (`mesonbuild/scripts/depscan.py`), so ninja dies with "inputs may not also have
inputs" on GCC. xmake scans and orders module units correctly for GCC; that is the whole
reason for the switch.

Generated code (`wayland-scanner` glue, SPIR-V arrays) lands in `build/generated/` at
configure time and is regenerated only when its input is newer.

## Modules

There is no `include/`. Nothing here is a header, so nothing is split
interface-from-implementation; `src/` is grouped by responsibility instead:

```
src/luminaria.cppm       primary interface unit — nothing but `export import`
src/core/                display event_loop expected handle signal
src/util/                box color dmabuf pixel rect_fill region transform
src/backend/             backend output input_event session drm headless libinput wayland
src/render/              vulkan cursor_theme + quad.{vert,frag}
src/scene/               scene output_layout direct_scanout
src/protocol/            the 25 Wayland globals, one file each
src/xwayland/            the X11 bridge
src/detail/wayland_fwd.h the one remaining header
```

- `src/**/<name>.cppm` is one **partition** per concept, named after the *file* and not its
  path: `util/box.cppm` is `luminaria:box`. Directories organise the tree for humans;
  partition names are flat, so moving a file between folders is not an API change.
  Each file holds its interface *and* its implementation — the exported declarations in
  `export namespace luminaria { … }`, then a `// --- implementation` divider, then the pimpl
  `struct X::Impl`, the protocol glue in an anonymous namespace, and the member definitions.
  Consumers never name a partition.
- **Partitions must import what they use, and the import graph must stay acyclic.** This is
  the one thing the old layout hid: implementation units got the whole primary interface for
  free, so nothing ever declared a dependency. Now `seat.cppm` needs an explicit
  `import :compositor;` to see `Surface`. The graph is currently a DAG eight levels deep —
  `box`/`signal`/`expected` at the bottom, `data_control` at the top — and a new edge that
  closes a cycle will not compile. If two partitions genuinely need each other, the shared
  type belongs in a third, lower one.
- Anything a unit `#include`s belongs in its **global module fragment**: `module;` first, then
  the includes, then `export module …`. Declaring a C type after `export module` attaches it
  to module luminaria and makes it a *different type* from the one the C headers declare.
  That is why the opaque forward declarations live in
  `src/detail/wayland_fwd.h` and are `#include`d — declaring them inline in the
  fragment trips `-Wglobal-module`. Note that the GMF now also carries the real
  `<wayland-server-core.h>` etc. that the implementation needs; GMF entities are not exported,
  so `import luminaria;` still pulls in none of them. **Every** `#include` must be up there —
  one below the module declaration is an error under clang
  (`-Winclude-angled-in-module-purview`), which is why `session.cppm` wraps its
  `extern "C" { #include <libseat.h> }` into the fragment.
- **A module-linkage declaration may not name a TU-local type.** The glue in an anonymous
  namespace has internal linkage, but `struct X::Impl` and its members have module linkage, so
  `Impl::light_up(DrmOutput&)` where `DrmOutput` sits in the anonymous namespace is ill-formed
  (`-WTU-local-entity-exposure`). This never came up while the implementations were separate
  `.cpp` files. The fix is to lift just that type to `namespace luminaria` scope — it is still
  unexported, so it stays private to the module — and leave the free functions where they are.
  `drm.cppm`, `workspace.cppm` and `linux_dmabuf.cppm` carry a comment saying so.
- **Namespace-scope names in the implementation half are now shared across the whole module.**
  Two partitions can each have a `static`-ish `manager_bind` because those live in anonymous
  namespaces, but a bare `using Mgr = Foo::Impl;` at namespace scope collides with every other
  partition's `Mgr`. Hence `DcMgr` / `DdMgr` / `FsMgr` / `FtImpl` / `WsBinding` and friends:
  aliases in the implementation half carry a partition prefix.
- Two gcc-16 quirks worth knowing before you hit them (clang 22 accepts both): a defaulted **hidden-friend**
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

Layers, bottom-up (one `src/` folder each, one or more partitions per folder):

- **core** — `Display` (owns `wl_display` + socket + main loop), `EventLoop`/`EventSource`
  (non-owning view + RAII timer/fd sources), `Signal<Event>`, `Result<T>`, `CUnique`.
- **backend** — `Backend` emits `new_output`; `Output` emits `frame` and accepts
  `commit(Color)` / `commit_frame(rgba)`. Implementations: `HeadlessBackend` (software frame
  pump), `WaylandBackend` (nested window in a parent compositor, forwards parent input AND
  its keymap; presents through the parent's `zwp_linux_dmabuf_v1` when it has one —
  `scanout_modifiers` returns what the parent advertised, empty when it advertised nothing,
  which is how a caller knows to fall back to the wl_shm path), `DrmBackend` (KMS atomic modeset, one Output per connected connector kept live
  by a udev hotplug monitor, `import_scanout`/`commit_scanout(id, in_fence)` for GPU dmabuf
  framebuffers, plus an optional hardware cursor plane and `set_mode()` over the
  connector's mode list), `LibinputBackend` (bare-metal input
  signals). Every `Output` carries a `scale` and a `Transform`; `Output::destroy` fires when a
  monitor goes away and `Output::mode_changed` when a `set_mode()` lands — everything
  sized for the old mode (scanout targets, scanout imports, the layout box, the
  `OutputGlobal`) has to be rebuilt there. `Session` (libseat, `session.cppm`) owns the device fds and tells
  both bare-metal backends when the VT is taken away.
- **protocol types** — one partition per Wayland global: `compositor` (`wl_compositor`/`wl_surface`,
  including the subsurface tree), `subcompositor`, `xdg_shell` (toplevels + popups +
  positioners), `seat` (keyboard/pointer/touch/cursor/DnD hooks), `output_global`,
  `linux_dmabuf`, `screencopy` (wlr-screencopy + ext-image-copy-capture, including cursor sessions
  fed by `ScreencopyManager::set_cursor_source()`), `data_device` (clipboard + DnD + primary selection),
  `drm_syncobj` (explicit sync), `single_pixel_buffer`, `presentation_time`,
  `tearing_control`, `cursor_shape`, `workspace` (ext-workspace), `xdg_decoration`,
  `viewporter`, `fractional_scale`, `layer_shell` (wlr-layer-shell, plus the
  `arrange_layer_surface()` anchor/exclusive-zone solver), `foreign_toplevel`
  (wlr-foreign-toplevel-management, mirrors an `XdgShell` on its own via
  `track()`), `xdg_activation`, `relative_pointer`, `pointer_constraints`,
  `text_input` (text-input-v3), `idle_inhibit`, `data_control`
  (wlr-data-control, bridged into `data_device` through `SelectionSource`).
- **`vulkan`** — two paths. GPU: `GpuTexture` (client dmabuf imported with no copy, or
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
  fence clears. Each texture's descriptor set is written once and cached on the
  `GpuTexture` (the view it binds never changes); dead textures' sets go back on a free
  list, but only once every submit that could still be sampling through them has
  retired — `Impl::reap()` gates that on the oldest surviving `in_flight` index.
- **`scene`** — retained tree (Tree/Rect/Surface), positioning, hit-testing, flattening to
  `RectFill`s and `GpuTextureFill`s, and damage (`scene_damage()` →
  `scene_rects(root, damage)` / `scene_textures(root, renderer, damage)` →
  `scene_clear_damage()`); `OutputLayout` (`output_layout.cppm`) is the one place
  that knows where each output sits in the global coordinate space, in *logical* units
  (`transform.cppm` holds the logical↔device mapping and nothing else does).
  `region.cppm` is the disjoint-box set both damage and wl_region are built on.
- **`xwayland`** — spawns Xwayland plus a minimal XWM over xcb.

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
A third bridge skips the renderer entirely: `Surface::current_buffer_dmabuf()` describes a
client buffer well enough to hand to `Output::import_scanout()`, and `DirectScanout`
(`scene/direct_scanout.cppm`) decides when that is allowed — one fullscreen, unrotated,
uncropped surface whose buffer is in a layout the display advertised — caches the imports per
wl_buffer (a client rotates a swapchain; importing per frame would leak framebuffers), and
holds the buffer through `Surface::hold_buffer()` so the client is not told it may redraw
into a frame the display hardware is still scanning out. `luminaria-tty` uses it.

What is still missing on the GPU path: passing fences instead of blocking on them
(`render_to` waits on a fence today).

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
- **The exported interface is C-header-free.** `wl_display`, `wl_resource`, etc. are forward-declared
  only; all libwayland/Vulkan/xcb includes live in the global module fragment. Protocol globals
  use a pimpl (`struct Impl` declared public, defined below the implementation divider) so their address is stable for the
  `wl_global` user-data pointer, while the wrapper stays move-only.
- **Lifetime via RAII, never manual list surgery.** `Signal<E>::Connection` disconnects on
  destruction and survives either destruction order; C handles are wrapped in `CUnique<T, fn>`.
  There should be no `wl_list_remove` calls in compositor code.
- **Null request slots abort libwayland.** Real GTK/Qt clients call requests we haven't
  implemented, and libwayland aborts on a null slot — so unimplemented requests are wired to
  explicit no-op functions (`surface_noop_*` in `compositor.cppm`, `tl_set_parent` /
  `tl_show_window_menu`). When adding a protocol, fill every slot in the interface vtable.
  `-Wno-missing-field-initializers` is set project-wide for exactly this idiom.
- **Raw `Surface*` needs a `Surface::destroy` subscription.** Anything caching a surface
  pointer (seat focus, cursor, drag focus, scene nodes) connects to `Surface::destroy` and
  clears the pointer there; the `Signal::Connection` is RAII so it can't outlive the holder.
  `seat.cppm` is the reference for the pattern.
- **Any retained `wl_resource*` owned by a CLIENT needs a destroy listener.** Buffers are
  the sharp edge: toolkits drop their whole swapchain on resize / hide / re-show, so a
  committed `wl_buffer` dies under you and the next `wl_buffer.release` or readback
  segfaults. `Surface::BufferWatch` (`compositor.cppm`) and `ExtFrame`'s
  `buffer_destroy` (`screencopy.cppm`) show the shape:
  `wl_resource_add_destroy_listener` + null the slot. `tests/test_buffer_destroy.cpp`
  guards it. This is the one place raw libwayland listeners are correct — the C signal is
  on the client's resource, not on one of our `Signal`s.

## Adding a protocol

1. Stable/unstable protocols come from `wayland-protocols` (path resolved via pkgdata);
   ones not shipped there live as XML in `protocol/`.
2. Add a row to the `protocols` (or `local_protocols`) table in `xmake.lua`: name, path, and
   which halves to generate — `s` server header, `c` private code, `l` client header for
   tests that act as clients.
3. Write `src/protocol/<name>.cppm`: the exported interface, the `// --- implementation`
   divider, then the glue — following the pimpl + `Result` pattern above. Add an
   `import :<dep>;` for every partition you use, and keep the graph acyclic. The file is
   picked up by the existing glob; nothing else to register.
4. Add `export import :<name>;` to `src/luminaria.cppm`.
5. Drop a test in `tests/` — `xmake.lua` turns every `tests/test_*.cpp` into its own binary
   automatically. Exit 77 to skip when the machine cannot run it.

An XML argument name becomes a C parameter name verbatim, and everything here that
consumes the generated headers is C++ — so an argument called `namespace`, `class`,
`new`, `template`… simply does not compile. The name never appears on the wire, so the
fix for a protocol whose XML we vendor is to rename the argument in `protocol/` with a
comment saying why: `wlr-layer-shell-unstable-v1.xml` has `namespace` renamed to `scope`.
