# Mali kbase 现状与 DRM 需求分析

本文是 "CIX Mali kbase DRM Render Node Integration" 的 PR #1 交付物。
目标是在改动任何生产代码之前，把以下三件事记录清楚：

1. CIX Mali kbase 驱动当前如何创建设备、如何管理内存、如何导入 dma-buf；
2. 当前 Sky1 图形栈上有哪些 DRM 设备，它们分别由谁创建；
3. 通用用户态（Mesa Zink / GBM / Wayland / Xwayland / Snap）实际需要哪些 DRM 能力。

## 1. 调查对象

| 项目 | 值 |
| --- | --- |
| 仓库 | https://github.com/cixtech/cix_opensource__gpu_kernel |
| 分支 | `cix_k6.6.89_2026q2` |
| 基线提交 | `a9c0a471d663369f0329a280a15449d2206d5e94`（CIX P1 2026Q2 RC4 RELEASE） |
| DDK 版本 | `r54p1-11eac0`（见 `drivers/gpu/arm/midgard/Kbuild` 中的 `MALI_RELEASE_NAME`） |
| 目标内核 | Linux 7.3+ 主线（2026-11-16 起；6.6.89 保持可编译） |
| 构建方式 | 外部模块（DKMS，`drivers/gpu/arm/midgard/Makefile` + `KDIR`） |

kbase 在本仓库中是 **out-of-tree 模块**，并不放在内核源码树的
`drivers/gpu/arm/` 下。`Kconfig` 只在把整个目录放进内核树时才会被 Kconfig 解析；
out-of-tree 构建时，所有 `CONFIG_MALI_*` 由 `drivers/gpu/arm/midgard/Makefile`
的默认值加 `MAKE_ARGS` 传给 kbuild。这一点决定了后续新增配置项时必须同时改
`Kconfig` 和顶层 `Makefile`，否则 DKMS 构建永远拿不到新选项。

## 2. 设备模型与生命周期

### 2.1 注册路径

```
platform_driver(kbase_platform_driver)
  .probe   = kbase_platform_device_probe()      mali_kbase_core_linux.c:4565
  .remove  = kbase_platform_device_remove()     mali_kbase_core_linux.c:4527
  .of_match_table = { arm,malit6xx, arm,mali-midgard,
                      arm,mali-bifrost, arm,mali-valhall }
  .acpi_match_table = { CIXH5000 }
```

探针流程：

```
kbase_platform_device_probe()
  kbdev = kbase_device_alloc()                 device/mali_kbase_device.c:91
  kbdev->dev = &pdev->dev
  dev_set_drvdata(kbdev->dev, kbdev)
  kbase_device_init(kbdev)                     device/backend/mali_kbase_device_csf.c:379
```

`kbase_device_init()` 按顺序执行 `dev_init[]` 表中的每一个 `init` 回调；任一步失败时
调用 `kbase_device_term_partial(kbdev, i)` **逆序**回退已完成的步骤，然后 probe 释放
kbdev。这一机制就是 "注册失败不能留下半初始化设备" 的现成实现，DRM 注册必须挂到
这张表里，而不是在 probe 末尾单独调用。

卸载路径：

```
kbase_platform_device_remove()
  kbase_device_term(kbdev)     -> dev_init[] 逆序 term，最后 kbase_mem_halt()
  dev_set_drvdata(kbdev->dev, NULL)
  kbase_device_free(kbdev)     -> kfree(kbdev)
```

**关键约束**：`kbase_device_free()` 是同步 `kfree()`，没有引用计数。
任何在 kbdev 生命周期之外仍可能被访问的对象都不能保存 kbdev 指针，除非自己引入
引用计数。这直接决定了 DRM 设备在 PR #2 中不能持有 `kbdev` 指针（见设计文档）。

### 2.2 /dev/maliX

