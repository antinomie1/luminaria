# AGENTS.md — luminaria 开发指南

## 1. 设计目标

| 目标 | 工程约束 |
|---|---|
| **简单** | 机制与策略分离：`Display` 提供事件流、`accept` 默认行为与即时放置表 `layout`，没有回调网与引用计数 |
| **模块化** | `luminaria_server` / `luminaria_backend` / `luminaria_compose` 互不依赖，只通过 `luminaria_types` 纯数据交互，由 `luminaria` 门面接线 |
| **可插拔** | 结构化 trait `plug.Output`（继承 `plug.Input`）与 `plug.Renderer`，`Display[O, R]` 单态化，没有间接调用；自定义输出端可内嵌 `backend.Devices` 复用 libseat/libinput/xkb；扩展协议由 `d.extend` 运行时挂载 |
| **内存安全** | `unsafe` 只在 FFI、映射与线协议缓冲边界，且先查边界；客户端给的数值在请求入口校验或夹紧（坐标尺寸 ±2^24 px），之后的 i32 运算不会溢出；客户端内存只经 SIGBUS 安全窗口读取；fd 所有权明确，断开即关；`luminaria_server/tests/fuzz.mica` 守住"任何客户端都弄不崩、弄不漏服务端" |
| **高性能** | 只画损坏并剔除遮挡、整数 SIMD 命中测试、O(N)/O(N log N) 放置表 diff、零拷贝 dmabuf、稳态不分配 |
| **低延迟** | 渲染路径不等客户端栅栏和 GPU、帧按自适应时钟尽量晚开始、DRM 实时提交线程合入光标并直传 `IN_FENCE_FD` |
| **轻量** | 空闲零唤醒；系统依赖只有 `sys/*` 与 Vulkan 1.3；特性裁剪（`backend` / `vulkan` / `nested`）；显存按需分块（4 MiB 起倍增，空块释放） |

---

## 2. 项目导航

### 分层结构

```
luminaria (Display/plug/各主题文件)
  ├── luminaria_server (协议状态，无 GPU) ── luminaria_proto (生成) ── luminaria_wire
  ├── luminaria_compose (Vulkan 合成，[vulkan]) ── vk_compose / vk_draw2d / vk_gpu (git)
  └── luminaria_backend (DRM/Nested/Headless + Devices，[backend]) ── sys/* [; luminaria_proto (nested)]
        └── 公共纯数据层: luminaria_types
```

`luminaria/src/display.mica` 只放 `Display` 的状态、事件循环和公开调用；背后的工作按主题分文件，都是
`fn f[O: plug.Output, R: plug.Renderer](mut d: display.Display[O, R], ...)` 形式的函数（Mica 的方法
只能定义在类型所在模块）：`outputs`、`clients`（缓冲、表面、弹出菜单）、`waits`（事务等待）、`place`
（布局落到放置表、动画、遮挡）、`seats`（keymap、光标）、`input`（路由、`accept`）、`manipulation`、
`frames`、`capture`、`shell`。只属于一个主题的状态放在该主题的结构体里（`waits.Waits`、
`manipulation.State`、`capture.Captures`）。

### 改动速查

| 修改目标 | 先看接口（`mica doc`） | 主要改动范围 |
|---|---|---|
| Wayland 协议 / 事件 | `luminaria_server/src/server.mica` | `protocols/*.xml` → `luminaria_scanner` → `luminaria_server/src/{state,route,<模块>,server}.mica` |
| 合成 / dmabuf / overlay / 截屏 / GPU 恢复 | `luminaria_compose/src/lib.mica`、`luminaria/src/plug.mica` | `luminaria_types/src/scene.mica`、`luminaria_compose/src/`、`luminaria/src/{frames,capture}.mica` |
| DRM / KMS / libinput / XKB / 多 seat | `luminaria_backend/src/{lib,devices}.mica`、`luminaria/src/plug.mica` | `luminaria_types/src/device.mica`、`luminaria_backend/src/`、`luminaria/src/{outputs,seats}.mica` |
| 布局 / 命中测试 / 外壳 / 输入路由 | `luminaria/src/display.mica` | `luminaria/src/{place,input,manipulation,shell,hit,layout}.mica` |
| 统计 `Stats` 与门禁 | `luminaria_types/src/stats.mica` | `luminaria_types/src/stats.mica`、`luminaria/tests/probe.mica` |

架构文档：[docs/README.md](docs/README.md)、[api.md](docs/api.md)、[compose.md](docs/compose.md)、
[backend.md](docs/backend.md)、[protocol.md](docs/protocol.md)、[input.md](docs/input.md)。

### 跨层不变量

