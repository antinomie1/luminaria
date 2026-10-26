# ADR 0007：渲染器最低要求 Vulkan 1.3

日期：2026-10-26
状态：已实现

## 背景

渲染器最初按 Vulkan 1.1 写：经典的 `VkRenderPass` + `VkFramebuffer`、`vkCmdPipelineBarrier`、
`VkSubmitInfo` 加一个 fence 一次提交，物理设备直接取 `devices.front()`。这几样各自都有代价：

- **render pass 和 framebuffer 是为了“清还是留”这一个比特而存在的。** 局部重画要 `eLoad`，
  整帧重画要 `eClear`，于是每种 target 格式两个 render pass，每个 target 再缓存两个
  framebuffer（`fb_load` / `fb_clear`），每条 pipeline 还得绑定其中一个 render pass 才能建。
  它们不携带任何别的信息。
- **旧式屏障把 stage 和 access 分开写**：stage 在调用上，access 在结构体里，一次调用的所有
  屏障共享同一对 stage。等客户端 acquire fence 的 semaphore 只能写 `eAllCommands`，因为没有
  办法说清“只有采样要等它”。
- **一次提交一个 fence。** 回收 in-flight 提交要挨个查 fence，fence 本身也要走空闲表复用；
  描述符集的安全回收则要靠“最老的存活提交序号”间接推出来。
- **取第一块设备在双显卡机器上是错的。** loader 先枚举谁是偶然的；选错 GPU，显示器的 buffer
  导入不进来，渲染结果也扫描不出去。

## 决定

渲染器以 **Vulkan 1.3** 为硬性下限，并且只用它需要的那几样：

- 实例请求 `VK_API_VERSION_1_3`；loader 低于 1.3、或某块设备的 `apiVersion` 低于 1.3，都给出
  明确的 `Result` 错误（后者被跳过，并在“没有可用设备”的错误里逐个写明原因）。
- 特性通过一条 `vk::StructureChain<PhysicalDeviceFeatures2, Vulkan11Features,
  Vulkan12Features, Vulkan13Features>` 先查询、再只打开用到的三项：`synchronization2`、
  `dynamicRendering`、`timelineSemaphore`。缺任何一项的设备同样被跳过。
- **动态渲染**取代 render pass / framebuffer：清还是留就是 `RenderingAttachmentInfo::loadOp`，
  pipeline 只经 `PipelineRenderingCreateInfo` 声明附件格式。`ScanoutTarget` 与
  `OffscreenTarget` 上的 framebuffer 缓存整个删掉。
- **synchronization2** 取代旧屏障与旧提交：每个 `ImageMemoryBarrier2` 自带 stage + access，
  一趟渲染的所有转换合成一次 `pipelineBarrier2`；`queue.submit2` 用 `SemaphoreSubmitInfo`，
  客户端 acquire fence 只挡 fragment shader，显示器的 out-fence 只挡颜色输出。sync_file 的
  导入导出、`VK_QUEUE_FAMILY_FOREIGN_EXT` 所有权转移都不变。
- **一个 timeline semaphore** 取代每次提交一个 fence：每次提交 signal 下一个值，`reap()` 读一次
  计数器就知道哪些提交完成了，描述符集在计数器越过它最后可能被引用的提交时回收。同步路径
  （上传、读回）就是“提交后等这个值”。fence 对象和它的空闲表一起消失。
- **物理设备按规则挑**：调用方经 `VulkanOptions::drm_device`（`drm_device_id(fd|path)`，
  `DrmBackend::fd()`）告诉渲染器显示走的是哪块 DRM 设备，`VK_EXT_physical_device_drm` 的
  primary / render 节点与之匹配者优先；否则有 dmabuf 扩展的优先，再按独显 > 集显 > 虚拟 > CPU。
  `on_drm_device()` 报告是否真的匹配上了。
- 显式扩展列表只剩 1.3 里**不是**核心的：`VK_KHR_external_memory_fd`、
  `VK_EXT_external_memory_dma_buf`、`VK_EXT_image_drm_format_modifier`，可选的
  `VK_EXT_queue_family_foreign`、`VK_KHR_external_semaphore_fd`。`VK_KHR_image_format_list`
  （1.2 起核心）不再列出。
- **校验层按需开**：设了 `LUMINARIA_VK_VALIDATION=1` 才加载 `VK_LAYER_KHRONOS_validation`，
  并用 `VK_EXT_debug_utils` messenger 把 warning / error 打到 stderr；层没装就打一行提示后照常
  运行。debug 构建也**不**默认开——测试会因此慢一个量级，而且很多机器没装这个层。

## 为什么是 1.3，而不是逐个扩展兼容

这三项特性在 1.3 里都是核心且必须支持的。按扩展（`VK_KHR_dynamic_rendering`、
`VK_KHR_synchronization2`）兼容 1.1/1.2 驱动意味着两套代码路径或一层函数指针转发，而能跑
这个库的目标——有 dmabuf 与 DRM modifier 扩展的 Linux GPU 驱动——早已提供 1.3：Mesa 的
RADV、ANV（Gen9 起）、NVK、Turnip，以及 NVIDIA 专有驱动都是。被排除在外的只有很老的硬件
或很老的 Mesa，而它们在 `SceneRenderer` 下仍然有 CPU 合成可用——降级本来就在运行时。

## 后果

- 没有 Vulkan 1.3 的机器：`VulkanRenderer::create()` 失败并说明原因，GPU 路径不可用，
  `SceneRenderer` 落到 CPU 合成器。这是现有的降级路径，不是新的。
- `submission_count()` 现在数**所有**提交（含上传、读回），因为它们都 signal 同一条 timeline；
  原先只数异步的 `render_to`。
- 客户端给的 dmabuf modifier 在 `vkCreateImage` 之前先用
  `vkGetPhysicalDeviceImageFormatProperties2` 核对是否可导入；隐式 modifier
  （`DRM_FORMAT_MOD_INVALID`）直接拒绝，交给 `Frame` 现有的 CPU 回退。这是校验层第一次跑起来
  就指出的非法用法。
- 测试套件在 `LUMINARIA_VK_VALIDATION=1`（外加同步校验
  `VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true`）下零告警，是这条 ADR 的验收标准之一。
  `test_render_alloc` 例外地关掉校验层：层本身是进程内的 C++，会经被替换的 `operator new`
  分配，计数测的就不再是渲染器了。
- 1.0 之前不承诺 API 稳定（ADR 0004）：`VulkanRenderer::create(const VulkanOptions&)` 是新增，
  无参的 `create()` 照旧可用。
