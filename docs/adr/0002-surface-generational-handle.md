# 表面一律经代际句柄访问

长寿命结构不再持有 `Surface*`。取而代之的是 `SurfaceId{index, generation}`，解引用要查表，
表能告诉你这个表面已经没了。焦点、光标、drag focus、外壳层的每帧列表全部改用它。

## 为什么

现有的防线是**约定**：任何缓存 `Surface*` 的地方都必须订阅 `Surface::destroy` 并在回调里清空。
这条规矩写在 CLAUDE.md 里，配了 RAII 的 `Signal::Connection`，也确实有效——但它是一条**要记得
遵守**的规矩，而"忘了订阅"这个错误类别永远存在。事实是已经忘过：`data_device` 的 drag focus
和 `scene` 的 `SceneSurface` 各犯过一次，各配了一个回归测试。测试只覆盖犯过的那一处。

内存安全若要作为本库相对 wlroots 的卖点，就不能是"我们很小心"，得是"这个错误写不出来"。
代际句柄把"崩溃"降级成"查表返回 null"——忘了订阅不再是 use-after-free，只是一次没画出来。

## 只做 Surface

- `Output` 的销毁由混成器自己驱动（udev → `Output::destroy`），它需要的是**重建**按旧模式算过
  的一切，句柄给不了这个；空指针保护解决不了"scanout target 尺寸不对"。
- `wl_buffer` 已有 `BufferWatch`——贴着 libwayland 自己的 destroy listener 语义，比套一层句柄
  更直白。

一个类型换掉一整族崩溃，其余两个用现有机制加测试。

## 后果

`surface.commit()` 这类写法变成先查表。API 的直白程度有损失，这是明知的代价。

配套（同一个目标的另一半）：畸形协议流的**模糊测试客户端**常态化跑。
`f103082`（客户端声明的 buffer layout 越界）那一类洞靠人审是漏的。
