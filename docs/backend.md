# 后端

三种后端用同一组调用：`acquire` 取一帧要画的图像（带缓冲年龄），`present` 提交并带上变化的矩形，
`scanout` 把客户端的 dmabuf 直接显示出来，`Presented` 报告翻转完成的时间。

| 后端 | 输出 | 输入 |
|---|---|---|
| DRM | 原子 KMS | libinput（经 libseat）、xkb |
| 嵌套 | 父合成器里的一个窗口 | 父合成器的 seat 和 keymap |
| 无头 | 按刷新率定时的纹理 | `inject` |

`Kind.Auto`：设置了 `WAYLAND_DISPLAY` 就嵌套，否则 DRM。

## 扫描输出缓冲

每个输出 2 或 3 个（`Options.buffers`）由 `vk_gpu.export_dmabuf_with` 导出的渲染目标。取帧时选
最近画过的空闲缓冲，缓冲年龄 = 已呈现帧数 − 它上次被画时的帧数 + 1（从没画过为 0，整帧重画）。
缓冲在显示或排队显示时不会被取到。

## DRM / KMS

- **选卡**：`LUMINARIA_DRM_DEVICE`，否则 `/dev/dri` 下第一个 card；经 libseat 打开，要求原子模式设置
  和通用平面。
- **连接器**：每个已连接的连接器配一个空闲 CRTC、它的主平面和光标平面（有的话）；名字来自类型和
  编号（`DP-3`），厂商和型号从 EDID 解析。
- **缓冲布局**：主平面 `IN_FORMATS` 里 XRGB8888 的修饰符与 GPU 可渲染的修饰符求交，交给驱动挑选；
  平铺、压缩布局比线性省显存带宽。每个缓冲导入成 framebuffer（`prime_fd_to_handle` +
  `AddFB2WithModifiers`）。
- **提交**：一次非阻塞原子提交，带 `PAGE_FLIP_EVENT`：
  - 首帧或模式变化时设 CRTC 的 `MODE_ID`、`ACTIVE`、`VRR_ENABLED` 和连接器的 `CRTC_ID`；
  - 主平面的 `FB_ID`、源与目标矩形、`IN_FENCE_FD`（GPU 的 sync_file，内核等，CPU 不等）、
    `FB_DAMAGE_CLIPS`（变化的矩形）；
  - 光标平面的位置或隐藏。
- **翻转事件**：直接读 DRM fd 解析 `drm_event_vblank`，得到序号和 CLOCK_MONOTONIC 时间戳；排队的缓冲
  变成显示中，之前显示的缓冲空出来。
- **直接扫描输出**：客户端 dmabuf 按缓冲缓存 framebuffer，先 `TEST_ONLY` 再真正提交；客户端允许撕裂
  且驱动支持时用 `PAGE_FLIP_ASYNC`。
- **光标**：光标平面上一个 dumb buffer（尺寸取 `DRM_CAP_CURSOR_WIDTH/HEIGHT`）。指针移动时，没有
  翻转挂起就立即提交只含光标的原子请求，否则在下一次翻转完成后补上：光标按输入速率移动，不等帧。
- **其他**：`set_mode` 换模式（下一帧按新尺寸重建缓冲）、`enable` 关屏（`ACTIVE` 0）、`set_vrr`、
  udev 热插拔（重扫连接器，增删输出）、`probe` 只读列出已连接的显示器而不接管。

## 会话与输入设备

- **libseat**（seatd、logind 或 noop）：设备经它打开，切 VT 时被收回。禁用时停止提交、暂停 libinput；
  恢复时重新模式设置、恢复 libinput，发 `Session`。
- **libinput**：udev seat 上下文，设备的打开和关闭走 libseat。键和按钮按 seat 计数，只报第一次按下和
  最后一次抬起。指针（相对、绝对）、滚动（滚轮 v120、手指、连续）、触摸、滑动 / 捏合 / 按住手势、
  设备增删都变成纯数据的 `Input`。`configure_input` 设置轻触点击、自然滚动、加速、左手、禁用。
- **xkb**：按 RMLVO 名字编译 keymap，键在这里就算出 keysym 和文字；序列化的修饰键变化时单独报出。
  keymap 文本放进一个 memfd，所有客户端共享。

## 嵌套

- 父合成器里一个 xdg 窗口就是输出；窗口 configure 改尺寸时报 `OutputChanged`，下一帧按新尺寸重建缓冲；
  关窗口时报 `Closed`，`next` 随之结束。
- 装饰：父合成器有 `zxdg_decoration_manager_v1` 就在首次提交前请求服务端装饰，标题栏、边框、
  关闭按钮由父合成器画（KWin、sway 等）；没有它（GNOME）时窗口不带装饰。
- 帧：导出的 dmabuf 经父合成器的 linux-dmabuf（v3，`create_immed`）包成 `wl_buffer`；提交前用
  `DMA_BUF_IOCTL_IMPORT_SYNC_FILE` 把 GPU 栅栏挂成 dmabuf 的隐式写栅栏，父合成器等 GPU，我们不等；
  只 `damage_buffer` 变化的矩形；父合成器的帧回调当作 vblank。
- 节奏：帧回调一到、有东西要画就马上开始，不再按预测的 vblank 推迟（那会错过父合成器自己的截止
  时间）；至少 3 个缓冲，因为父合成器拿着正在显示的，可能还拿着排队的；缓冲释放时，因为没有空闲
  缓冲而放弃的帧立刻重排。
- 回复（pong、configure 的 ack）每次分派后立刻 flush，空闲时也不会被父合成器判成无响应。
- 直接扫描输出：铺满窗口的客户端 dmabuf 包成父合成器的 `wl_buffer` 直接 attach，父合成器释放它之前
  不释放给客户端。
- 输入：父合成器的指针（窗口内绝对坐标）、按钮、滚动，键盘的 keymap、按键和修饰键。
- 光标：光标图像作为父合成器的 shm 光标表面（父合成器的硬件光标）。

## 无头

每个输出一个 timerfd：提交后在一个刷新周期后报 `Presented`，帧时序和真实显示器一样。`read_output`
读回当前显示的纹理，用于测试和截图。
