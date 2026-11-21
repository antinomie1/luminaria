# 对外接口

合成器只接触三样东西：`Display`、句柄（`Window`、`Output`、`Client`、`Surface`、`Image`，都是可复制、
可作 `Map` 键的 id）和 `Event`。

## 事件循环

```mica
var d = try lum.open(lum.Options {})
while true {
    guard let Some(e) = try d.next() else {
        break
    }
    match e {
        // 处理关心的
        _ => d.accept(e)
    }
}
```

`next` 的语义只有一条：队列里有事件就立即返回；队列空了，先完成到期的帧、给每个客户端 flush
一次，再阻塞在 epoll 上。所以：

- 一批事件处理完才出帧，这批事件引起的布局变化都赶得上这一帧。
- 一个客户端在一次循环里收到的所有事件只用一次 `sendmsg`。
- 空闲时没有任何定时器，进程不被唤醒。

返回 `None` 表示结束：合成器调用了 `quit`，或嵌套模式下父合成器关了窗口。

| 接口 | 用途 |
|---|---|
| `lum.open(Options)` | 内置输出端（`Kind.Auto`：有 `WAYLAND_DISPLAY` 就嵌套，否则 DRM）和 Vulkan 渲染器，开 socket |
| `display.Display.with(Options, o, r)` | 任意输出端和渲染器（`plug.Output`、`plug.Renderer`） |
| `socket()` | 给客户端的 `WAYLAND_DISPLAY` |
| `next()` / `poll()` / `fd()` | 阻塞取事件 / 不阻塞取事件 / 嵌进别人的循环时要监听的 fd |
| `watch(fd, token)` / `unwatch(fd)` | 合成器自己的 fd 进同一个循环，可读时产生 `Ready(token)` |
| `timer(ms, token)` | `ms` 毫秒后产生 `Timer(token)` |
| `inject(Input)` | 当作设备送来的输入（远程输入、测试） |
| `quit()` | 让 `next` 返回 `None` |

`Options` 的字段：`socket`、`backend`、`headless`（无头输出的模式）、`keymap`（RMLVO 名字）、
`repeat_rate` / `repeat_delay`、`buffers`（每个输出的扫描输出缓冲数，2 或 3）、`background`、
`cursor_theme_size`、`title`（嵌套窗口标题）、`validation`、`realtime`（DRM 时事件循环和提交线程用
实时调度，默认开）。

## 事件与 `accept`

`accept(e)` 按协议惯例处理事件。合成器只拦下想改的，其余交给它。

| 事件 | `accept` 的默认动作 |
|---|---|
| `OutputAdded` / `OutputRemoved` / `OutputChanged` | 无（新输出已自动排在已有输出右侧） |
| `Frame(o, vblank_ns)` | 无。这是推进动画、更新布局的时机；`vblank_ns` 是预测的呈现时间 |
| `ClientNew` / `ClientGone` | 无 |
| `WindowNew(w)` | configure 0×0（客户端自定尺寸） |
| `WindowMapped` / `WindowUnmapped` / `WindowClosed` | 无 |
| `WindowTitle` / `WindowAppId` / `WindowParent` | 无 |
| `WindowResized(w, wd, ht)` | 无（客户端按新尺寸画好了） |
| `RequestMove(w)` / `RequestResize(w, edges)` | 启动库内移动 / 缩放（见 [input.md](input.md)） |
| `RequestMaximize(w, on)` / `RequestFullscreen(w, o, on)` | 按窗口所在（或指定）输出的逻辑尺寸 configure |
| `RequestMinimize` / `RequestMenu` | 无 |
| `Occluded(w, hidden)` | 重发上次的 configure，`suspended` 随 `hidden` |
| `Key(k)` | 发给键盘焦点；Ctrl+Alt+Fn（`XF86Switch_VT_n`）切 VT |
| `Modifiers` | 无（修饰键状态在库内已经发给焦点客户端） |
| `Motion` / `Button` / `Scroll` / `Touch` | 命中测试后发给对应表面，处理 enter/leave、隐式抓取、点外面关弹出菜单 |
| `Gesture` | 发给指针下的客户端（pointer-gestures）；`manipulate_pan` 进行中则由库消费 |
| `WindowDecoration(w, server_side)` | `decorate(w, false)`：库不画标题栏，客户端自己画 |
| `WindowIcon(w)` | 无（`icon(w)` 取图标） |
| `RequestActivate(w)` | `focus(w)`（客户端带着有效的激活令牌） |
| `Bell(w)` | 无 |
| `Locked(locked)` | 无（锁屏期间库只显示锁屏表面、只把输入给它） |
| `OutputArea(o)` | 无（面板改变了 `usable(o)`；最大化按它的尺寸 configure） |
| `OutputConfig(c, test)` | `apply_output_config(c, test)` 后 `output_config_done`：模式须是当前模式或（DRM 下）输出自己的模式，不支持旋转；禁用即 `enable(o, false)` |
| `DeviceAdded` / `DeviceRemoved` | 无 |
| `Moved` / `Resized` / `Panned` | 无：操纵结束，合成器把结果并入自己的布局 |
| `Session(active)` | 无（库已暂停或恢复设备） |
| `Ready` / `Timer` | 无 |

