# 切成四个 module，并给核心 module 定一条协议准入标准

`import luminaria;` 不再意味着链上 Vulkan + libdrm + libinput + libseat + xcb 全家桶。
交付面切成：

- **`luminaria`** — 协议对象 + 核心 + headless 后端。
- **`luminaria.gpu`** — Vulkan 渲染器、DRM/KMS 后端、dmabuf、explicit sync、直出。
- **`luminaria.xwayland`** — X11 桥。
- **`luminaria.desktop`** — 桌面外壳组件专用协议（见下）。

## 准入标准

> 一个协议如果**只服务桌面外壳组件**，或者它的语义是**"操作别人的窗口"**，
> 就不进核心 module。

据此移出核心的：`ext-workspace-v1`（工作区是摆放策略的协议投影，而摆放策略明确不进本库）、
`wlr-foreign-toplevel-management`、`wlr-data-control`。三者都是让一个客户端枚举并操作
**所有别人的**窗口/选区的高权限口子，不该出现在默认 global 列表里。

**移出不是删除**：这三个协议是"能日常用的桌面"的硬需求（任务栏、pager、剪贴板管理器），
删掉就是让每个下游重写一遍——那正是 wlroots 让人难受的地方。它们的问题只是不该默认注册。

## 为什么切三刀而不是更细

更细就变成第二个 wlroots 的 meson option 地狱：几十个开关、组合爆炸、大部分组合没人测过。
三刀对应三个真实存在的部署形态（嵌套/无头的下游、裸机 GPU 混成器、要 X11 兼容的桌面），
第四个 module 对应"我是桌面外壳"这一个身份。

C++ module 无法条件化 `export import`，所以这是**分模块**而不是编译期开关——下游 `import`
什么就付什么依赖，这也是低资源目标的一部分：不装载的东西不占内存。
