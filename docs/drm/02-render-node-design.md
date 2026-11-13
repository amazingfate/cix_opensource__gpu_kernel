# PR #2 设计：注册 Mali GPU DRM Render Node

本文描述 `CONFIG_MALI_KBASE_DRM_RENDER` 的设计，对应 PR #2 "注册 GPU DRM Render Node"。
本阶段只建立设备模型，**不**提供 GEM/PRIME、KMS 或任何 GPU 提交接口。

## 1. 目标与非目标

目标：

- Mali GPU 以自己的 platform device 为父设备，向 DRM core 注册一个标准 render node；
- `/dev/mali0` 及其 UAPI 完全不受影响；
- libdrm 能查询到真实的 driver name、版本、父设备；
- 关闭配置选项时，KMD 行为与今天完全一致。

非目标：

- 不实现 GEM object、PRIME 导入/导出（PR #4）、buffer 分配（PR #6）；
- 不添加 KMS；
- 不把 `/dev/mali0` 的 ioctl 迁到 DRM；
- 不声明 render node 已经可以用于 buffer 共享。

## 2. 设备模型

```
platform_device (ACPI CIXH5000 / DT arm,mali-*)
  │
  ├── /dev/mali0            (misc device, kbase UAPI，保持不变)
  │
  └── drm_device            (本 PR 新增，DRIVER_RENDER)
        └── /dev/dri/renderD<N>
              /sys/class/drm/renderD<N>/
                └── device -> ../../../<platform device>
```

- `drm_dev_alloc(&kbase_drm_driver, kbdev->dev)` 用 Mali platform device 作为 parent，
  DRM core 内部对该 parent 取引用（`get_device()`），因此 DRM 设备的生命周期
  可以独立于 kbdev。
- `driver_features = DRIVER_RENDER`：不设置 `DRIVER_MODESET`、`DRIVER_GEM`、
  `DRIVER_ATOMIC`。
- `fops` 只转发 DRM core 的标准操作：`drm_open`/`drm_release`/`drm_ioctl`/
  `drm_compat_ioctl`/`drm_poll`/`drm_read`。**不提供 `mmap`**，因为本阶段没有
  可以被 mmap 的对象。

### 2.1 为什么不用 devm_drm_dev_alloc()

`devm_drm_dev_alloc()` 把 `struct drm_device` 分配在 parent device 的 devres 上。
platform device 被 unbind 时 devres 会释放这块内存，而 **已打开的 DRM fd 仍然持有
drm_device 引用**；`drm_dev_put()` 只是让 devres 的 put 提前执行，内存仍会在
device detach 时被 devres 释放，形成 use-after-free。

`drm_dev_alloc()` 用 `kzalloc` + `drmm_add_final_kfree(dev, dev)`，内存只在最后一个
引用（driver + 每个打开的 fd）释放后才 `kfree`，这正是 PR #2 要求 12
（"正确处理已有文件描述符与设备注销的生命周期关系"）所需要的语义。

注释：`drm_dev_alloc()` 在 6.6 的文档中标记为 "deprecated version of
devm_drm_dev_alloc()"。这里的取舍是有意的：deprecated 只影响文档，不影响 API 可用性；
换成 devm 版本反而无法满足 fd 生命周期要求。PR #3 会评估嵌入 `struct drm_device`
到自带引用计数的容器中的方案。

### 2.2 为什么 DRM 设备不持有 kbdev

`kbase_platform_device_remove()` 最后调用 `kbase_device_free()`，这是同步
`kfree(kbdev)`，没有任何引用计数。若 DRM 设备保存 `kbdev` 指针，则 unbind 之后
仍持有 fd 的进程一旦触发 DRM ioctl 或 `drm_release()`，就会访问已释放内存。

因此本阶段的 `struct drm_device` **不引用 kbdev**：

- 不设置 `drm->dev_private`；
- 所有 DRM 回调都不需要 kbase 状态；
- render node 只暴露 DRM core 的通用 ioctl。

PR #4 引入 GEM 后，GEM object 需要 kbdev，届时必须在 kbase 侧为 kbdev 增加
引用计数（或把 DRM 私有结构做成独立、自持有 kbase 引用的对象），并在 PR #3 的
生命周期文档中给出完整引用图。

## 3. 初始化与回滚

### 3.1 注册点

`dev_init[]`（`device/backend/mali_kbase_device_csf.c`）的最后一项：