`dev_init[]` 中的 `kbase_device_misc_register` 用 `misc_register()` 注册
`/dev/maliX`（`kbdev->mdev`，`MISC_DYNAMIC_MINOR`），因此设备号是动态的，
用户态通过 `/dev/mali0` 这类名字由 udev 创建。DRM render node 与它是两个完全独立的
字符设备；本项目的所有 buffer/job 路径仍然走 `/dev/mali0`。

## 3. dev_init[] 表

CSF 后端（Sky1 使用的后端）的初始化表位于
`device/backend/mali_kbase_device_csf.c`，顺序如下（省略条件编译项）：

| # | init | 说明 |
| --- | --- | --- |
| 1 | `kbase_get_irqs` / `registers_map` | IRQ、寄存器窗口 |
| 2 | `power_control_init` | 电源/时钟 |
| 3 | `kbase_io_init`, `kbase_device_io_history_init` | IO 抽象 |
| 4 | `kbase_device_early_init` | GPU 早期探测 |
| 5 | `kbase_backend_time_init` | 时间戳 |
| 6 | `kbase_device_misc_init`, `kbase_device_pcm_dev_init` | 杂项设备数据、PCM |
| 7 | `kbase_ctx_sched_init` | 上下文调度器 |
| 8 | `kbase_mem_init` | 内存子系统 |
| 9 | `kbase_csf_protected_memory_init` | 受保护内存 |
| 10 | `kbase_device_coherency_init` | 一致性 |
| 11 | `kbase_protected_mode_init` | 受保护模式 |
| 12 | `kbase_device_list_init`, `kbase_device_timeline_init` | 设备链表、timeline |
| 13 | `kbase_clk_rate_trace_manager_init` | 时钟跟踪 |
| 14 | hwcnt 系列（watchdog、backend、context） | 性能计数器 |
| 15 | `kbase_csf_early_init`, `kbase_backend_late_init`, `kbase_csf_late_init` | CSF 固件/调度器 |
| 16 | `kbase_debug_csf_fault_init` | fault 调试 |
| 17 | `kbase_device_debugfs_init`, `kbase_csf_fence_timer_debugfs_init` | debugfs |
| 18 | `kbase_sysfs_init` | sysfs group |
| 19 | `kbase_device_misc_register` | **注册 /dev/maliX** |
| 20 | `kbase_gpuprops_populate_user_buffer` | GPU 属性用户态缓冲 |
| 21 | `kbase_device_late_init` | 晚期初始化 |
| 22 | `kbase_debug_coresight_csf_init` | CoreSight（可选） |

DRM render node 应作为 **表的最后一项**：此时 `/dev/maliX`、sysfs、debugfs 都已就绪，
render node 一旦出现在 `/dev/dri` 就可以被 udev/libdrm 安全探测；失败时前面的所有项
都会按既有机制逆序回退。

## 4. 内存子系统与 dma-buf

### 4.1 kbase_mem_from_umm() 的导入流程

`mali_kbase_mem_linux.c:1433`：

```
dma_buf_get(fd)                                  获取 dma_buf 引用
dma_buf_attach(dma_buf, kctx->kbdev->dev)        建立 attachment（绑定 Mali 设备）
kbase_import_size_is_valid()                     大小校验
kbase_ctx_alloc_free_region()                    分配 GPU VA
kbase_alloc_create(..., KBASE_MEM_TYPE_IMPORTED_UMM, ...)
kbase_update_region_flags()
[!MALI_DMA_BUF_MAP_ON_DEMAND]
  dma_buf_map_attachment()                       立即建立 GPU 映射
```

映射由 `kbase_mem_umm_map_attachment()`（同文件 1243 行）完成，得到 `sg_table` 后交给
kbase MMU 建立页表。导入的 buffer 保存在
`reg->gpu_alloc->imported.umm.{dma_buf, dma_attachment, sgt}` 中。

错误路径统一走 `dma_buf_exit`：`dma_buf_detach()` + `dma_buf_put()`。

### 4.2 所有权与释放顺序

`mali_kbase_mem_phy_alloc_put()` → `kbase_mem_phy_alloc_free()` 中
（`mali_kbase_mem.c:1994` 附近）：

