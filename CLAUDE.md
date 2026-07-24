# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Luminaria: a minimal Wayland compositor **library** in modern C++23, built on top of
`libwayland-server` (the wire protocol is never reimplemented), rendering with Vulkan-Hpp,
built with Meson. Roughly wlroots-shaped but far smaller (13 protocol types vs wlroots' 73).
`README.md` (Chinese) holds the current feature matrix and the roadmap of missing protocols;
`TODO-CORNERS-CUT.md` (Chinese) is an audit of every TODO and every deliberate no-op stub.

## Build / test / run

```sh
meson setup build
ninja -C build
meson test -C build                       # all tests
meson test -C build dmabuf                # one test by name (names in tests/meson.build)
meson test -C build --verbose vulkan      # show stdout
ninja -C build examples/tinyluminaria     # single target
```

`werror=true`, `warning_level=3`, `cpp_std=c++23` — warnings break the build. `<expected>` is a
hard requirement (gcc ≥ 14 / clang ≥ 18); Meson errors out at configure time without it.

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
  pump), `WaylandBackend` (nested window in a parent compositor, also forwards parent input),
  `DrmBackend` (KMS dumb buffers, legacy pageflip), `LibinputBackend` (bare-metal input signals).
- **types/** — one file per Wayland global: `compositor` (`wl_compositor`/`wl_surface`,
  including the subsurface tree), `subcompositor`, `xdg_shell` (toplevels + popups +
  positioners), `seat` (keyboard/pointer/touch/cursor/DnD hooks), `output_global`,
  `linux_dmabuf`, `screencopy`, `data_device` (clipboard + DnD + primary selection),
  `drm_syncobj` (explicit sync).
- **render/vulkan** — offscreen composite to an RGBA `std::vector<Pixel>`, plus dmabuf
  import/export via `VK_EXT_external_memory_dma_buf` + `VK_EXT_image_drm_format_modifier`.
- **scene** — retained tree (Tree/Rect/Surface), positioning, hit-testing, flattening to
  `RectFill`s for the renderer.
- **xwayland** — spawns Xwayland plus a minimal XWM over xcb.

Per-frame flow in a compositor built on this (see `examples/tinyluminaria.cpp`,
`examples/tty_compositor.cpp`): `Output::frame` fires → walk mapped toplevels, each expanded
via `Surface::surface_tree()` (the surface plus its subsurfaces, back-to-front), then popups
anchored to their parents, then the cursor → `Surface::current_buffer_rgba()` converts each
client buffer to RGBA → `VulkanRenderer::composite()` → `Output::commit_frame()`. Input goes
the other way: backend input signal → scene/manual hit test → `Seat` focus + event routing.
`tinyluminaria` builds one z-ordered layer list per frame and uses it for BOTH rendering and
hit-testing, so clicks can't disagree with pixels.

`Surface::current_buffer_rgba` is the single bridge from client buffers to pixels: shm first,
then `dmabuf_buffer_to_rgba()` (LINEAR via mmap, other modifiers via `VulkanRenderer::import_dmabuf`).
Everything is read back to CPU RGBA today — there is no zero-copy scanout path.

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
2. In the root `meson.build`, add `custom_target`s for the server header + private code
   (and a client header if a test acts as a client).
3. Add the new `src/types/<name>.cpp` and the generated targets to `src/meson.build`.
4. Public header in `include/luminaria/<name>.hpp` following the pimpl + `Result` pattern above.
5. Add a test in `tests/` and register it in `tests/meson.build`.
