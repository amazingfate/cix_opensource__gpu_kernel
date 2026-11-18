# PR #5：上游 Mesa 集成

## 1. 设备与库加载现状

debian-o6 使用发行版 Mesa 26.1.2 和 Khronos Vulkan Loader 1.4.309。
CIX 专有 UMD 位于 `/opt/cixgpu-pro/lib/aarch64-linux-gnu/libmali.so`，
提供 `vk_icdGetInstanceProcAddr`、`vk_icdGetPhysicalDeviceProcAddr` 和
`vk_icdNegotiateLoaderICDInterfaceVersion`，可以通过标准 loader 加载。
显式提供 ICD JSON 后，`vulkaninfo` 能枚举出 `Mali-G720-Immortalis`。

但这并不意味着 Zink 可以直接使用它。Zink 要把 EGL 的 DRM fd 与 Vulkan
physical device 对应起来，需要查询 `VkPhysicalDeviceDrmPropertiesEXT` 中的
`renderMajor/renderMinor`。CIX Mali UMD 的实例扩展列表没有
`VK_EXT_physical_device_drm`，因此匹配失败：

```text
MESA: error: ZINK: failed to choose pdev
```

KMD 的 render node 本身工作正常；问题发生在用户态的 DRM/Vulkan 设备关联阶段。

## 2. Vulkan ICD wrapper 方案

GPU 组确认不能修改 UMD 源码后，采用一个独立的 ICD wrapper：

```text
Mesa Zink / Vulkan 应用
    ↓
Khronos Vulkan Loader
    ↓ cix_mali_wrapper_icd.json
libvulkan_cix_mali_wrapper.so
    ├─ 转发 vkCreateInstance / vkEnumeratePhysicalDevices / device entry points
    ├─ 增加 VK_EXT_physical_device_drm
    └─ 在 VkPhysicalDeviceProperties2.pNext 中补充 DRM major/minor
    ↓
libmali.so（未修改，继续通过 /dev/mali0 提交）
```

实现位于 `tools/vulkan/cix_mali_vulkan_wrapper.c`：

- 通过 `dlopen`/`dlsym` 加载真实 UMD；
- 转发 loader/ICD 接口；
- 创建实例时从传给真实 UMD 的扩展列表中移除
  `VK_EXT_physical_device_drm`（该扩展由 wrapper 提供，而不是 UMD）；
- 拦截 `vkGetPhysicalDeviceProperties2` 和 KHR 别名，在原有 pNext 链上填写
  `VkPhysicalDeviceDrmPropertiesEXT`；
- 扫描 `/dev/dri/renderD*` 并用 `DRM_IOCTL_VERSION` 查找 `mali-kbase`，
  从设备节点的 `st_rdev` 动态取得 major/minor，不写死 `renderD128`；
- 记录 `vkEnumeratePhysicalDevices` 返回的 physical-device → instance 映射，
  保证属性查询转发到正确的 UMD 实例；
- 不修改 GPU 提交、内存管理或 WSI 调用。

## 3. 当前验证结果

在 debian-o6 上：

```text
[cix-mali-drm] loaded the Mali UMD
[cix-mali-drm] render node major=226 minor=128
[cix-mali-drm] vkCreateInstance forwarded
```

`eglinfo -B` 中，之前报 `failed to choose pdev` 的设备平台现在能初始化并报告：

```text
OpenGL core profile renderer: zink Vulkan 1.4(Mali-G720-Immortalis (ARM_PROPRIETARY))
OpenGL compatibility profile renderer: zink Vulkan 1.4(Mali-G720-Immortalis (ARM_PROPRIETARY))
OpenGL ES profile renderer: zink Vulkan 1.4(Mali-G720-Immortalis (ARM_PROPRIETARY))
```

这证明 wrapper 已经解决当前已确认的 **DRM 节点 ↔ Vulkan physical device 匹配问题**，
并且 Mesa 可以继续执行到 Zink/EGL 初始化。

## 4. 尚未完成

- Wayland/EGL 的 buffer 导入导出、format modifier 和显示路径；
- glmark2 的 Wayland 测试目前仍选择 llvmpipe，不能把 `eglinfo` 的成功等同于完整
  Wayland 渲染已经成功；
- X11/GLX、EGL surfaceless 的端到端渲染；
- Zink 对 Mali UMD 的 feature/extension 要求是否全部满足；
- 多 GPU、多实例和 loader layer 组合下的行为；
- wrapper 的安装位置、ICD JSON 打包和与 CIX 专有 GL/Vulkan 路径共存策略。

wrapper 目前是一个用户态诊断/集成组件，尚未替代发行版 Mesa，也未修改上游 Mesa。
