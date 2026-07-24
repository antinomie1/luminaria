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

## 完整 Compositor 路线图（缺失协议 / 部分）

达到"能日常用的完整 Wayland compositor"还需实现以下内容。按**客户端是否可用**分层。
✓=已有，✗=缺，△=部分。参照：wlroots 有 73 个 type 实现，luminaria 现有 5 个。

### P0 — 不做真实程序直接跑不起来
- [ ] **`linux-dmabuf-v1` + GBM 分配器** — 所有 GPU 客户端（浏览器 / GL·Vulkan app /
      游戏 / 视频）走 dmabuf 而非 wl_shm。现在只有 shm CPU 拷贝 → 大多数真实程序不渲染。
      需要 dmabuf import + 零拷贝 scanout。**分水岭，第一优先。**
- [ ] explicit sync（`linux-drm-syncobj-v1`）— 现代 GPU 帧同步
- [ ] **`xdg_popup` + `xdg_positioner`** — 菜单 / tooltip / 下拉框 / 右键菜单
- [ ] xdg-shell 窗口状态：maximize / fullscreen / resize / configure bounds / min-max
- [ ] `wl_subcompositor`（subsurface）— 视频层、装饰、很多 toolkit 依赖
- [ ] **`wl_data_device`** — 复制粘贴 + 拖放（必备）
- [ ] primary-selection（中键粘贴）
- [ ] △ `wl_seat`：补全 touch、pointer axis/scroll（现 touch 仅 stub）

### P1 — 真实桌面必须
- [ ] 多输出 + 热插拔 + output layout + 每输出 scale/transform（现单输出固定）
- [ ] `xdg-decoration` — 服务端/客户端装饰协商（否则无标题栏或双标题栏）
- [ ] `presentation-time`（正确 vsync 反馈）+ 真正的 damage tracking（现每帧全合成）
- [ ] HiDPI：`wp-fractional-scale-v1` + `wp-viewporter`
- [ ] **IME：`text-input-v3` + `input-method-v2`** — 中文/日文输入法
- [ ] libseat 会话管理 + VT 切换（现靠 logind ACL，不能安全切 VT）
- [ ] 光标：`cursor-shape-v1` + 实际绘制光标（`set_cursor` 现为空实现）

### P2 — 完整功能 / 桌面外壳
- [ ] layer-shell（`wlr-layer-shell` 或 `ext-*`）— 面板 / 状态栏 / 壁纸 / 锁屏层
- [ ] `ext-session-lock-v1` — 锁屏
- [ ] 截图/录屏：`wlr-screencopy` 或 `ext-image-copy-capture` + xdg-desktop-portal
- [ ] `wlr-foreign-toplevel-management` — 任务栏列窗口
- [ ] `xdg-activation` — 焦点转移 / 紧急提示
- [ ] idle：`ext-idle-notify` + idle-inhibit（视频防息屏）
- [ ] output-management + gamma-control（夜间模式）
- [ ] data-control（剪贴板管理器）
- [ ] relative-pointer + pointer-constraints（游戏/3D 锁定光标）、tablet-v2、pointer-gestures

### 非协议部分
- [ ] 真正的 XWM（现 xwayland 极简）— 窗口堆叠、焦点、`_NET_*` hints、X 剪贴板桥
- [ ] DRM atomic 模式设置 + GBM（现 `drm.cpp` 279 行，legacy 级，真机未验证）
- [ ] 渲染管线：damage、多层混合、透明/圆角、离屏缓冲

### 建议实现顺序（最短能用路径）
1. **dmabuf import + GBM 分配器** — 不做真实程序全渲染不了
2. **xdg_popup + 窗口状态** — 解锁绝大多数 GTK/Qt app
3. **data_device（剪贴板）+ xdg-decoration**
4. **多输出 + damage + presentation-time**
5. **layer-shell + session-lock** — 有这个才算"桌面"
6. **text-input / input-method** — 中文输入

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