```c
#if IS_ENABLED(CONFIG_MALI_KBASE_DRM_RENDER)
    { kbase_drm_render_init, kbase_drm_render_term,
      "DRM render node registration failed" },
#endif
```

放在最后的原因：

- 此时 `/dev/mali0`、sysfs、debugfs 已就绪，render node 一出现就是可探测的完整设备；
- `kbase_device_init()` 已有的失败回退逻辑会逆序调用所有已完成项的 `term`，
  DRM 注册失败不会留下半初始化设备；
- 卸载时 `kbase_device_term()` 逆序执行，DRM 设备 **最先** 被摘除，之后才拆内存、
  IRQ 等 kbase 资源。

### 3.2 `kbase_drm_render_init()`

```
if (kbdev->drm) return -EEXIST;              // 防御重复注册
drm = drm_dev_alloc(&kbase_drm_driver, kbdev->dev);
if (IS_ERR(drm)) return PTR_ERR(drm);
err = drm_dev_register(drm, 0);
if (err) { drm_dev_put(drm); return err; }   // 不留半初始化对象
kbdev->drm = drm;
return 0;
```

`drm_dev_alloc()` 内部完成 `drm_minor_alloc()`（render + primary 槽位）；
`drm_dev_register()` 才真正 `device_add()` 出 `/dev/dri/renderD<N>`。

### 3.3 `kbase_drm_render_term()`

```
drm = kbdev->drm;  if (!drm) return;
kbdev->drm = NULL;
drm_dev_unplug(drm);
drm_dev_put(drm);
```

选择 `drm_dev_unplug()` 而不是 `drm_dev_unregister()`：
`drm_dev_unplug()` 除了摘除 sysfs/字符设备，还把 `dev->unplugged` 置位，此后进入
`drm_ioctl()` 的路径会得到 `-ENODEV`（`drm_dev_enter()` 检查）。因此：

- 新打开被阻止（节点已删除）；
- 已打开的 fd 在 close 之前仍然可以安全地 `close()`/`poll()`，
  但不会再有 ioctl 进入驱动；
- `drm_dev_put()` 只减引用，`struct drm_device` 在最后一个 fd 关闭后才释放。

## 4. 失败矩阵

| 失败点 | 行为 |
| --- | --- |
| `drm_dev_alloc()` 失败（ENOMEM 等） | `dev_init[]` 记录错误，逆序回退已经完成的项，probe 返回错误，不产生 `/dev/dri` 节点 |
| `drm_dev_register()` 失败 | 立即 `drm_dev_put()`，同样由 `dev_init[]` 回退，`kbdev->drm` 保持 NULL |
| 注册成功后 kbase 后续步骤失败 | 本项是 `dev_init[]` 最后一项，不存在 "后续步骤"；若将来在其后追加，逆序 term 会先摘 DRM |
| 设备已注册且已有 fd 时 unbind | `kbase_drm_render_term()` 先 unplug，fd 继续存活到 close，kbdev 可以安全释放 |
| 模块卸载时仍有 fd 打开 | VFS 对 `fops->owner` 持有模块引用，模块卸载被阻止，直到最后一个 fd 关闭 |

## 5. 配置与构建

### 5.1 in-tree（Kconfig）

```kconfig
config MALI_KBASE_DRM_RENDER
	bool "Register a DRM render node for the Mali GPU"
	depends on MALI_MIDGARD && DRM
	default n
```

选择 `depends on DRM` 而不是 `select DRM`：避免为一个可选功能强制打开整个 DRM
子系统，也避免 `select` 造成的依赖次序问题。

### 5.2 out-of-tree（DKMS，本项目实际使用的方式）

新增选项必须同时出现在三处，否则 DKMS 构建拿不到它：

1. `drivers/gpu/arm/midgard/Makefile`：`CONFIG_MALI_KBASE_DRM_RENDER ?= n` 与 `CONFIGS` 列表；
2. `drivers/gpu/arm/midgard/Kbuild`：`mali_kbase-$(CONFIG_MALI_KBASE_DRM_RENDER) += drm/mali_kbase_drm.o`；
3. `drivers/gpu/arm/midgard/Kbuild`：`CONFIG_DRM` 依赖检查；