```
dma_buf_unmap_attachment[_unlocked](attachment, sgt, DMA_BIDIRECTIONAL)
kbase_csf_scheduler_delegate_imported_buf_alloc_free()   // 可能延迟释放
dma_buf_detach(dma_buf, attachment)
dma_buf_put(dma_buf)
```

即：**unmap → detach → put**，顺序正确，且 GPU 侧释放可以延迟到 CSF 调度器安全点。
这个 "延迟释放" 是 PR #4 设计 GEM wrapper 时必须考虑的点：GEM object 的 `free`
回调不能假设立即完成 dma-buf 引用释放。

### 4.3 kbase 是否已有可复用的 DMA-BUF Exporter

**没有。** 全仓库（`drivers/gpu/arm/midgard`）搜索 `dma_buf_export` / `struct dma_buf_ops`
无任何结果；kbase 只做 importer（`dma_buf_get` + `dma_buf_attach`）。因此：

- PR #4 的 "外部 DMA-BUF 导入" 可以直接复用现有 `kbase_mem_from_umm()` 语义；
- PR #6 的 "导出 kbase 原生内存" 需要从零实现 exporter，不存在可复用代码。

### 4.4 缓存与 IOMMU

- 导入时通过 MGM（memory group manager）的
  `mgm_get_import_memory_cached_access_permitted()` 决定 `BASE_MEM_CACHED_CPU`，
  没有 MGM 查询接口时保守地强制 cached；
- `kbase_mem_umm_map_attachment()` 负责 cache maintenance；
- IOMMU 映射完全由 dma-buf exporter 提供的 `sg_table` 决定，kbase 不自行 IOMMU map。

结论：GEM wrapper 不应重新实现映射，只需把 dma-buf 交给现有 UMM 路径。

## 5. 当前 Sky1 上的 DRM 设备

（源码结论，需在板子上用第 6 节的脚本确认实际值）

当前 CIX 6.6 图形栈 **没有** Mali GPU 的 render node。`/dev/dri/renderD*` 由
**Display Controller（linlondp / cix-dpu）** 提供：

`cix-dpu-1.0.0/linlon-dp/linlondp_kms.c`：

```c
static bool enable_render = true;
module_param(enable_render, bool, 0644);

static const struct drm_driver linlondp_kms_driver = {
    .driver_features = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
    ...
};

if (enable_render)
    linlondp_kms_driver.driver_features |= DRIVER_RENDER;
```

即：DPU 的 KMS 驱动被人为加上 `DRIVER_RENDER`，于是同一个 `drm_device` 同时导出
`/dev/dri/card0` 和 `/dev/dri/renderD128`。这就是问题描述中 "Display Controller 提供的
Render Node 兼容方案" 的来源。

它的后果：

- `renderD128` 的 DRM driver name 是 DPU（`linlondp`），不是 Mali；
- 通过它做 PRIME 导入/导出实际落在 DPU 的 GEM 上，与 Mali 的 GPU VA/MMU 无关；
- Mesa Zink 只要求 "存在一个可用的 render node"，因此可以用它跑起来，但这并不代表
  Mali GPU 可以被标准图形栈发现。

## 6. 硬件取证脚本

见 `tools/drm/01-current-behavior.sh`（脚本 + 结果模板）。在 Sky1 板子上执行：

```bash
sudo tools/drm/01-current-behavior.sh /tmp/drm-baseline
```

脚本会采集：

- `/dev/dri/`、`/dev/mali*`、`/sys/class/drm/*`、每个节点的 `device` 软链接；
- `drm-info`（本仓库工具，直接发 DRM ioctl）输出的 driver name / major/minor /
  parent / DRM_CAP_PRIME；
- `lsmod` 中 mali/panthor/drm 模块；
- 在 Vulkan 使用 Mali ICD、Mesa 使用 Zink 的配置下 `glmark2-wayland` 的
  `strace -ff -yy -e trace=openat,openat2,ioctl,close,mmap` 记录；
