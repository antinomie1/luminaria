# ADR 0006：自定义片元着色器必须声明损伤契约

日期：2026-08-16
状态：已实现

## 背景

离屏合成（ADR 0005）解决了“把一扇窗当成一个整体”这个问题，但圆角、色调、噪点和类似的
桌面效果仍然不应都变成库内置的特例。混成器需要一个小而明确的 hook，把它自己的 SPIR-V
片元着色器套到一个 compositor-owned texture 上。

这个 hook 不能只有“给我一个 shader”。`Frame` 会按 damage 只重画一小块，空 damage 时还会
停掉输出帧；而 shader 可能依赖时间、随机数、全屏坐标或外部 uniform。若库把这种 shader 当作
普通纹理，结果就是一块冻结的动画或一块没有刷新的旧像素。让每个混成器再手写一套 wake-up 和
full repaint，又违背了 `Frame` 代管帧账本的目的。

## 决定

`VulkanRenderer::create_fragment_shader()` 接受编译好的 SPIR-V，并创建只能由该 renderer 使用的
`FragmentShader`。它只替换纹理 quad 的 fragment stage：descriptor set 0 / binding 0 仍是
`sampler2D`，push constant 仍是 `quad.vert` 的 `Push`，输入 UV 在 location 0，输出的预乘 RGBA
在 location 0。顶点阶段、混合方式、坐标变换和显式同步都不变。

shader 创建时必须选一个 `ShaderDamage`：

- `none`：shader 是静态的，且只影响它自己的 destination quad；普通 placement diff 已足够。
- `full`：每一帧都必须重画整个输出，但由外部事件决定何时调用 `Frame::invalidate()`。
- `continuous`：每一帧都必须重画整个输出，并在成功提交后继续请求下一帧；用于时间驱动动画。

`Frame::place(texture, transform, shader)` 和 `compose_group(..., shader)` 把这个契约放进同一份
placement ledger。`full`/`continuous` 使整帧失效，`continuous` 复用 `Frame::animate()` 的持续帧
请求；任何 shader placement 都禁止 direct scanout。shader identity 也进入 placement diff，替换
shader 即使几何未变也会损伤旧/新 quad。

管线缓存归 renderer，而不是 `FragmentShader`。一次异步 submit 仍可能引用它，即使调用方已经
销毁 shader；让 renderer 持有管线直到自己销毁，避免提前销毁 Vulkan pipeline。代价是每个不同
shader/输出格式在 renderer 生命周期内留下一个小的 pipeline cache entry，这是正确性优先的有界
资源策略。

## 边界

这不是通用 scene shader，也不允许它采样“已经画好的下层窗口”。它只有自己的源纹理；需要整窗
效果时先 `compose_group()`，再对最终纹理套 shader。背景模糊仍走 x-ray cache；真正的 lower-window
backdrop sampling 需要显式的 scene snapshot / layer 顺序语义，不能用这个 API 偷渡。

SPIR-V 的接口错配由 Vulkan 在创建 pipeline 时拒绝，并作为 `Status` 返回。库不加载运行时 GLSL，
也不替调用方验证 shader 的视觉或时间依赖；damage 枚举就是调用方对此作出的可审计声明。