1. **fd 所有权**：跨接口导入或 IPC 发送（`Conn.put_fd`、dmabuf 导入）时在接收端 `os.dup`；失败分支关掉
   已复制的 fd，调用方保留原 fd；资源销毁与 `drop` 释放全部持有的 fd。服务端长期持有的客户端 fd 先验证
   类型（如沙盒的监听 fd 必须是在监听的 socket）。
2. **客户端数值**：坐标、尺寸、偏移在请求处理入口用 `ids.clamp` / `ids.rect` 夹紧，`u32` 转 `i32` 先截到
   `ids.limit`；时间、大小等按协议报错。新增请求照此处理，Mica 的整数溢出和越界转换会 panic。
3. **缓冲与异步生命周期**：`Server.dmabuf` 只借用 fd；截屏与提交按状态标记 `busy`，异步完成后同时通知
   结果并调用 `srv.release(buf)`。
4. **主循环**：`Display.next()` 在 `finish()` 入队事件或置 `quit` 后先重检队列再 `wait(-1)`，持有未消费
   事件时不能睡眠。

---

## 3. 工程规范

### 先查接口

- 动手前用 `mica doc <file.mica> [Symbol] [--all]` 看签名和文档注释。
- `luminaria_proto/src/*.mica` 由 `protocols/*.xml` 生成，查签名直接看 XML 或局部 `grep`。
- 新逻辑放进对应主题文件；新主题另开文件，不往 `display.mica` 堆实现。

### 格式与卫生

- 改完 `.mica` 后 `mica fmt <file>`，提交前 `mica fmt --check` 改动的文件。
- 新增 `pub` 类型、方法、trait 方法和关键字段写简洁的 `///` 注释。
- 删掉废代码与冗余字段；接口或行为变化时同步 [README.md](README.md)、[README.zh-CN.md](README.zh-CN.md)
  与 [docs/](docs/README.md)。文档和提交信息里不写本机路径。
- [mica.toml](mica.toml) 的外部依赖用远端 git，`sheaf update` 刷新 [mica.lock](mica.lock)，不留 `path`。

### 构建、测试与门禁

- 自底向上：`sheaf test -p luminaria_server` → `-p luminaria_backend` → `-p luminaria_compose` → `-p luminaria`。
- 定稿后：`sheaf test`、`sheaf check --no-default-features -p luminaria`、
  `sheaf check --no-default-features -p luminaria_backend`、`sheaf check -p tiny`。
- 改了协议处理：除默认的 fuzz 测试外，用 debug 构建（溢出会 panic）多跑几个种子，如
  `FUZZ_SEED=3 FUZZ_ROUNDS=2000 sheaf test -p luminaria_server`。
- 涉及 Vulkan 同步 / dmabuf / 截屏：`VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation` 下零校验错误。
- 涉及热路径：`sheaf test --release -p luminaria_compose` 与 `-p luminaria`（`probe.mica` 门禁）；本机桌面
  负载会让数字抖动，取多次运行的最好值比较。

---

## 4. Mica 速查

完整摘要见 `mica prompt`：

- **语法**：没有 `;`、`&`/`*`（类型和一元表达式中无 `&`/`*`）、`::`、`?`、宏、`impl`、`as`、`let mut`；路径用 `.`；`let v = try f()`；
  `guard cond else { return ... }`；`defer`；函数必须显式 `return`；`and` / `or` / `not`；仅无损整型拓宽隐式，缩窄/浮点转换显式；
  指令统一使用前置 `@attr`（`@derive(...)`、`@packed`、`@align(N)`、`@bits(T)`、`@inline` 等）；字节字面量用 `b'a'`（`u8`）；
  不用的 `Result` 写 `discard r`；字符串插值 `"{x}"` 里不能再写字符串字面量。
- **模块**：方法只能定义在类型所在模块；同一 bundle 的模块互相看得到全部条目（含私有字段），别的 bundle
  只看 `pub`；别的 bundle 里结构体字段的默认值必须是常量表达式。
- **所有权与借用**：参数默认只读借用，`mut p: T` 可变借用，`take p: T` 转移；借用视图是 `Slice[T]`、
  `MutSlice[T]`、`StrSlice`；`str`、`List`、`Box` 及含它们的结构体默认移动，读字段或元素要 `.clone()` 或
  `mem.replace(mut self.field, empty)`；一次调用不能同时把 `self` 作 `mut` 传又借出它的字段（先 clone 出来）。
- **集合与匹配**：`Map.entries()` 的字段是 `.key` / `.val`；`match` 必须穷尽，忽略载荷写 `Err =>`，单句跳转/丢弃/赋值可省略大括号；
  视图 `Slice[T]` 没有 `.any`（`List` 有）；闭包捕获外部 `mut` 变量会占住借用，必要时拆成顶层函数。
- **其他**：`sync.mutex(v)`；三引号多行字符串；整数溢出与越界转换在 debug 构建里 panic。
