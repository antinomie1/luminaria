# 协议层

## 线协议（`luminaria_wire`）

不依赖 libwayland，用 Mica 实现 Wayland 线协议，能和 libwayland 客户端互通。

- **接收**：每个连接一个 8 KiB 的固定缓冲，一次 `recvmsg` 填满，最多同时收 64 个 fd。消息在缓冲里
  原地解码：数字直接读，只有字符串和数组拷贝。
- **发送**：事件编码进一个发送缓冲，`flush` 一次交给内核；超过 28 个 fd 时按 libwayland 的上限分批，
  每个 fd 不晚于它的消息到达。
- **稳态不分配**：缓冲和对象表复用。
- **内存有界**：对端不读，积压超过 1 MiB 就断开。
- **对象表**：稠密数组，每项 8 字节。客户端的新 id 只能是下一个或已释放的，不能让表无限增长。
  析构请求自动释放 id 并发 `delete_id`；发给已销毁对象的事件在客户端一侧被解码后丢弃。
- **检查**：参数的接口、空值、长度、`since` 版本都检查，失败发 `wl_display.error` 并断开。
  高于对象版本的事件不发。

## 生成的接口（`luminaria_proto`）

`luminaria_scanner` 从 `protocols/*.xml` 生成，每个协议文件一个模块：

- 类型化 id（`wl.Surface`，`derive(Hash, Show)`）
- 请求枚举，用 `proto.next` 读
- 事件发送函数，是类型化 id 上的方法；析构事件自动释放 id
- 协议枚举，位域用 `Flags`
- 客户端一侧也生成（`proto.next_event`、请求方法），给嵌套后端和测试用；不调用就被链接器剔除

## 服务端状态（`luminaria_server`）

没有 GPU，也没有策略。需要决定的请求以事件出来，合成器通过方法回应。

### 存储

- 每类对象一个 slab：稳定槽位加代数（奇数表示使用中）。对外句柄是 `(index, gen)`，失效的句柄不会
  指到复用了槽位的新对象。
- 每个客户端一张表：协议 id → 对应种类 slab 的槽位。请求进来时先查表，再按接口分派到各模块。
- 客户端断开时，按 id 从大到小销毁它的所有对象（子对象先于父对象）。

### 提交与事务

每个表面有三份双缓冲状态：`pending`（请求写入）、`cached`（同步子表面暂存）、`current`（生效）。

1. `wl_surface.commit` 把 `pending` 快照下来。
2. 表面自己或任一祖先是同步子表面：并入 `cached`，等父表面提交时一起带走。
3. 否则组成一个**事务**：这次的状态，加上所有同步子孙的 `cached` 状态。
4. 事务需要等待时发 `Committed(txn)`，合成器（门面）解决 `waits(txn)` 后调 `ready(txn)`；不需要等待且
   该客户端没有排队的事务时立即生效。
5. 同一客户端的事务按顺序生效。慢栅栏只挡住这个客户端自己，挡不住别人。

等待的种类：

| `Wait` | 含义 | 门面怎么等 |
|---|---|---|
| `Implicit(buffer, fd)` | dmabuf 的隐式写栅栏 | 导出成 sync_file 放进 epoll |
| `Timeline(fd, point)` | linux-drm-syncobj 获取点 | 转成 eventfd 放进 epoll |
| `Fifo(surface)` | wp_fifo 屏障还没被呈现解除 | 等该表面下一次被显示（不可见的表面不等） |
| `Time(ns)` | wp_commit_timing 的目标时间 | 定时器 |

事务生效时：换缓冲（更新使用计数）、合并损坏（超过 16 个矩形就合成外接矩形）、更新区域、变换、
缩放、视口、帧回调、呈现反馈（之前未呈现的反馈发 `discarded`）、fifo 屏障、tearing 提示、
xdg 几何与尺寸限制、子表面位置与叠放，然后重算表面尺寸并发 `Applied(surface)`。门面用
`take_changes` 取走变化位和损坏。

### xdg-shell

| 时机 | 事件 |
|---|---|
| 初始提交（标题和 app id 已知） | `WindowNew` / `PopupNew` |
| ack 了 configure 后的第一个带缓冲的提交 | `WindowMapped` / `PopupMapped` |
| 每次 ack 的 configure 随提交生效 | `WindowAcked(w, width, height)` |
| 提交空缓冲 | `WindowUnmapped`（回到需要重新初始提交的状态） |
| 销毁 | `WindowClosed` / `PopupGone` |
| move、resize、maximize、fullscreen、minimize、window menu | `WindowRequest` |

还没 ack configure 就提交缓冲、给已有角色的表面再给角色等，都按协议报错。弹出菜单位置由 positioner
计算：锚点矩形、锚边、重力、偏移，超出约束矩形时按客户端允许的顺序翻转、滑动、缩小。

### seat

- 每个 seat 记住键盘焦点、指针焦点和触点所在的表面；事件只发给焦点客户端的对象。
- 键盘：keymap memfd 共享给所有客户端；enter 带已按下的键；修饰键变化才发 `modifiers`。
- 指针：enter / leave 配对；每组事件后发 `frame`（v5+）；滚轮按版本发 `axis_value120`（v8+）或
  `axis_discrete`，手指滚动结束发 `axis_stop`。
- `set_cursor` 只接受指针所在客户端、带最近 enter serial 的请求，以 `Cursor` 事件交给门面。
- serial 全局递增。

### 剪贴板

`wl_data_device_manager` 的选区按 seat 保存。键盘焦点客户端收到一个 data offer；它要哪种 MIME 就把管道
fd 直接转给源客户端，数据不经过合成器。拖放目前拒绝（源被取消）。

### 输出

每个输出一个 `wl_output` global，绑定时发几何、模式、整数缩放（向上取整，客户端缩小）、名字、描述和
`done`，变化时重发。表面所在输出变化时发 enter / leave。首选缩放用 wp_fractional_scale（客户端要了
的话）和 `wl_surface` v6 的提示。

### 扩展协议

| 协议 | 版本 | 作用 |
|---|---|---|
| linux-dmabuf | 4 | 共享格式表（sealed memfd）、主设备、扫描输出分组优先 |
| linux-drm-syncobj | 1 | 获取点与释放点（GPU 支持时间线 syncobj 才公布） |
| presentation-time | 1 | CLOCK_MONOTONIC 时间戳、刷新间隔、vblank 序号、零拷贝标志 |
| viewporter | 1 | 源矩形与目标尺寸 |
| fractional-scale | 1 | 首选缩放（1/120） |
| fifo | 1 | 屏障与等待 |
| commit-timing | 1 | 目标呈现时间 |
| tearing-control | 1 | 允许撕裂（直接扫描输出时异步翻转） |
| wl_fixes | 1 | 销毁 registry |

核心协议的版本：`wl_compositor` 6、`wl_subcompositor` 1、`wl_shm` 2（ARGB/XRGB/ABGR/XBGR8888）、
`wl_data_device_manager` 3、`wl_seat` 9、`wl_output` 4、`xdg_wm_base` 6。