```make
ifeq ($(CONFIG_MALI_KBASE_DRM_RENDER),y)
    ifeq ($(filter y m,$(CONFIG_DRM)),)
        $(error CONFIG_MALI_KBASE_DRM_RENDER depends on CONFIG_DRM ...)
    endif
endif
```

当 `CONFIG_MALI_MIDGARD != m` 时强制 `CONFIG_MALI_KBASE_DRM_RENDER = n`，与表中其它
选项的做法一致。

模块依赖由 modpost 自动记录：`mali_kbase.ko` 会依赖 `drm.ko`（`DRM=m`）或内核内建的
DRM core（`DRM=y`）。`MODULE_SOFTDEP("pre: memory_group_manager ...")` 不变。

### 5.3 验证矩阵

| `CONFIG_MALI_MIDGARD` | `CONFIG_DRM` | `CONFIG_MALI_KBASE_DRM_RENDER` | 预期 |
| --- | --- | --- | --- |
| m | y/m | n | 与今天完全一致，无 `/dev/dri` 新节点 |
| m | y/m | y | 新增 render node，`/dev/mali0` 正常 |
| m | n | y | Kbuild 在编译期直接报错 |
| n | - | - | 不构建 mali_kbase.ko |

## 6. 验证方法

### 6.1 编译

```bash
make -C drivers/gpu/arm/midgard KDIR=/path/to/linux-6.6.89 \
     ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
     CONFIG_MALI_PLATFORM_NAME=sky1 CONFIG_MALI_CSF_SUPPORT=y \
     CONFIG_MALI_KBASE_DRM_RENDER=y
```

要求同时通过 `W=1` 与 `checkpatch.pl`，并分别验证 `CONFIG_MALI_KBASE_DRM_RENDER=n/y`。

### 6.2 功能

```bash
# 1. 节点存在且父设备是 Mali platform device
ls -l /sys/class/drm/renderD*/device
# 2. driver name 是 mali-kbase 而不是 linlondp
tools/drm/drm-info /dev/dri/renderD128
# 3. /dev/mali0 不受影响
multica ... # 实际用 Mali UMD 的测试程序
# 4. 不支持的能力必须诚实报错
#    PRIME_FD_TO_HANDLE 在 PR #2 阶段返回 -EOPNOTSUPP
# 5. 关闭配置后对比 /sys/class/drm 无新增节点
```

`tools/drm/drm-info` 直接发送 DRM ioctl，不依赖 libdrm，可在板子上直接编译运行。

### 6.3 生命周期

```bash
# 持有 fd 卸载模块
exec 9</dev/dri/renderD128
rmmod mali_kbase            # 预期：EBUSY（fd 持有模块引用）
exec 9<&-
rmmod mali_kbase            # 预期：成功，dmesg 无 warn
```

```bash
# unbind 平台设备（不卸载模块）
echo <platform-device> > /sys/bus/platform/drivers/mali/drivers/.../unbind
# 预期：renderD 节点消失；已有 fd 的 ioctl 返回 ENODEV；close 后无泄漏
```

## 7. 已知副作用与注意事项

1. **DRM core 会同时创建 `/dev/dri/card<N>`。**
   6.6 的 `drm_dev_init()` 对非 ACCEL 驱动无条件分配 primary minor，
   `drm_dev_register()` 也会注册它。即使是 `DRIVER_RENDER`-only 的驱动（例如
   vgem），也会出现 `card<N>`。该节点没有 KMS 对象，`DRM_IOCTL_MODE_*` 由 DRM core
   返回错误。这是 DRM core 的行为，不是本驱动伪造的能力。
2. **`DRM_CAP_PRIME` 从 6.6 起无条件返回 IMPORT|EXPORT。**
   `drm_ioctl.c` 不检查驱动是否实现 GEM。验收必须用真实 `PRIME_FD_TO_HANDLE`
   的返回值判断，不能看 CAP。
3. **`drm_dev_unplug()` 的语义依赖 `drm_dev_enter()`。**
   本阶段 fops 全部是 DRM core 的通用实现，已经在 `drm_ioctl()` 内部使用了
   enter/exit；PR #4 增加驱动自己的 ioctl/mmap 回调时，必须同样包 `drm_dev_enter()`。
4. **节点编号不固定。** 全部由 DRM core 动态分配，驱动不引用 `renderD128`。

## 8. 与后续 PR 的接口

