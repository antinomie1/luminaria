# Luminaria

用现代 C++ 写 Wayland 混成器的库。架在 `libwayland-server` 上（不重造线协议），
Vulkan 渲染，以 C++20 named module 交付 —— `import luminaria;` 就是全部接口，
没有公开头文件。

**它想成为的东西**：一个**有主见的库**，抽象程度落在"基于 wlroots 写"和"手写一个 X11
窗口管理器"之间。凡是每个混成器都得写一遍、写错就是 bug 的公共账本（帧调度、damage 记账、
遮挡剔除、fence 编排、光标合成），库替你写好；凡是构成"这个混成器是什么样子"的决定
（窗口摆在哪、怎么平铺、快捷键、工作区），一律不进来。你仍然自己写每一帧的循环。

**它不想成为的东西**：第二个 wlroots。

## 四个目标

- **低占用、低功耗** — 空闲的桌面应该是**零次 GPU 提交、零次翻页**，不是 60Hz 空转。
  这三个数（空闲提交次数 / 稳态 RSS / 每帧 GPU 耗时 p99）是会进 CI 的硬指标，不是形容词。
- **内存安全靠构造，不靠小心** — 每个 C 句柄 RAII 包装，每个信号监听析构自动摘链，
  没有一处手写 `wl_list_remove`。目标是让悬空指针那一族错误**写不出来**，而不是靠代码评审
  逮住。
- **API 简洁明了** — 25 个协议对象（wlroots 是 73 个），一个 `import`，一层 `Result<T>`，
  异常不跨 C 边界。
- **完整到能日常用** — 而不是停在"能跑个 demo"。

## 现在能跑到哪

真实客户端（Firefox、konsole、weston-terminal）可以连上来、映射窗口、GPU 合成、交互。
合成走纯 GPU 链路且全程无 CPU 停等：客户端 dmabuf 零拷贝导入成 Vulkan 纹理，合成进一块
导出为 dmabuf 的渲染目标，由 DRM atomic 扫描输出；explicit sync 全链路异步。裸机侧有
libseat 会话管理、硬件光标平面、多输出热插拔、每输出 scale/transform 与模式切换。

完整的功能矩阵见 **[docs/features.md](docs/features.md)**。

![tinyluminaria 嵌套运行，里面是 konsole 跑 fastfetch](docs/screenshot.png)

上图是自带的参考混成器 `tinyluminaria` **嵌套**跑在 KDE Plasma 里 —— 最外层那条标题栏是
宿主画的原生装饰，框内是我们自己合成的 800×600 输出，里面跑着 konsole，konsole 里跑
fastfetch，而 fastfetch 认出了 `WM: tinyluminaria (Wayland)`。

## 上手

```sh
xmake f -y --toolchain=clang && xmake        # 需要 clang ≥ 22 或 gcc ≥ 16
WAYLAND_DISPLAY=wayland-0 ./build/examples/tinyluminaria    # 嵌套跑，打印自己的 socket
```

依赖、测试、裸机运行方式见 **[docs/building.md](docs/building.md)**。

## 文档

| 文件 | 内容 |
|---|---|
| [CONTEXT.md](CONTEXT.md) | 词汇表。这个项目里"混成器"/"合成"/"布局"各指什么，以及不指什么 |
| [docs/architecture.md](docs/architecture.md) | 分层、模块结构、一帧的流程、几条会咬人的规矩、已知限制 |
| [docs/features.md](docs/features.md) | 已实现并有测试覆盖的全部功能 |
| [docs/adr/](docs/adr/) | 不可逆的设计决定及其理由 |
| [TODO.md](TODO.md) | 未完成的工作与执行顺序 |

## 稳定性

**1.0 之前不承诺任何 API / ABI 稳定性**，下游请锁 commit。原因写在
[ADR 0004](docs/adr/0004-no-api-stability-before-1.0.md)：已经排上日程的几件事
（删场景树、切模块、表面换代际句柄）每一条都是 break，现在承诺稳定等于把简化永远留在原地。
