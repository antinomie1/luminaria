# TODO

未完成的工作与技术边界。架构设计见 [docs/architecture.md](docs/architecture.md) 与 [docs/adr/](docs/adr/)；已完成功能见 [docs/features.md](docs/features.md)。

---

## 待落地任务（按执行优先级）

### 0. 真机验证 DRM 剩余路径

已在一台 1920×1080@60Hz 显示器上验证 atomic 模式设置、翻页、fence 链、libinput、硬件光标平面及 VT 切换。仍需真机覆盖：

- 多显示器 CRTC / primary / cursor plane 分配与跨屏指针。
- udev connector 热插拔与 per-output 状态回收。
- `Output::set_mode()` 后的整链重建（scanout target、layout、`wl_output`）。
- 客户端 dmabuf 直接扫描输出（direct scanout）。
- 数位板与 VRR（`VRR_ENABLED`）在真实硬件下的翻页与输入行为。

验证方法：空闲 VT 上运行 `minimal`（`LUMINARIA_BACKEND=drm`），配合 `scripts/tty-check.sh` 采集证据日志。

### 1. 性能指标进 CI 门禁

设立三个自动化指标门禁，防止低功耗与低占用特性退化：

- **空闲态每秒 atomic 提交次数**：目标稳定为 **0**（无客户端 damage 时零提交、零翻页）。
- **稳态 RSS**（单客户端运行下）。
- **每帧 GPU 提交耗时 p99**。

### 2. 视觉特效与渲染进阶

- **动态下层采样背景模糊**：目前已实现基于静态底图缓存的 x-ray 模糊（`place_xray_blur`）；仍需支持实时采样下层动态窗口内容的真实双重模糊（需处理下层 damage 传播与离屏累加）。
- **HDR 与色彩空间转换**：目前已完成 `wp-color-management-v1` 协议协商；渲染器内部仍按客户端编码直出，需在片元阶段补充线性化、原色矩阵转换、色调映射与 HDR 元数据传递。

---

## 源码待优化标记

| 位置 | 内容 |
|---|---|
| `region.cppm:14` | `Region` 操作为 O(n)、合并为 O(n²)。矩形数量成为瓶颈时替换为 `pixman_region32`。 |
| `tearing_control.cppm:96` | 重复创建的 tearing_control 对象暂未下发协议错误。 |

---

## 已知边界与技术限制

- **色彩管理**：不支持 ICC profile；`wp-color-representation` 暂不支持 YUV 直读。
- **复合提交唤醒**：同一个表面同时挂载 `commit-timing` 与 `fifo` 时，队头被 FIFO 放行后需等待下次时间戳或定时器 tick 才能触发后续判定。
- **不可见表面 FIFO 闸门**：最小化或完全遮挡的表面需由混成器调用 `FifoManager::unblock_hidden()` 主动放行，否则会停在提交闸门中。
- **输入法键盘 Grab 路由**：`InputMethod::keyboard_grab()` 激活时，按键需由混成器策略决定是否先拦截送入输入法。
- **Token 校验**：`xdg_activation` 未在库内校验 serial，防焦点窃取由混成器根据上下文核对。
- **全局过滤槽限制**：`data_control` 依赖 `wl_display` 的单一全局过滤器，混成器若有自定 filter 需合并处理。
- **嵌套修饰键状态**：嵌套后端下无独立于 Shift/Ctrl 的修饰键状态（以宿主报告为准）。
- **绝对指针定位**：`LibinputBackend` 尚未将平板与虚拟机的绝对指针事件归一化映射到具体输出。
- **Headless Present 事件**：无头后端当前在每帧前触发一次 `present`，无法单独测试 `unchanged` 漏发帧回调的情况。

---

## 明确不做

- **窗口摆放策略**：平铺、浮动、分屏、工作区切换逻辑均属混成器策略，明确不属于机制库。
- **服务端窗口装饰绘制**：库不绘制标题栏与阴影边框，`xdg-decoration` 默认答复 client-side。