- 对 trace 做 ioctl 归类统计，区分 **设备发现**、**buffer 导入导出**、**GPU 提交**。

### 待填写的实测结果

> 当前执行环境为 x86_64 构建机（无 Sky1 板卡、无 `/dev/mali*`），以下项目需由测试
> 同事在板子上补全；脚本已把输出格式固定，直接粘贴即可。

| 项目 | 期望（源码推导） | 实测 |
| --- | --- | --- |
| Mali render node 是否存在 | 否 | 待填 |
| `/dev/dri/renderD128` 的 driver name | `linlondp`（DPU） | 待填 |
| `/dev/dri/renderD128` 的 device parent | DPU platform device | 待填 |
| `/dev/dri/card0` 的 driver name | `linlondp` | 待填 |
| `/dev/mali0` | 由 `kbase_device_misc_register` 创建 | 待填 |
| glmark2(Zink) 打开的 DRM 节点 | `renderD128`（DPU） | 待填 |
| 出现 `PRIME_FD_TO_HANDLE` 的节点 | DPU render node | 待填 |
| 出现 `PRIME_HANDLE_TO_FD` 的节点 | DPU render node | 待填 |
| 是否出现 GPU 提交 ioctl | 否（提交走 `/dev/mali0`） | 待填 |

## 7. 通用用户态真正需要的 DRM 能力

根据 Mesa `pipe_loader_drm.c`、GBM 与 Vulkan WSI 的实现，一个 render node 至少需要：

| 能力 | ioctl | 本阶段是否需要 |
| --- | --- | --- |
| 设备发现与版本 | `DRM_IOCTL_VERSION`, `DRM_IOCTL_GET_CAP` | 是（PR #2） |
| 能力查询 | `DRM_IOCTL_GET_CAP(DRM_CAP_PRIME/SYNCOBJ/...)` | 是（PR #2，仅透传） |
| 导入外部 dma-buf | `DRM_IOCTL_PRIME_FD_TO_HANDLE` + `DRM_IOCTL_GEM_CLOSE` | 是（PR #4） |
| 导出 buffer | `DRM_IOCTL_PRIME_HANDLE_TO_FD` | 暂不要求（PR #6） |
| GEM 创建/mmap | `DRM_IOCTL_MODE_CREATE_DUMB`, `mmap` | 暂不要求（PR #6） |
| KMS | `DRM_IOCTL_MODE_*` | 否，明确不做 |
| GPU 提交 | 无标准 DRM ioctl，继续走 `/dev/mali0` | 否 |

**结论**：PR #2 只需要 `DRIVER_RENDER` 与 DRM core 的标准 file operations，
不实现 `DRIVER_GEM` 是安全的——没有 GEM 时 DRM core 会让 PRIME/GEM ioctl 返回
`-EOPNOTSUPP`，不会出现 "假装成功" 的空实现。

需要特别说明的一点：从 Linux 6.6 开始，`DRM_IOCTL_GET_CAP(DRM_CAP_PRIME)` 在
`drm_ioctl.c` 中无条件返回 `DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT`
（不检查驱动是否真的实现了 GEM）。因此 **不能** 用 CAP 查询判断 PRIME 是否可用，
验收时必须用真实 ioctl 的返回值判断。

## 8. 对后续 PR 的输入

1. DRM 注册点：`dev_init[]` 末尾（第 3 节）。
2. 生命周期：kbdev 会被同步 `kfree()`，DRM 设备不得反向持有 kbdev（PR #2 设计）。
3. 内存：PR #4 的 GEM object 必须是一个独立 wrapper，`free` 时把 dma-buf 交回
   `kbase_mem_from_umm()` 的现有 UMM 路径；不能把 `kbase_mem_phy_alloc` 当作 GEM 对象。
4. dma-buf exporter 不存在，PR #6 的 C 方案是从零开发。
5. 构建：所有新选项必须同时写进 `Kconfig` 和 out-of-tree 的 `Makefile`/`Kbuild`。
