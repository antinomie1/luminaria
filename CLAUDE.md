# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Luminaria: a minimal Wayland compositor **library** in modern C++23, built on top of
`libwayland-server` (the wire protocol is never reimplemented), rendering with Vulkan-Hpp,
built with xmake. Roughly wlroots-shaped but far smaller (30 protocol types vs wlroots' 73).

It ships as four **C++23 named modules** with no public headers: `luminaria` is the
dependency-light protocol/core interface, with opt-in `luminaria.gpu`, `luminaria.desktop`
and `luminaria.xwayland` extensions.
All prose docs are in Chinese. `README.md` is the introduction only — no technical detail.
`CONTEXT.md` is the glossary (what "混成器"/"合成"/"布局" mean here, and what they don't);
`docs/architecture.md` is the layering, module structure and the rules that bite;
`docs/features.md` is the feature matrix; `docs/adr/` holds the irreversible design
decisions and why; `TODO.md` is the open work in execution order.

**Four ADRs set the current direction:** no retained scene graph, generational identities for
retained surfaces, a four-way module split with an admission rule for the core module, and no
API stability before 1.0. The first three are implemented. Read `docs/adr/` before designing
anything structural.

## Build / test / run

```sh
xmake f -y                # configure (also runs wayland-scanner + glslangValidator)
xmake                     # build the library and the examples
xmake build -a            # ...and every test binary
xmake test                # run all tests
xmake test test_dmabuf/*  # run one
xmake build tinyluminaria # one target
xmake f -m release        # optimised build; debug is the default

# under a sanitizer — see the comment in xmake.lua for what each does NOT catch
xmake f --sanitize=address --toolchain=clang && xmake build -a
LSAN_OPTIONS=suppressions=tests/lsan.supp xmake test
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
src/luminaria.cppm       core primary interface — protocol + nested/headless
src/luminaria.gpu.cppm   GPU primary interface — Vulkan/DRM/input/session
src/luminaria.desktop.cppm desktop primary interface — privileged shell protocols
src/core/                display event_loop expected handle signal
src/util/                box color dmabuf pixel pixel_layout rect_fill region transform
src/backend/             backend output input_event session drm headless libinput wayland
src/render/              vulkan cursor_theme + quad.{vert,frag}
src/shell/               frame output_layout direct_scanout
src/protocol/            the 25 Wayland globals, one file each
src/xwayland/            `luminaria.xwayland`, the X11 bridge
src/detail/wayland_fwd.h the one remaining header
```

- Most `src/**/<name>.cppm` files are one **partition** per concept, named after the *file*
  and not its path: `util/box.cppm` is `luminaria:box`, while GPU and desktop concepts use
  `luminaria.gpu:<name>` and `luminaria.desktop:<name>`. Directories organise the tree for humans;
  partition names are flat, so moving a file between folders is not an API change.
  Each file holds its interface *and* its implementation — the exported declarations in
  `export namespace luminaria { … }`, then a `// --- implementation` divider, then the pimpl
  `struct X::Impl`, the protocol glue in an anonymous namespace, and the member definitions.
  Consumers never name a partition.
- **Partitions must import what they use, and the import graph must stay acyclic.** This is
  the one thing the old layout hid: implementation units got the whole primary interface for
  free, so nothing ever declared a dependency. Now `seat.cppm` needs an explicit
  `import :compositor;` to see `Surface`. Each module's partition graph must remain a DAG;
  extension partitions import `luminaria` for core types. A new edge that closes a cycle will
  not compile. If two partitions genuinely need each other, the shared type belongs in a
  third, lower one or in the core module when it crosses an extension boundary.
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
  **`frame` is asked for, not free-running.** One `Output::schedule_frame()` buys one `frame`
  event; committing does NOT implicitly buy the next one. An idle screen therefore subscribes
  to no vblank and wakes nobody — DRM's frames come from page-flip completions (plus a 1ms
  timer to break out of idle), the nested backend asks its parent for a frame callback with an
  empty commit, and headless's timer only paces requests instead of manufacturing them.
  `Frame` does the asking for compositors built on it; anything else must call it by hand.
- **protocol types** — one partition per Wayland global: `compositor` (`wl_compositor`/`wl_surface`,
  including the subsurface tree), `subcompositor`, `xdg_shell` (toplevels + popups +
  positioners), `seat` (keyboard/pointer/touch/cursor/DnD hooks), `output_global`,
  `linux_dmabuf`, `screencopy` (wlr-screencopy + ext-image-copy-capture, including cursor sessions
  fed by `ScreencopyManager::set_cursor_source()`), `data_device` (clipboard + DnD + primary selection),
  `drm_syncobj` (explicit sync), `single_pixel_buffer`, `presentation_time`,
  `tearing_control`, `fifo` + `commit_timing` (the two pacing protocols, which park a
  commit on `Surface`'s commit gate until the previous frame was presented or a stamp
  arrives), `content_type`, `cursor_shape`, `workspace` (ext-workspace), `xdg_decoration`,
  `viewporter`, `fractional_scale`, `layer_shell` (wlr-layer-shell, plus the
  `arrange_layer_surface()` anchor/exclusive-zone solver), `foreign_toplevel`
  (wlr-foreign-toplevel-management, mirrors an `XdgShell` on its own via
  `track()`), `xdg_activation`, `relative_pointer`, `pointer_constraints`,
  `text_input` (text-input-v3), `idle_inhibit`, `data_control`
  (wlr-data-control, bridged into `data_device` through `SelectionSource`),
  `session_lock` (ext-session-lock, which fails CLOSED: a lock client that dies leaves
  the session locked), `input_method` (input-method-v2, bridged to `text_input`).
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
  **A steady-state `render_to` allocates nothing** (`tests/test_render_alloc.cpp` counts it,
  the same way `test_frame` does for the shell): the regions are `Impl` scratch, the
  framebuffers are built once per render pass on the `ScanoutTarget`, and command buffers,
  fences and the semaphore list come off free lists that `reap()` refills when a submit's
  fence clears. `Region::subtract`/`intersect` ping-pong two buffers for the same reason —
  they used to move-assign a fresh vector, which is an allocation per call per surface.
- **shell** — the opinionated half, and deliberately immediate-mode: there is no retained
  tree (ADR 0001). `Frame` (`shell/frame.cppm`) is the per-output ledger a compositor
  refills every frame — `begin(view)` then `place(surface, x, y)` per window, giving a
  z-ordered `Placement` list that is BOTH drawn and hit-tested (`surface_at()`), so a click
  cannot disagree with the pixels. `submit(background)` then does everything that is the
  same in every compositor: try direct scanout, build the `GpuTextureFill`s, repaint this
  frame's client damage plus the damage the buffer being drawn into still owes (buffer age),
  thread the display's out-fence and the clients' acquire fences through the GPU, flip, and
  clear the surfaces' damage. It answers `Presented::{composited,scanout,unchanged,fallback}`;
  `unchanged` is what step 3 needs to stop flipping over a still screen. **Nothing about a
  window survives a frame** — what survives is the memory (every vector is cleared, never
  freed; `Placement::opaque` is an index range into the frame's arena, never a `Region`
  copy) and the damage debt. `tests/test_frame.cpp` asserts zero heap allocations across a
  steady-state rebuild by replacing global `operator new`.
  `OutputLayout` (`output_layout.cppm`) is the one place that knows where each output sits
  in the global coordinate space, in *logical* units (`transform.cppm` holds the
  logical↔device mapping and nothing else does). `region.cppm` is the disjoint-box set both
  damage and wl_region are built on.
- **`xwayland`** — spawns Xwayland plus a minimal XWM over xcb.

Per-frame flow in a compositor built on this (see `examples/tinyluminaria.cpp`,
`examples/tty_compositor.cpp`): `Output::frame` fires → `Frame::begin(view)` → one
`Frame::place(surface, x, y)` per mapped toplevel (which expands its subsurface tree),
then popups anchored to their parents, then the cursor when there is no cursor plane →
`Frame::submit(background)`. Inside that: Frame's GPU bridge imports or uploads each client
buffer, `VulkanRenderer::render_to(ScanoutTarget)` composites, and
`Output::commit_scanout()` flips — or the whole composite is skipped and one client's own
buffer goes to the CRTC. Outputs without dmabuf scanout (headless, nested) still composite
on the GPU and only the finished frame crosses to the CPU: `read_scanout()` →
`commit_frame()`. Input goes the other way: backend input signal → `Frame::surface_at()`
→ `Seat` focus + event routing.

Two bridges expose client buffers: `Surface::current_buffer_dmabuf()` lets Frame import dmabuf
straight into `VulkanRenderer` with no readback; `Surface::current_buffer_rgba()` handles shm and
single-pixel buffers for upload, screencopy and non-dmabuf backends. The dmabuf description also
skips the renderer entirely when handed to `Output::import_scanout()`, and `DirectScanout`
(`shell/direct_scanout.cppm`) decides when that is allowed — one fullscreen, unrotated,
uncropped surface whose buffer is in a layout the display advertised — caches the imports per
wl_buffer (a client rotates a swapchain; importing per frame would leak framebuffers), and
holds the buffer through `Surface::hold_buffer()` so the client is not told it may redraw
into a frame the display hardware is still scanning out. `luminaria-tty` uses it.

**Surface coordinates are not buffer pixels.** A client can hand over a denser buffer
(`set_buffer_scale`), a rotated one (`set_buffer_transform`), or crop and stretch it
(`wp_viewporter`). Layout, hit-testing and subsurface offsets all use
`Surface::surface_width()/surface_height()`; only code that touches pixels wants
`buffer_width()`. Hit-testing goes through `Surface::accepts_input()` so the client's input
region decides, not the buffer rectangle.

**The renderer must outlive the Display.** Each `Frame` caches `GpuTexture`s owned by the
`VulkanRenderer`, so the renderer has to be declared *before* the `Display`
(locals are destroyed in reverse order). Both examples do this with a comment; getting it
wrong aborts inside `vk::raii` at shutdown.

**Frame callbacks are the compositor's job.** `wl_surface.frame` is NOT answered on commit —
answering there makes clients render frames the display never shows. They queue until the
compositor calls `Surface::send_frame_done(time_ms)`, which belongs in the `Output::present`
handler (alongside `Presentation::notify_presented`). Forget it and every client freezes after
one frame. Damage works the same way: `Surface::damage()` accumulates until whoever rendered it
calls `clear_damage()`. The one exception is `Presented::unchanged`: nothing was committed, so
no `present` follows, and the same `send_frame_done()` has to happen in the `frame` handler
instead — a client that commits without damaging anything is otherwise frozen for good. Both
example compositors show the shape.

## Conventions that matter here

- **No exceptions across the C boundary.** Fallible operations return `Result<T>` /
  `Status` (`fail("msg")`, `ok()`). Vulkan-Hpp throws internally; every `VulkanRenderer`
  method catches at its boundary and converts to `Result`.
- **The exported interface is C-header-free.** `wl_display`, `wl_resource`, etc. are forward-declared
  only; all libwayland/Vulkan/xcb includes live in the global module fragment. Protocol globals
  use a pimpl (`struct Impl` declared public, defined below the implementation divider) so their address is stable for the
  `wl_global` user-data pointer, while the wrapper stays move-only.
- **Lifetime via RAII, never manual list surgery.** `Signal<E>::Connection` disconnects on
  destruction and survives either destruction order; C handles are wrapped in `CUnique<T, fn>`
  and owned fds in `UniqueFd` (both in `core/handle.cppm`).
  There should be no `wl_list_remove` calls in compositor code.
- **An owned fd is a `UniqueFd`; a borrowed one is an `int`.** This is the rule that stops
  the leaks fence plumbing used to grow: `DrmOutput::atomic()` had four separate `close()`
  calls for one in-fence, one per early return, and the next early return would have got
  none. A member that owns an fd (`Surface::acquire_fence_`, `ScanoutTarget::Impl::acquire_fence`,
  `DrmOutput::present_fence`) holds a `UniqueFd`; a function that takes ownership takes one
  by value. The exported signatures stay `int` — `set_acquire_fence(int)`,
  `commit_scanout(id, int)`, `take_present_fence()` — because they are the C boundary, and
  the implementation wraps or `release()`s on the first line.
- **Never index client memory on a client's word.** A client declares width,
  height, stride and offset as four independent integers and nothing upstream cross-checks
  them in the units that matter: libwayland validates `wl_shm_pool.create_buffer` as
  `stride >= width` — *bytes against pixels* — and `zwp_linux_buffer_params_v1.add` validates
  the stride not at all. Every CPU pixel loop here indexes `row[x * 4 + 3]`, so a stride a
  quarter of the real row length walks off the mapping: reading leaks adjacent process memory
  back through screencopy, and the capture protocols *write*, which is corruption in another
  client's address space. Both were reachable by an unprivileged client.
  So: run the layout through `layout_fits()` / `layout_length()` (`util/pixel_layout.cppm`)
  before touching the pixels — at the protocol request, where a real error can be posted, AND
  again at the loop, so a later caller cannot reintroduce the hole by taking a shortcut.
  `tests/test_buffer_bounds.cpp` and `tests/test_screencopy_bounds.cpp` guard this; the second
  works by canary rather than by fault, because ASan does not shadow mmap'd regions and so
  catches none of this class.
- **Null request slots abort libwayland.** Real GTK/Qt clients call requests we haven't
  implemented, and libwayland aborts on a null slot — so unimplemented requests are wired to
  explicit no-op functions (`surface_noop_*` in `compositor.cppm`, `tl_set_parent` /
  `tl_show_window_menu`). When adding a protocol, fill every slot in the interface vtable.
  `-Wno-missing-field-initializers` is set project-wide for exactly this idiom.
- **Retain `SurfaceId`, never `Surface*`, across dispatch.** Resolve with `surface_from_id()`
  immediately before use; destruction clears the slot and increments its generation, so an old
  id cannot resolve to a later surface that reused the slot. `Surface::destroy` remains useful
  for semantic teardown tied to that surface, but it is not the memory-safety boundary. Seat,
  data-device, Frame and DirectScanout are the reference implementations. The deterministic
  malformed-stream client in `tests/test_protocol_fuzz.cpp` runs in every ordinary suite.
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
   divider, then the glue — following the pimpl + `Result` pattern above. Apply ADR 0003's
   admission rule first: core protocols use `luminaria:<name>` and partition imports;
   GPU/desktop extensions use their module name, `import luminaria;`, and sibling partition
   imports as needed. Keep the graph acyclic.
4. Add the file to the corresponding xmake target and `export import :<name>;` to that
   target's primary interface (`src/luminaria*.cppm`). Xwayland is a standalone primary unit.
5. Drop a test in `tests/` — `xmake.lua` turns every `tests/test_*.cpp` into its own binary
   automatically. Exit 77 to skip when the machine cannot run it.

An XML argument name becomes a C parameter name verbatim, and everything here that
consumes the generated headers is C++ — so an argument called `namespace`, `class`,
`new`, `template`… simply does not compile. The name never appears on the wire, so the
fix for a protocol whose XML we vendor is to rename the argument in `protocol/` with a
comment saying why: `wlr-layer-shell-unstable-v1.xml` has `namespace` renamed to `scope`.