- PR #3：为 kbdev 引入引用计数或改造成 "DRM 私有对象自持有 kbase 引用"，给出完整引用图；
  补 suspend/resume、GPU reset、多进程并发打开等测试。
- PR #4：新增 `drm/mali_kbase_drm_gem.c` / `mali_kbase_drm_prime.c`，设置 `DRIVER_GEM`，
  `gem_prime_import` 走 `drm_gem_prime_import()`，其底层仍然把 dma-buf 交给
  `kbase_mem_from_umm()` 的 UMM 路径。
- PR #5：DPU 的 `enable_render` 模块参数可以关闭，验证 Mesa Zink 在没有 DPU render node
  时能否发现 Mali 的 render node。

## 9. 构建验证结果（2026-11-13）

验证环境：

- 内核头文件：vanilla Linux 6.6.89（arm64），`aarch64-linux-gnu-gcc`；
- 驱动：`cix_k6.6.89_2026q2` / `a9c0a47`；
- 参数：`CONFIG_MALI_PLATFORM_NAME=sky1`、`CONFIG_MALI_CSF_SUPPORT=y`、`ARCH=arm64`；
- 由于 vanilla 内核没有 `CONFIG_ARCH_CIX`，而 CIX 的 `platform/sky1` 代码依赖它，
  构建时用 `KCFLAGS=-DCONFIG_ARCH_CIX=1` 打开对应分支。**这不是目标内核的等价构建**，
  正式 CI 仍需在 `cix_opensource__linux` 的 `cix_k6.6.89_2026q2` 上重跑。

| 场景 | 命令要点 | 结果 |
| --- | --- | --- |
| `MALI_KBASE_DRM_RENDER=y`，`W=1` | 见上 | **成功**，`mali_kbase.ko` 生成（8.4 MB），`modinfo -F depends` = `drm` |
| 新文件告警 | `W=1` 全量编译 | `mali_kbase_drm.c` / `mali_kbase_drm.h` **0 warning** |
| `MALI_KBASE_DRM_RENDER=n` | 同上 | **成功**，`mali_kbase_drm.o` 不参与编译，`depends` 为空，无任何 `drm_*` 未定义符号 |
| `MALI_KBASE_DRM_RENDER=y` + `CONFIG_DRM=n` | 同上 | **编译期报错**：`CONFIG_MALI_KBASE_DRM_RENDER depends on CONFIG_DRM to be set in Kernel configuration` |
| `checkpatch.pl --strict` | 全部新增/修改文件 | 0 errors, 0 warnings |

`y` 配置下 `mali_kbase.ko` 的 DRM 未定义符号（由 modpost 从 DRM core 解析）：
`drm_dev_alloc`、`drm_dev_register`、`drm_dev_unplug`、`drm_dev_put`、`drm_open`、
`drm_release`、`drm_ioctl`、`drm_compat_ioctl`、`drm_poll`、`drm_read`。

硬件相关验收（节点出现、libdrm 查询）已在 Sky1 板卡上完成，见第 10 节；
unbind/卸载生命周期仍待 PR #3 的测试。

## 10. 板端测试记录（debian-o6 / Sky1，2026-11-13）

测试机：CIX Phecda（Sky1），Armbian `7.3.1-edge-cixdev`，GPU 为 ACPI 设备
`CIXH5000:00`，原本由 `panthor` 驱动；`linlon_dp.enable_render=0`，即 DPU 不提供
render node；系统已安装 `cix-gpu-kmd 1.0.0` DKMS 包。

由于 `cix_k6.6.89_2026q2` 分支本身无法在 7.3 上编译（`mm_get_unmapped_area()`
原型变化，见 `include/linux/version_compat_defs.h`），板端测试用**发行包源码**
`/usr/src/cix-gpu-kmd-1.0.0`（同一 DDK r54p1-11eac0，已包含 6.19+ 兼容修改）
加上本 PR 的补丁构建。

### 10.1 已验证

切换为 `modprobe.blacklist=panthor`，把打好补丁的模块按 DKMS 布局安装到
`/lib/modules/$(uname -r)/updates/dkms/` 后，模块由 udev 的 `kmod load` 自动加载，
probe 成功（无需手工 insmod）：

```
[drm] Initialized mali-kbase 1.0.0 for CIXH5000:00 on minor 5
mali CIXH5000:00: DRM render node renderD128 registered
mali CIXH5000:00: Probed as mali0
```

