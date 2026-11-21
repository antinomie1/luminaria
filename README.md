# luminaria

[中文](README.zh-CN.md)

A library for writing Wayland compositors in
[Mica](https://github.com/antinomie1/mica), built with
[sheaf](https://github.com/antinomie1/sheaf). It provides the mechanism:
protocol state, composition and devices. The compositor keeps the policy
and its own loop. The design is in [docs/](docs/README.md) (Chinese).

| Package | Role |
|---------|------|
| `luminaria` | `Display`: one event loop over everything below, layouts, input routing, manipulations |
| `luminaria_server` | Protocol state: surfaces, transactions, xdg-shell, layer shell, session lock, seats and input methods, selections, outputs, buffers, timing; no GPU |
| `luminaria_compose` | Composition on `vk_compose`: placement diffs, client buffers as textures, fences, clock |
| `luminaria_backend` | DRM/KMS, nested and headless outputs; libseat, libinput, xkb |
| `luminaria_proto` | Interfaces generated from `protocols/*.xml` |
| `luminaria_wire` | Wire protocol: sockets, fd passing, encoding, object maps |
| `luminaria_scanner` | Writes `luminaria_proto` (dev tool) |

```
luminaria ─┬─ luminaria_server ─── luminaria_proto ── luminaria_wire
           ├─ luminaria_compose ── vk_compose, vk_draw2d, vk_gpu (github.com/antinomie1/vk)
           └─ luminaria_backend ── luminaria_proto, vk_gpu, sys/*
```

`protocols/` holds the XML from wayland 1.26, wayland-protocols 1.50 and
wlr-protocols. The server speaks:

| Area | Protocols |
|------|-----------|
| Core and shell | wl_compositor 6, subcompositor, shm, wl_seat 9, wl_output 4, xdg-shell 6, wl_fixes |
| Buffers and timing | linux-dmabuf 4, linux-drm-syncobj, single-pixel-buffer, viewporter, fractional-scale, presentation-time, fifo, commit-timing, tearing-control, alpha-modifier, content-type |
| Windows | xdg-decoration, xdg-activation, xdg-toplevel-icon, xdg-toplevel-tag, xdg-dialog, xdg-foreign v2, xdg-system-bell, ext-foreign-toplevel-list |
| Desktop | wlr-layer-shell 5, ext-session-lock, xdg-output 3, ext-idle-notify 2, idle-inhibit |
| Input | relative-pointer, pointer-constraints, pointer-gestures 3, cursor-shape 2, keyboard-shortcuts-inhibit, text-input-v3, input-method-v2, virtual-keyboard |
| Selections | clipboard, primary selection, ext-data-control |

Not yet: drag and drop, screen capture, tablets, security-context, output
management.

## Writing a compositor

```mica
import luminaria as lum

var d = try lum.Display.open(lum.Options {})   // DRM, or nested under WAYLAND_DISPLAY
var wins: List[lum.Window] = []
while true {
    guard let Some(e) = try d.next() else {
        break
    }
    match e {
        WindowMapped(w) => {
            wins.push(w)
            d.focus(Some(w))
            d.layout(fn(mut l) {
                for i, x in wins {
                    l.window(x, f64(40 * i), f64(40 * i), lum.Style { radius: 8.0, shadow: 16.0, transition_ms: 150 })
                }
            })
        }
        Key(k) => {
            if k.pressed and k.mods.logo and k.sym == lum.keysym.key_q {
                break
            }
            d.accept(e)
        }
        _ => d.accept(e)
    }
}
```

- **One loop, no callbacks.** `next` returns events as values. When none
  is queued it finishes the frames that are due, flushes every client
  (one `sendmsg` each) and sleeps in epoll; an idle desktop has no timers
  armed. The compositor's own descriptors and timers join the loop
  (`watch`, `timer`); `fd` and `poll` embed it in another loop.
- **`accept` is the default.** Input goes to the surface under the pointer
  (hit test with implicit grabs) or with keyboard focus, new windows get a
  configure, maximize and fullscreen requests are granted on the window's
  output, moves and resizes run inside the library, popups are placed by
  their positioners and dismissed by clicks outside, hidden windows are
  told they are suspended, Ctrl+Alt+Fn switches VTs. Handle what you want
  differently and pass the rest on.
- **Layouts, not scene trees.** `layout` takes the whole placement list
  (windows with their subsurfaces and popups, the compositor's images);
  each output gets the part it shows and only what changed is redrawn.
  `transition_ms` animates a placement to its new position in the library.
- **Manipulations run at input rate.** `manipulate_move`,
  `manipulate_resize` and `manipulate_pan` (a swipe, with inertia) move
  windows on each pointer or gesture event without a round trip through
  the compositor; the cursor plane moves at once. `Moved`, `Resized` and
  `Panned` report the result.
- **Transactions.** Configures between `begin` and `end` show together
  with the layout given meanwhile, once every window drew its new size,
  or at the timeout with what arrived.
- **The shell runs itself.** Panels, wallpapers and launchers (layer
  shell) are arranged around the layout; `usable` is what they leave free.
  A lock screen hides everything at once and the locker hears `locked`
  only after every output showed a frame without the session. Input
  method popups follow the text cursor; pointer locks, confinement,
  relative motion, gestures, idle notification and clipboard managers
  need no code in the compositor.

[`examples/tiny`](examples/tiny/src/main.mica) is a complete stacking
compositor in about 250 lines: windows open centred, click to focus and
raise, Alt+drag moves (left) or resizes (right), Alt+F1 cycles, Alt+Q
closes, Alt+Escape quits; `-s` runs a command once it is up.

```sh
sheaf run -p tiny -- -s foot    # on a VT, or as a window in your session
```

### Frames

- Each output keeps a compositor clock: a frame starts as late as the
  next vblank allows (the margin grows after a late frame and shrinks
  back), or as soon as there is something to show with variable refresh.
- Commits wait for their buffers in the event loop, never in the render
  path: a dmabuf's implicit fence is exported as a sync_file, a syncobj
  point becomes an eventfd, fifo barriers and commit-timing targets wait
  for presentation. A client's commits apply in order; a slow client
  holds back only itself.
- shm commits are coalesced: the buffer is uploaded once per frame, only
  the damaged rectangles, then released; a buffer replaced before the
  frame is released without a copy.
- A client buffer that fills an output unscaled and opaque is shown
  directly: a KMS framebuffer after an atomic test (zero copy, its damage
  as `FB_DAMAGE_CLIPS`), or handed to the parent compositor when nested.
- Composed frames redraw only the damage, skip what opaque windows hide,
  pass the GPU's sync_file as `IN_FENCE_FD` (the CPU never waits) and
  send the changed rectangles as `FB_DAMAGE_CLIPS` or parent damage.
- Frame callbacks and presentation feedback go only to surfaces the frame
  showed; hidden clients stop drawing.

### Backends

| Backend | Output | Input |
|---------|--------|-------|
| DRM | atomic KMS: primary and cursor planes, modifiers from `IN_FORMATS`, page-flip timestamps, async flips for tearing clients, VRR, DPMS, hotplug | libinput through libseat, xkb |
| Nested | a window of the parent compositor (its title bar and borders through xdg-decoration), dmabufs over linux-dmabuf with implicit fences | the parent's seat and keymap |
| Headless | textures paced by a timer at the mode's refresh | `inject` |

Release build, measured on this machine (`sheaf test --release`):

| What | Time |
|------|------|
| A 1920x1080 frame, 50 rounded, shadowed windows, one moving: CPU to record and submit | 8 us |
| The same, until the GPU is done | 46 us |
| Hit test over 512 surfaces, worst case (integer SIMD, 8 per step) | 251 ns |

## Wire and protocol

`luminaria_wire` speaks the Wayland wire protocol in Mica, without
libwayland, and interoperates with libwayland clients.

- **Receiving.** One `recvmsg` fills a fixed 8 KiB buffer per connection.
  Messages are decoded in place: numbers are read straight from the
  buffer, and only strings and arrays are copied.
- **Sending.** Events are encoded into one send buffer. `flush` hands it
  to the kernel once per dispatch, in a single `sendmsg` unless more than
  28 fds are queued; each fd arrives no later than its message.
- **No allocation in steady state.** The buffers and the id map are
  reused.
- **Bounded memory.** A peer that stops reading is dropped after 1 MiB of
  queued data.
- **Checks every compositor needs:**
  - Object ids are a dense map, 8 bytes each. A client's new id must be
    its next or a freed one, so it cannot make the map grow without
    bound.
  - Arguments are checked for interface, null, length and `since`
    version. A failure sends `wl_display.error` and closes the client.
  - Destructors free the id and send `delete_id` automatically.
  - An event newer than the object's version is skipped.

`luminaria_proto` is generated by `luminaria_scanner`. Each protocol file
is a module (`wayland`, `xdg_shell`, ...) with:

- typed ids (`wl.Surface`)
- request enums, read with `proto.next`
- event senders, as methods on the typed ids
- protocol enums, `Flags` for bitfields

The client side (`proto.next_event`, request methods) is generated too,
for the nested backend and tests. A compositor that never calls it
carries no extra code: the linker drops it.

```mica
while true {
    guard let Some(r) = try proto.next(mut conn) else {
        break
    }
    match r {
        WlDisplay(_, Sync(cb)) => cb.done(mut conn, serial)
        WlSurface(s, Attach(buffer, x, y)) => ...
        _ => {}
    }
}
discard try conn.flush()
```

Release build, through a Unix socket, 128 messages per flush:

| Direction | Per message |
|-----------|-------------|
| Request: encode, send, receive, decode | ~60 ns |
| Event (`wl_pointer.motion`) | ~48 ns |

```sh
sheaf run -p luminaria_scanner -- protocols luminaria_proto/src   # after editing protocols/
```

## Bindings

`sys/` holds bindings for the Linux libraries a compositor stands on.
They are general-purpose: any Mica program can depend on them.

| Package | Library | Contents |
|---------|---------|----------|
| `xkbcommon_sys` | libxkbcommon 1.x | keymaps, state, compose, all 2638 keysyms |
| `libinput_sys` | libinput 1.x | path and udev contexts, events, device config |
| `libseat_sys` | libseat | seat and device access (seatd, logind, noop) |
| `libudev_sys` | libudev | enumerate, devices, netlink monitor, hwdb |
| `libdrm_sys` | libdrm | xf86drm, KMS (`xf86drmMode`), kernel uapi in `libdrm_sys.uapi` |

Generated by `mica bind` (`sys/gen.sh`), with these fixes on top:

- Names drop the library prefix: `libinput_get_event` is `get_event`,
  `struct xkb_state` is `State`. Record fields are `pub`, so other bundles
  can build and read them.
- Keysyms keep their case, because `bind` would fold `XKB_KEY_a` and
  `XKB_KEY_A` into one name: `xkb.key_a`, `xkb.key_A`, `xkb.key_Escape`.
- Macros built from function-like macros are evaluated by `cc`. This
  covers every `DRM_FORMAT_*` and `*_FORMAT_MOD_*` constant, and the
  `DRM_IOCTL_*` numbers for raw `drm.ioctl` calls with the `uapi` structs.
- libdrm's own types (`drmVersion`, `drmModeCrtc`) and the kernel's
  (`drm_version`, `drm_mode_crtc`) would map to the same names, so the
  uapi headers live in their own module:
  `import libdrm_sys.uapi`.
- `libinput_sys` uses `libudev_sys`'s handles, so a `udev.Udev` goes
  straight into `udev_create_context` and `device_get_udev_device` returns
  a `udev.Device`.
- Log handlers take a `va_list`, which Mica cannot. A small C file formats
  each message and passes it on without its trailing newline:
  `log_set_handler` (libinput), `set_log_handler` (libseat) and
  `context_set_log_fn` (xkbcommon). There is one handler per process.

Callbacks are `extern fn` with a body (`extern unsafe fn` when they take a
`Ptr`):

```mica
import libseat_sys as seat

extern unsafe fn on_enable(s: seat.Libseat, data: Ptr[u8]) {
}

let l = seat.SeatListener { enable_seat: Some(on_enable) }
```

Limits: an optional `const char *` parameter is `str` and cannot be NULL,
and the string macros (`XKB_MOD_NAME_SHIFT`) are not emitted.

## Regenerate

```sh
MICA=/path/to/mica sys/gen.sh    # INC=/usr/include by default
```

## Test

```sh
sheaf test    # luminaria: a libwayland xdg window (built with cc) and foot on a
              # headless output: configured, composited, read back, focused,
              # clicked; server: commits, transactions, toplevels,
              # subsurfaces, popups, focus, dmabuf waits; compose: diffs,
              # uploads, clock; backend: xkb, headless frames, the DRM card's
              # displays, a nested window; protocols: layer surfaces, the
              # session lock, selections, input methods, decorations and
              # activation, the toplevel list, pointer locks, idle, single
              # pixels; the shell on a headless output: panel zones, lock
              # screen; wire, proto, sys as before
sheaf test --release -p luminaria_compose   # frame cost
sheaf test --release -p luminaria           # hit testing
sheaf test --release -p luminaria_proto     # wire throughput
```

## License

Apache-2.0 ([LICENSE-APACHE](LICENSE-APACHE)) or BSD-2-Clause
([LICENSE-BSD](LICENSE-BSD)), at your option.
