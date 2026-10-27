# 按层切五个 module，而不是按依赖轻重切两个

`import luminaria;` 付的是 `wayland-server` + `xkbcommon`；上层的三块按层排开，
依赖只朝下走。取代 ADR 0003 的三刀（core / gpu / desktop）。

```
        luminaria.shell        Frame, SceneRenderer, Cursor, DirectScanout,
             │  │              AutoBackend, LayerManager, PopupTree
    ┌────────┘  └────────┐
    ▼                    ▼
luminaria.backend   luminaria.render     输出/输入设备    ←→   像素
    └────────┬───────────┘
             ▼
        luminaria         协议 + 机制内核 + Backend/Output 抽象 + 值类型
```

外加两个与上面正交的：`luminaria.desktop`（高权限桌面协议，只依赖 core）与
`luminaria.text`（fcft + pixman）。

## 为什么要改

ADR 0003 那一刀是**按“链接什么 C 库”切的**，不是按层切的，于是落下三个结构性问题：

1. **`luminaria.gpu` 是个杂烩。** 名字说 GPU，里面却装着 `libinput`、`libseat`、
   `Session`、`AutoBackend`——和外设正交的东西被迫和 Vulkan 绑在一个开关上。
   想在没有 Vulkan 的机器上跑 KMS + libinput，也必须链接整个 GPU 栈。
2. **core 不能自闭环。** `SceneItem` 在 core，`SceneRenderer` 在 gpu；
   `CursorImage`/`CursorManager` 在 core，`Cursor` 在 gpu。只用 core 的混成器
   既拿不到统一的图元渲染器，也拼不出一个能用的光标——所谓“依赖轻的核心”
   其实是半成品，`luminaria.gpu` 才是真正的基础库。
3. **协议准入标准自相矛盾。** `layer_shell` 进了 core 而 `workspace` 进了
   desktop；`text_input` 在 core 而与之双向桥接的 `input_method` 在 desktop。

## 切法

- **`luminaria`** — 机制内核（`Display`、`EventLoop`、`Signal`、`Result`）、
  值类型（`Box`、`Region`、`Transform`、`BlurParams`…）、二者之间的抽象
  （`Backend`、`Output`、`InputEvent`、`OutputLayout`）以及**语义上就是纯 Wayland**
  的协议。没有设备，没有 Vulkan。依赖：`wayland-server`、`wayland-client`、`xkbcommon`。
  （`wayland-client` 只是为了让测试与嵌套后端能在同一个 module 里做客户端。）
- **`luminaria.backend`** — 输出与输入设备：headless、嵌套、DRM/KMS、libinput、
  libseat 会话。**不碰渲染器**，只有 libdrm/libinput/libudev/libseat。
- **`luminaria.render`** — 像素：Vulkan 1.3 渲染器、CPU 合成器、以及三个只为搬运
  buffer 而存在的协议（linux-dmabuf、drm-syncobj、screencopy）。**不碰设备后端**，
  只有 vulkan/libdrm/gbm。
- **`luminaria.shell`** — 有主见的那一半：`Frame`、`SceneItem`/`SceneRenderer`、
  `Cursor`、`CursorManager`、`LayerManager`、`PopupTree`、`DirectScanout`，以及
  `AutoBackend`（在 nested / DRM / headless 之间选，是策略，因此在这一层）。
  它 `export import` 了 backend 与 render，所以混成器只写一句 `import luminaria.shell;`。
- **`luminaria.desktop`** — 高权限桌面协议，**只依赖 core**：一个没有 GPU 的桌面外壳
  也要用它，所以不能挂到 shell 底下。

`backend` 与 `render` 互为兄弟而不是一条链：渲染器拿的是逻辑坐标与
`ScanoutTarget`，从不问是哪个驱动在呈现它们；`Frame` 要两者都在，所以它在 shell。

## 准入标准（取代 ADR 0003 的那一条）

> 一个协议进 core，当且仅当实现它**不需要**本库自己决定的东西——不需要设备、
> 不需要渲染器、不需要场景表。

据此 `layer_shell` 留在 core（它是协议对象；把它摆在哪一层是 `LayerManager` 的
事，而那个在 shell），`linux_dmabuf`/`screencopy`/`drm_syncobj` 进 render
（它们的实现就是 buffer 与 GPU），`workspace`/`foreign_toplevel`/`data_control`/
`output_management`/`session_lock`/`input_method` 留在一个独立的 `desktop`——
它们的问题不是依赖重，而是**高权限**（枚举或操作别人的窗口/选区），
所以不默认注册，也不是 shell 的一部分。

## 后果

- 每一层都能单独构建、单独链接；`--no-default-features` 是真的 core。
- Rust 绑定按同一套模块切（`backend`/`render`/`shell` 三个 feature，`gpu` 作为
  `shell` 的历史别名保留）。眼下的桥还只有一个 compositor-stack TU（它
  `import luminaria.shell`），按模块拆桥是剩下的工作；在那之前
  `backend`/`render` 单独打开只构建对应的 C++ module，不额外暴露 Rust API。
- `BlurParams` 下沉到 core 的 `util/blur.cppm`：渲染器建 Kawase 链要用它，
  场景表要用它，而 render 在 shell 下面。
- 目录跟着模块走：`src/backend/base/`（core 的抽象）、`src/backend/`（backend 实现）、
  `src/render/`、`src/shell/`，协议按归属分在 `src/protocol/{core,render,desktop}/`。
  分区名依旧是扁平的（跟文件名走），所以搬文件不构成 API 变化。