`Key` 带 `code`（evdev）、`sym`（xkb keysym，常量在 `lum.backend.keysym`）、`text`、`pressed`、`mods`。
`Pointer` 带全局逻辑坐标 `x, y`、`dx, dy`（加速后）和 `raw_dx, raw_dy`。

## 布局

```mica
d.layout(fn(mut l) {
    l.image(wallpaper, 0.0, 0.0, 2560.0, 1440.0, lum.Style {})
    l.window(w, 100.0, 80.0, lum.Style { radius: 8.0, shadow: 24.0, transition_ms: 150 })
})
```

- 整张放置表替换，自下而上，全局逻辑坐标。一直有效到下次调用。
- `window` 把窗口几何的左上角放在给定位置；子表面和弹出菜单自动跟随。
- `image` 是合成器自己的纹理（`d.image(w, h, pixels, opaque)` 创建，`update_image(img, x, y, w, h,
  pixels, stride)` 局部更新：`pixels` 是整幅图像，只拷 `x, y, w, h`；`drop_image` 释放），用于壁纸、
  服务端装饰、面板。
- `Style`：`opacity`、`radius`、`shadow`、`scale`（缩略图；输入坐标同步缩放）、`transition_ms`
  （从上一次的位置和不透明度动画过去，由库插值）。
- 每个输出只拿到它显示的那部分；只有变化的部分被重绘。

## 窗口

| 接口 | 用途 |
|---|---|
| `configure(w, width, height, States)` | 尺寸（0 让客户端自定）与状态（maximized、fullscreen、resizing、activated、tiled_*、suspended），返回 serial |
| `close(w)` | 请客户端关闭 |
| `focus(Option[Window])` / `focused()` | 键盘焦点；有弹出菜单抓取时焦点留在菜单上 |
| `window_at(x, y)` | 全局坐标下的窗口（子表面和弹出菜单算作所属窗口） |
| `title` / `app_id` / `parent` / `size` / `size_hints` / `client` / `credentials` | 查询 |
| `sandbox(c)` | 客户端所在沙盒（security-context）：引擎、app id、实例 id |
| `icon` / `tag` / `modal` / `content_type` | 图标、跨重启的标签、模态、内容类型（照片、视频、游戏） |
| `decorate(w, server_side)` | 谁画标题栏：合成器（在布局里放图像）或客户端 |
| `visible(w)` | 是否有任何部分在某个输出上可见 |
| `activation_token()` | 合成器启动程序时放进 `XDG_ACTIVATION_TOKEN`，它的窗口就能拿焦点 |

## 桌面外壳

库按协议自己处理，合成器只需要知道结果：

- **layer 表面**按锚定、外边距和独占区排在各自输出上：背景层和底层在布局下面，顶层和覆盖层在布局上
  面。`usable(o)` 是面板留下的区域（`OutputArea` 通知变化），合成器在里面摆窗口；`accept` 的最大化
  也按它。键盘交互为 exclusive 的顶层 / 覆盖层表面拿走键盘；on-demand 的点击后拿到，直到合成器
  `focus` 别的窗口。
- **锁屏**：收到锁屏请求立即只显示锁屏表面（没有的输出显示背景色），每个输出出完一帧才告诉锁屏客户端
  `locked`；输入只给锁屏表面，`focus` 记下但不生效，解锁后恢复。`locked()` 查询。
- **输入法**候选窗放在文本光标下方，在一切之上。
- **空闲**：输入时通知恢复，到时间发空闲；可见表面上的 idle-inhibit 挡住普通通知。`idle_inhibited()`
  供合成器自己的熄屏策略参考。
