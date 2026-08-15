# 表面一律经代际句柄访问

长寿命结构不再持有 `Surface*`。取而代之的是 `SurfaceId{index, generation}`，解引用要查表，
表能告诉你这个表面已经没了。焦点、光标、drag focus、外壳层的每帧列表全部改用它。

## 为什么

现有的防线是**约定**：任何缓存 `Surface*` 的地方都必须订阅 `Surface::destroy` 并在回调里清空。
这条规矩写在 CLAUDE.md 里，配了 RAII 的 `Signal::Connection`，也确实有效——但它是一条**要记得
遵守**的规矩，而"忘了订阅"这个错误类别永远存在。事实是已经忘过：`data_device` 的 drag focus
和（当时的）`scene` 的 `SceneSurface` 各犯过一次，各配了一个回归测试。测试只覆盖犯过的
那一处。

内存安全若要作为本库相对 wlroots 的卖点，就不能是"我们很小心"，得是"这个错误写不出来"。
代际句柄把"崩溃"降级成"查表返回 null"——忘了订阅不再是 use-after-free，只是一次没画出来。

## 只做 Surface

- `Output` 的销毁由混成器自己驱动（udev → `Output::destroy`），它需要的是**重建**按旧模式算过
  的一切，句柄给不了这个；空指针保护解决不了"scanout target 尺寸不对"。
- `wl_buffer` 已有 `BufferWatch`——贴着 libwayland 自己的 destroy listener 语义，比套一层句柄
  更直白。

一个类型换掉一整族崩溃，其余两个用现有机制加测试。

## 后果

跨 dispatch 留存表面身份时要先查表。API 的直白程度有损失，这是明知的代价；当前 dispatch
里拿到的 `Surface&` 仍可直接使用，不需要为了形式把每一次局部访问都绕成句柄。

## 落地

- 每个 `Surface` 创建时登记 `SurfaceId{index, generation}`；销毁先清槽再递增 generation，
  `surface_from_id()` 同时核对两项，旧 id 不会误认复用同一槽的新表面。
- seat 的键盘/指针/触摸/光标焦点、data-device 的 drag focus / drag icon、`Frame` 的摆位、
  命中结果、纹理缓存和直出 hold 全部只留 id。`SurfaceInvalidated` 只负责行为清理，内存安全不
  依赖监听者有没有订阅。
- `test_surface_handle` 覆盖销毁、槽复用和 ABA；`test_frame` 把旧摆位留过客户端销毁后再次
  命中与 submit；`test_seat_input` 验证焦点身份自动失效。
- 配套的 `test_protocol_fuzz` 在每次普通 `xmake test` 中跑 48 条固定种子流：随机交错
  surface / region / buffer 生命周期，并注入非法 scale、transform、重复 xdg role、自父
  subsurface 与短 stride。`f103082` 那一类洞由常态测试守住，不再只靠人审。