`tools/drm/drm-info /dev/dri/renderD128`：

```
node: /dev/dri/renderD128
  major=226 minor=128
  parent: ../../../CIXH5000:00
  driver: mali-kbase
  desc: ARM Mali GPU (kbase)
  version: 1.0.0
  cap_prime: 0x3
  cap_syncobj: 0x0
  cap_syncobj_timeline: 0x0
  prime_handle_to_fd: rejected
  prime_fd_to_handle: rejected
```

- `/dev/dri/renderD128` 与 `/dev/dri/card5` 的 parent 都是 `/sys/devices/platform/CIXH5000:00`，
  `uevent` 中 `DRIVER=mali`；
- `/dev/mali0` 正常创建；
- 与设计一致：DRM core 额外导出了 primary node `card5`（没有 KMS 对象）；
- `cap_syncobj` 为 0（未声明 `DRIVER_SYNCOBJ`），PRIME 两个方向都按预期被拒绝，
  没有"假装成功"的空实现；
- 打开/关闭 render node 连续 100 次全部成功，无新增内核告警。

### 10.2 7.3 上发现的两处 API 变化（已修复）

| 变化 | 现象 | 处理 |
| --- | --- | --- |
| `struct drm_driver` 删除 `.date` 字段（6.18 起） | 编译失败 | `#if (KERNEL_VERSION(6, 18, 0) > LINUX_VERSION_CODE)` |
| `drm_open_helper()` 要求 `fops->fop_flags` 含 `FOP_UNSIGNED_OFFSET`，否则 open 返回 `-EINVAL` 并 WARN（`drm_file.c:329`） | `drm-info` 打开 renderD128 报 `Invalid argument`，systemd-logind 触发 WARN | `#ifdef FOP_UNSIGNED_OFFSET` 中设置该标志，并显式 `#include <linux/fs.h>` |

### 10.3 模块压缩格式：必须用 CRC32

把 59 MiB（带调试信息）的 .ko 手工压缩成 `.ko.xz` 时，`xz` 默认使用 **CRC64** 校验。
内核的模块解压器（`kernel/module/decompress.c` + `lib/xz`）只支持 **CRC32**：

```
lib/xz/xz_dec_stream.c: if (s->check_type > XZ_CHECK_CRC32) return XZ_OPTIONS_ERROR;
```

结果 `finit_module()` 收到压缩数据后解压失败，内核打印
`decompression failed with status 6`，`modprobe` 返回 `EINVAL`，udev 的自动加载随之失败；
而 `insmod` 直接加载未压缩的 .ko 不受影响，这正是"手工 insmod 能加载、重启后不自动加载"
的原因。DKMS 包自带的 .ko.xz 本身就是 CRC32，所以打包时必须保持：

```bash
xz --check=crc32 -c mali_kbase.ko > mali_kbase.ko.xz
```

### 10.4 整机挂死与 IDM "GPU secure access"

`rmmod` 后再次 `insmod` 时整机失联，串口持续打印：

```
ERROR:   IDM: GPU secure access, error status=0x0, addr=0x0
```

该消息来自 BL31（ATF）的 RAS 处理：`plat/cix/sky1/ras/sky1_idm_ras.c` 中的
`IDM_ERR("%s %s access, error status=0x%x, addr=0x%lx")`，`GPU` 的 IDM 基地址是
`0x0d0eb000`。"secure access" 表示 GPU 发起了一次 secure 访问被 IDM 记录；
`status=0x0, addr=0x0` 说明处理函数读到的是已清零/无效的错误寄存器，但中断仍持续触发，
形成 EL3 中断风暴，非安全世界因此无法得到 CPU——这与"整机无响应"完全吻合。

触发路径是 kbase 在 `rmmod` 之后重新 probe：GPU 没有被完整 reset，CSF 固件和
protected/TZMP 配置处于不一致状态。这个行为在 6.6 上没有被验证过（该驱动此前从未
真正加载过），属于 PR #3 的生命周期问题，与本 PR 的 DRM 注册代码无关。当前建议：
测试期间不要做 rmmod/insmod 循环，改配置后直接重启。

另有一处与 DRM 无关的既有告警：`kbase_reset_gpu_init()` 创建的 workqueue 缺少
`WQ_UNBOUND/WQ_PERCPU`（7.3 新增检查），以及 SCMI perf domain 重复 OPP。
