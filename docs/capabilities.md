# 能力文档：用 luminaria 拼一个混成器

> 这份文档回答"luminaria 替我做了什么、还差什么"，按混成器真正要写的东西组织，
> 而不是按模块。功能矩阵与测试见 [features.md](./features.md)，分层与规矩见
> [architecture.md](./architecture.md)。

## 边界先说清

luminaria 管**机制**，不管**策略**：

- **窗口管理不是库的事。** 窗口摆在哪、怎么平铺、工作区怎么切，混成器自己实现。
  库给的是"把一棵表面树画在这里"的原语，以及"这棵树现在长什么样"的遍历
  （`Surface::surface_tree()`）。
- **快捷键绑定不是库的事。** 库给的是键盘状态（`KeymapState`），按哪个 keysym
  触发什么动作是混成器的策略。
- **窗口装饰不是库的事。** `xdg-decoration` 默认答 client-side，库不画标题栏。

## 合成（Compositing）

两条路径，都是"把一棵表面树和一个 z 序列表变成像素"：

| 路径 | 入口 | 用在哪 |
|---|---|---|
| GPU | `Frame`（`luminaria.gpu`）| 有 Vulkan 的混成器。零拷贝、damage 记账、fence 编排、直出判断全包 |
| CPU | `CpuCompositor`（`luminaria`）| 没有 GPU 的地方：headless、嵌套、测试、screencopy 前置。纯 CPU 混合，无 Vulkan 依赖 |

两者吃**同一种 z 序列表**——混成器每帧现搭一串"画什么、画在哪"，按列表顺序
从后往前画。列表由两种图元组成：

```cpp
using CpuItem = std::variant<RectFill, CpuView>; // CPU 路径的图元
```

`CpuView` 是一棵客户端表面树的根（拖带全部 subsurface），`RectFill` 是混成器
自己的纯色矩形。`CpuCompositor` 处理 subsurface 树、buffer scale（HiDPI 抽稀）、
`set_buffer_transform` 旋转、`wp_viewporter` 裁剪，全部最近邻采样 + 预乘 alpha
source-over：

```cpp
CpuCompositor cpu;
std::vector<CpuItem> items;
items.emplace_back(RectFill{{0, 0, 800, 600}, kWallpaper}); // 混成器自己的底色
for (const Window& w : windows) items.emplace_back(CpuView{w.id, w.x, w.y});
cpu.composite(800, 600, kBackground, items);
output.commit_frame(cpu.pixels(), cpu.width(), cpu.height()); // 交给 wl_shm 呈现
```

## 图元（Primitives）

混成器-owned 的绘制原语，与客户端表面同一条摆位/渲染路径，因此 z 序、damage、
diff 全自动对齐：

- **纯色矩形**：CPU 路径是 `RectFill` 图元；GPU 路径是
  `Frame::place_rect(x, y, w, h, color)`（边框、背景面板、遮罩、光标底）。它不参与
  命中测试，也不报自己的 damage——移动或换色由摆位 diff 恢复，代价恰好是两个矩形。
- **纹理**：`Frame::place(GpuTexture, ...)`，比如光标位图、离屏整窗结果、壁纸。
- **背景**：`submit(background)` / `composite(..., background, ...)` 的底色参数。

## 节拍（Frame callbacks / presentation）

`wl_surface.frame` 不在 commit 时应答——那样客户端会画出永远不上屏的帧。它攒到
混成器说"这一帧真的上屏了"才发。库替你把"遍历本帧画过的表面、逐个发"这一步
收成批量 helper，**时间戳仍由混成器提供**（它知道自己的时钟）：

```cpp
// GPU：这一帧摆位里的每个表面，同一个时间戳
output.present -> frame.send_frame_done(pe.time_ms());
// 非 Frame 的混成器（CPU 路径）：任意表面 id 列表
luminaria::send_frame_done(ids, time_ms);
```

两条规矩值得记住（两个示例混成器都这么写）：

1. **`submit()` 答 `Presented::unchanged` 时没有 `present`**——那帧没提交、没翻页。
   那次 `send_frame_done()` 得在 `frame` 处理里补上，否则"commit 了但没报 damage"
   的客户端永久冻住。
2. **present 里先答帧回调，再答 presentation feedback**——两个都只在这一刻为真。

## 键盘状态（KeymapState）

`KeymapState`（`luminaria`）是一个 RAII 的 xkb 包装：编译 keymap（按布局名或按文本）、
喂 evdev 键码、读出 keysym 与修饰键掩码。接口只有键盘**状态**，绝无绑定或动作：

```cpp
auto km = KeymapState::from_layout("us");          // 或 from_text(客户端给的 keymap)
km->update_key(42, true);                          // 左 Shift 按下
km->keysym(30);                                    // KEY_A → XKB_KEY_A
km->modifiers();                                   // 掩码，直接进 wl_keyboard.modifiers
```

`LibinputBackend` 内部就持有一个 `KeymapState`（`keymap_state()` 可读），所以
"裸机按键 → 修饰键掩码"与"快捷键查找"用同一个状态，不会各算各的。keymap 文本
（`keymap()` / `KeymapState::text()`）交给 `Seat::set_keymap()`，客户端拿到的
布局与混成器算的 keysym 必然一致。

## 输入

后端（libinput 裸机 / 嵌套转发）发出后端无关的事件信号
（`KeyEvent` / `PointerMotion*` / `PointerButton` / `PointerAxis` / `ModifiersEvent`），
混成器路由进 `Seat`。命中测试用 `Frame::surface_at()`（GPU）或各表面自己的
`accepts_input()` 在布局坐标上做——点击不可能跟像素对不上。

## 协议覆盖

30 个 Wayland global 全部实现（`xdg-shell`、`layer-shell`、`seat`、`data-device`、
`presentation-time`、`linux-dmabuf`、`screencopy`、`session-lock`、`input-method`、
`text-input`、`workspace`、`foreign-toplevel`……），矩阵见 [features.md](./features.md)。

## 先别抽象的东西

- **光标"当前该显示什么"的解析**（cursor theme + 客户端 cursor surface + cursor
  shape + cursor plane 的合成决策）：库已有全部零件（`cursor_theme`、`set_cursor`
  信号、`cursor_hidden()`、硬件光标平面），但"画在 plane 上还是合成"与输出策略
  纠缠，先让混成器自己拼，不急着给一个半吊子的 helper。
- **模糊**：协议侧已就绪（`Surface::blur_region()` + x-ray 路径），非 x-ray 的
  真实采样留给视觉策略落地时再说。
