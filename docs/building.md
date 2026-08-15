# 构建、测试、运行

> 面向想把 Luminaria 编起来跑一遍的人。设计与结构见 [architecture.md](./architecture.md)。

## 依赖（Fedora）

核心：

```sh
sudo dnf install -y \
  gcc-c++ xmake pkgconf-pkg-config \
  wayland-devel wayland-protocols-devel \
  libxkbcommon-devel \
  vulkan-loader-devel vulkan-headers glslang mesa-vulkan-drivers \
  mesa-libgbm-devel libdrm-devel \
  libinput-devel libseat-devel systemd-devel
```

Xwayland（可选）：

```sh
sudo dnf install -y \
  xorg-x11-server-Xwayland libxcb-devel xcb-util-wm-devel
```

编译需 **clang ≥ 22**（`xmake f --toolchain=clang`）或 **gcc ≥ 16** —— 更早的版本编不动
模块分区。目前实际验证过的是 clang 22：库、示例与全部测试构建通过，`xmake test` 全绿。
另需 Vulkan、xkbcommon、
libdrm、libinput、libudev、**libseat**、wayland-protocols，外加
**`glslangValidator`（glslang 包）**—— 渲染器的纹理四边形管线用它把 GLSL 编成 SPIR-V 并
直接嵌进二进制（运行时不读 shader 文件）。

光标主题不需要 libXcursor：XCursor 文件格式的解析器就在
`cursor_theme.cppm` 里（含主题继承与动画帧），免去把 X11 拖进 Wayland
compositor。

---

## 构建 & 测试

```sh
xmake f -y      # 配置：顺带跑 wayland-scanner 和 glslangValidator
xmake           # 构建库与示例
xmake build -a  # 连测试二进制一起
xmake test      # 跑全部测试
xmake f -m release && xmake   # 优化构建（默认是 debug）
```

多数测试用**进程内 `libwayland-client`**（socketpair）驱动真实协议，无需 GPU /
父 compositor；Vulkan 测试用真 GPU；`wayland-nested` 连真父 compositor（有
`WAYLAND_DISPLAY` 才跑）。跑不了的测试以 **exit 77** 自我跳过（无 GPU / 无空闲 VT /
无 seat），`xmake test` 会把原因打出来并记为通过。

## 运行

**嵌套**（在现有桌面里开一个窗口，暴露自己的 `WAYLAND_DISPLAY` 供客户端连接）：

```sh
WAYLAND_DISPLAY=wayland-0 ./build/examples/tinyluminaria &
WAYLAND_DISPLAY=<打印的 socket 路径> weston-terminal
# 或 LUMINARIA_BACKEND=headless 强制无头模式；LUMINARIA_OUTPUT=1280x800 改输出尺寸
```

**裸机**（需切到空闲 VT、停掉桌面，对该 VT 有 DRM master + 输入 ACL）：

```sh
./build/examples/luminaria-tty                 # 自动探测 /dev/dri/card*，可传 card1 显式指定
```

`luminaria-tty` 是完整裸机 compositor：DRM 输出 + libinput 输入 + wl_compositor /
xdg-shell / seat，每帧用 Vulkan 合成 mapped 客户端窗口后扫描到显示器，键盘路由到
聚焦窗口，Esc 退出。它打印自己的 `WAYLAND_DISPLAY`，从另一 VT / ssh 指客户端过来：

```sh
WAYLAND_DISPLAY=wayland-1 weston-terminal
```

**截图 / 录屏**（compositor 跑起来后，任一后端，指向它的 `WAYLAND_DISPLAY`）：

```sh
sudo dnf install -y grim wf-recorder            # 依赖 screencopy 协议的现成客户端
WAYLAND_DISPLAY=<socket> grim shot.png          # 整屏截图 → PNG
WAYLAND_DISPLAY=<socket> wf-recorder -f out.mp4 # 录屏（Ctrl-C 停）
```

`grim` 走 `wlr-screencopy-unstable-v1`，把当前帧的 GPU 合成结果拷进它自己的 shm
buffer；`wf-recorder` 逐帧拉流。示例 compositor 已注册截图 manager 并挂好每个
输出的 capture 回调。

---