- **快捷键抑制**：`shortcuts_inhibited()` 为真时，焦点客户端要求拿到所有按键，绑定应放行。
- **拖放**：拖动期间指针事件改走数据设备，拖动图标画在指针处；合成器不用做什么。
- **截屏录屏**：有客户端等帧时库自己出帧、读回、拷给它。

## 布局事务

```mica
d.begin()
discard d.configure(a, 640, 720, lum.States {})
discard d.configure(b, 640, 720, lum.States {})
d.layout(fn(mut l) { ... })
d.end(100)
```

`begin` 与 `end` 之间的 configure 和布局一起生效：等每个窗口按新尺寸提交后，新缓冲和新布局在同一帧
出现；超时（这里 100 ms）就按已到的生效，慢客户端拖不住整个事务。

## 操纵

| 接口 | 用途 |
|---|---|
| `manipulate_move(w)` | 跟着指针移动窗口，松开按键时发 `Moved(w, dx, dy)` |
| `manipulate_resize(w, edges)` | 从边（上 1、下 2、左 4、右 8）缩放，松开时发 `Resized(w, dx, dy, width, height)` |
| `manipulate_pan()` | 下一次滑动手势平移整个布局，带惯性，结束时发 `Panned(dx, dy)` |

偏移一直叠加在布局上，直到收到结果后合成器的下一次 `layout`。

## 输出、指针、输入设备

| 接口 | 用途 |
|---|---|
| `outputs()` / `output_rect(o)` / `output_desc(o)` / `output_under(x, y)` | 查询 |
| `set_scale(o, s)` / `set_position(o, x, y)` / `set_mode(o, mode)` | 缩放（客户端按它渲染）、位置、模式 |
| `enable(o, on)` / `set_vrr(o, on)` | 关屏省电、可变刷新率 |
| `output_config(c)` / `apply_output_config(c, test)` / `output_config_done(c, ok)` | 输出配置工具（wlr-randr、kanshi）的请求 |
| `pointer()` / `warp(x, y)` | 指针位置 |
| `set_cursor_named(name)` / `hide_cursor()` | 主题光标 |
| `configure_input(device, InputConfig)` | 轻触点击、自然滚动、加速、左手、禁用 |
| `set_keymap(Names)` / `mods()` | 键盘布局、当前修饰键 |
| `read_output(o)` | 无头输出当前显示的像素（测试、截图） |
| `stats(o)` / `reset_stats()` | 输出 `o` 测到的 `types.Stats`（见 [compose.md](compose.md#测量)）/ 重新开始计 |

## 扩展协议

服务端不懂的协议按消息签名在运行时加上，不必改库：

| 接口 | 用途 |
|---|---|
| `extend(Interface, global)` | 加一个接口（名字、版本、请求和事件的签名，同 libwayland 的 `wl_message`：`i u f s o n a h`，`?` 表示可空；请求可标 `destructor`），`global` 时成为可绑定的 global；返回它的序号 |
| `ExtensionBound(o, version)` | 客户端绑定了它 |
| `ExtensionRequest(o, op, args)` | 对它的对象的请求，参数按签名解码（`server.ext.Arg`）；`accept` 关掉其中的 fd |
| `ext_send(o, op, args)` | 按签名编码发事件 |
| `ext_new(client, k, version)` / `ext_destroy(o)` | 服务端创建对象（放进事件的 `n` 参数）/ 释放对象 |

只有扩展协议的消息走这条通用路径（每条消息会分配参数表）；内置协议仍是生成的零分配代码。

## 换输出端和渲染器

`plug.Output` 和 `plug.Renderer` 是两个结构化 trait：类型有同名同签名的方法就满足，不用声明实现。
`display.Display.with(o, output, renderer)` 用它们组装；`Display` 单态化，没有间接调用。必需的只有
各自的核心方法（输出端：事件、fd、输出描述、取帧 / 给目标 / 呈现 / 退还目标；渲染器：输出、放置表、
纹理、shm 上传、渲染目标、渲染、读回），其余是带默认实现的可选能力（直接扫描输出、光标平面、键盘、
模式设置、dmabuf 导入、遮挡判断、设备计数 `stats`）。`tests/plug.mica` 是一个定时器驱动的输出端加一个软件渲染器。

## 底层

`luminaria` 同时导出 `types`、`server`、`proto`、`wire`、`plug`、`clock`、`fence`，开着对应特性时还有
`backend`、`compose`。只用协议层、自己做渲染或自己管设备时，可以绕过 `Display` 直接用下层包。
