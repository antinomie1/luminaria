# Luminaria — TODO & Progress

A from-scratch minimal Wayland compositor **library** in modern C++ (C++23),
built on `libwayland-server` with a Vulkan renderer and a Meson build. Think
"tiny wlroots": the library provides the pieces, `tinyluminaria` wires them into
a working compositor.

**Status:** core path works end-to-end. A real client (`weston-terminal`)
connects, maps a window, is GPU-composited, and is visible + interactive
(pointer, keyboard, modifiers) when run nested inside a parent compositor.

Tests: **17 pass, 1 skips** (DRM — needs a bare VT / real GPU KMS).

---

## Progress (what works)

### Core substrate
- [x] RAII display / event loop wrappers (`core/display`, `core/event_loop`)
- [x] `Signal<Event>` with defined re-entrancy (connect/disconnect during emit is safe)
- [x] `Result<T>` / `Status` error type (`core/expected`)
- [x] `Handle`, `Box`, `Color`, `Pixel`, `Rect` utilities

### Protocol objects (server side)
- [x] `wl_compositor` + `wl_surface` — attach / damage / commit / frame wired
- [x] `wl_shm` buffers → RGBA readback (`current_buffer_rgba`, ARGB8888 + XRGB8888)
- [x] `xdg_wm_base` / `xdg_surface` / `xdg_toplevel`
- [x] `wl_seat` v5 — keyboard + pointer, xkb keymap, focus enter/leave
- [x] `wl_output` global — geometry/mode/scale/done (real clients need this to map)

### Rendering
- [x] Vulkan-Hpp compositor: solid background, rect fills, client textures
- [x] Texture upload via staging buffers + GPU read-back to CPU pixels
- [x] **Texture clipping** — partially off-screen surfaces render their visible part
      (previously dropped whole surfaces crossing the output edge)

### Backends
- [x] Headless (smoke-test wiring, no GPU/display)
- [x] Nested Wayland (window inside a parent compositor; wl_shm CPU present)
  - [x] Presents composited frames to the parent
  - [x] **Forwards parent input**: pointer enter/leave/motion/button, keyboard
        key + **modifiers** (Shift/Ctrl work), routed via a hit-test into the seat
- [x] DRM/KMS (code present; untested on real hardware — test skips)
- [x] libinput (bare-metal input; emits KeyEvent / PointerMotion / PointerButton)

### Scene / Xwayland / examples
- [x] Scene graph (`scene/scene`)
- [x] Minimal Xwayland + XWM (`xwayland/xwayland`)
- [x] `tinyluminaria` — reference compositor (headless or nested)
- [x] `luminaria-tty` — DRM/TTY example
- [x] Window lifecycle: closed windows reaped on the next frame (no stale entries)

---

## TODO (by priority)

### P0 — make the compositor genuinely usable
- [ ] **Wire libinput into `tinyluminaria`** — real keyboard/mouse only works on
      the nested backend today; DRM/TTY path emits input events but nothing routes
      them into the seat. Mirror the nested wiring (hit-test → seat).
- [ ] **Cursor rendering** — no visible cursor sprite; honor `wl_pointer.set_cursor`
      (currently a no-op) and composite the cursor surface.
- [ ] **Seat `ptr_focus` / `kb_focus` surface-destroy safety** — the seat stores raw
      `Surface*` focus pointers with no destroy listener. Current call ordering
      avoids the dangling deref, but a `wl_surface` destroyed while focused should
      clear focus explicitly.

### P1 — correctness / presentation
- [ ] **Frame timing** — `frame` events fire on commit, not on actual presentation
      (`compositor.cpp` TODO). Pace to the output's real vblank / frame callback.
- [ ] **Damage tracking** — every frame re-composites the whole output. Track
      surface + output damage and only redraw dirty regions.
- [ ] **Multi-output** — one `wl_output` today; support >1 output, hotplug,
      per-output scale/transform.
- [ ] **Keymap consistency** — nested backend forwards the parent's xkb modifier
      masks against our own keymap; fine for standard layouts, but adopt the
      parent keymap (or reconcile) for non-US layouts / groups.

### P2 — performance / capability
- [ ] **Zero-copy buffers** — replace wl_shm CPU upload with `linux-dmabuf`
      import (nested + DRM), avoid the per-frame CPU pack/readback.
- [ ] **libseat** — DRM/libinput rely on logind session ACLs; use libseat for
      proper session/VT management and device handover.
- [ ] **Subsurfaces** (`wl_subcompositor`), `wl_data_device` (copy/paste, DnD).
- [ ] **xdg-shell completeness** — popups, positioners, configure bounds,
      window states (maximize/fullscreen/resize).

### P3 — polish
- [ ] Xwayland: fuller XWM (window stacking, focus, `_NET_*` hints, clipboard).
- [ ] Output layout / positioning API for the compositor.
- [ ] Screen capture protocol (`wlr-screencopy` or ext-image-copy) — note: KDE
      Plasma does not expose wlr-screencopy, so `grim` cannot shoot Luminaria's
      own output; would need this to self-screenshot.

---

## Known limitations / notes
- **No Shift/Ctrl-independent modifier state per client** beyond what the parent
  reports (nested) — fine in practice.
- **wl_seat bound at v1 on the nested backend** deliberately: higher versions
  send `wl_pointer.frame`/axis events; libwayland aborts on a null listener slot,
  so we only ask for the v1 events we handle.
- **DRM path is unverified on hardware** in this environment (no bare VT).
- Build needs a C++23 stdlib with `<expected>` (gcc ≥ 14 / clang ≥ 18), Vulkan,
  xkbcommon, libdrm, libinput, libudev, wayland-protocols.

---

## Build & run

```sh
meson setup build
ninja -C build
meson test -C build            # 17 pass, 1 skip (DRM)

# nested inside a running compositor (window appears in your session):
WAYLAND_DISPLAY=wayland-0 ./build/examples/tinyluminaria &
WAYLAND_DISPLAY=<printed-socket> weston-terminal
```
