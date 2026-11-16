# PR #4 设计：GEM 与 PRIME Import

本阶段为 Mali render node 增加最小可用的 GEM 能力，只支持**外部 dma-buf 导入**，
让通用用户态（GBM/Mesa/合成器）可以在标准 render node 上完成
`DRM_IOCTL_PRIME_FD_TO_HANDLE` → 使用 handle → `DRM_IOCTL_GEM_CLOSE` 的流程。
暂不实现导出、mmap 和原生分配。

## 1. 目标与非目标

目标：

- render node 声明 `DRIVER_GEM`；
- 每个打开 render node 的文件拥有一个独立的 kbase context / GPU 地址空间；
- `PRIME_FD_TO_HANDLE` 走 `drm_gem_prime_import()`，底层复用 kbase 现有的
  `kbase_mem_from_umm()` UMM 导入路径（dma_buf_get → dma_buf_attach →
  dma_buf_map_attachment）；
- `GEM_CLOSE` 释放 GEM 对象与 kbase region，重复导入/关闭引用计数正确；
- 关闭时无泄漏、无 use-after-free。

非目标（明确返回错误，不做空实现）：

- `PRIME_HANDLE_TO_FD`（kbase 没有 dma-buf exporter）；
- `mmap`（没有可 mmap 的原生分配）；
- `MODE_CREATE_DUMB` / 私有分配 ioctl；
- 通过 render node 提交 GPU job（仍走 `/dev/mali0`）。

## 2. 关键设计

### 2.1 每个 DRM 设备一个导入上下文

`gem_prime_import(dev, dma_buf)` 的回调签名里没有 `drm_file`，而导入又必须发生在一个
kbase context 里。PR #4 的取舍是：**在 DRM 设备级别维护一个惰性创建的导入上下文**，
所有 render node 客户端共享它的 GPU 地址空间：

```c
struct kbase_drm_device {
	struct kbase_device *kbdev;
	struct kbase_context *import_ctx;
	struct mutex import_lock;
};
```

- 第一次导入时（`kbase_drm_import_context_get()`，持 `import_lock`）创建；
- 创建前必须先调用 `kbase_device_firmware_init_once(kbdev)`：CSF 固件和调度器
  线程在 `/dev/maliX` 首次 open 时才加载，render node 没有这条路径。实测不调用时
  `kbase_create_context()` 会在 `kbase_csf_scheduler_context_init()` 里
  `WARN_ON(!kthread_running)` 并失败；
- `kbase_create_context(kbdev, false, 0, 0, NULL)`：`filp` 为 NULL 表示内部上下文，
  api_version 只影响 `MEM_QUERY` 的返回标志，导入路径不会查询；
- 上下文在 `drm_driver.release`（最后一个 DRM 引用释放）时销毁，此时所有 GEM 对象
  都已释放，不会留下仍被引用的 region。

已知限制：所有客户端共享同一个导入地址空间；如果后续 PR #5/#6 需要按进程隔离
（例如每个客户端独立的 GPU VA），再改为 per-file 上下文，需要在 fops 的 ioctl 路径
上把当前 `drm_file` 与 `gem_prime_import` 关联起来。

### 2.2 GEM 对象

```c
struct kbase_drm_gem {
	struct drm_gem_object base;
	struct kbase_va_region *reg;
};
```

- `reg` 由 kbase UMM 导入路径创建并已映射到该文件的 GPU 地址空间；
- GEM 对象本身不保存 kctx 指针，`postclose` 在 GEM 释放之后才销毁 kctx；
- `drm_gem_object_funcs.free`（或 `drm_driver.gem_free_object_unlocked`）里调用
  `kbase_mem_free_region(kctx, reg)`；该函数负责 unmap attachment、detach dma_buf、
  put dma_buf（顺序见 `docs/drm/01-current-behavior.md` 4.2）。

### 2.3 导入路径

当前 `kbase_mem_from_umm()` 是 `mali_kbase_mem_linux.c` 内的 static 函数，签名以
`int fd` 为输入。PR #4 需要把它拆成两层：

```c
/* 新增，供 DRM 侧使用 */
struct kbase_va_region *
kbase_mem_import_dma_buf(struct kbase_context *kctx, struct dma_buf *dmabuf,
			 u64 *va_pages, base_mem_alloc_flags *flags);

/* 保留原 fd 版本，内部 dma_buf_get() 后调用上面的函数 */
static struct kbase_va_region *
kbase_mem_from_umm(struct kbase_context *kctx, int fd, ...);
```

- 函数内部**不**调用 `dma_buf_get()`（引用由调用者持有并转移），避免额外的
  fd→dma_buf 往返；
- `kbase_mem_import_dma_buf()` 负责 `dma_buf_attach`、大小校验、region 分配、
  `kbase_gpu_mmap()`（`BASE_MEM_IMPORT_HAVE_PAGES` 路径），失败时按现有
  `error_out` 顺序回退；
- DRM 侧：

```c
static struct drm_gem_object *
kbase_drm_gem_prime_import(struct drm_device *dev, struct dma_buf *dma_buf)
{
	struct kbase_drm_file *kfile = ... /* 见 2.4 */;
	struct kbase_drm_gem *obj;
	u64 va_pages;
	base_mem_alloc_flags flags = 0;

	obj = kzalloc(...);
	drm_gem_object_init(dev, &obj->base, dma_buf->size);  /* 仅供 core 记账 */
	obj->reg = kbase_mem_import_dma_buf(kfile->kctx, dma_buf, &va_pages, &flags);
	if (!obj->reg) { ... }
	return &obj->base;
}
```

- `drm_gem_prime_import()` 的通用实现会调用 `drm_driver.gem_prime_import`；
  我们使用自己的实现以取得文件私有的 kctx（通用实现拿不到 `drm_file`，见 2.4）。

### 2.4 关于"在 import 回调里拿到当前文件"

`gem_prime_import(dev, dma_buf)` 没有 `drm_file` 参数，这是本 PR 最初的主要
不确定点。备选方案及其结论：

| 方案 | 做法 | 结论 |
| --- | --- | --- |
| A | 每个 DRM 设备一个共享导入上下文 | **采用**（PR #4），实现简单、无并发歧义；共享 VA 空间 |
| B | fops ioctl 包装 + 线程局部 `kctx`，实现 per-file 上下文 | 留待需要按进程隔离时再做 |
| C | 每个 DRM 文件一个独立 `drm_device` | 不符合 DRM 模型，否决 |

方案 B 若要落地，需要在 `kbase_drm_ioctl()` 里从 `filp->private_data`
（`struct drm_file`）取出 `driver_priv`，保存到任务局部变量，供
`gem_prime_import` 读取；这会让导入路径依赖 ioctl 的调用上下文，因此 PR #4
先用方案 A。

## 3. 生命周期与引用

新增的引用关系（在 PR #3 的基础上）：

```
drm_device ──> kbase_drm_device.import_ctx ──> kbase_context ──> kbdev（弱引用，kbdev 由 drm_device 保活）
drm_file ──handle──> kbase_drm_gem ──> kbase_va_region ──> dma_buf（+attachment/sgt）
```

顺序保证：

1. `drm_release()`：先释放该文件的全部 handle → GEM free → region free →
   `dma_buf_detach/put`；所有 fd 关闭后 `drm_driver.release` 触发
   `kbase_destroy_context()`；
2. 若同一 dma-buf 被另一个文件/进程重复导入，各自持有独立的 attachment 与 region，
   引用计数互不影响；
3. 如果 GEM 对象在上下文销毁后仍被引用会触发 UAF，因此上下文只在 DRM 设备最后
   一个引用释放时销毁（此时不可能还有 GEM）；
4. DRM 设备 unplug 时（PR #3 已实现），已有 fd 的 ioctl 返回 `-ENODEV`，
   GEM/上下文在 close 时按上述顺序回收。

## 4. 错误处理

| 场景 | 返回 |
| --- | --- |
| dma-buf 不是本设备可导入的类型/大小非法 | `-EINVAL` |
| kctx 创建失败 | `-ENOMEM` |
| region 分配/映射失败 | `-ENOMEM` / `-EINVAL`，回滚 attach |
| `PRIME_HANDLE_TO_FD` | `-EOPNOTSUPP`（未实现 export） |
| `mmap` | `-ENODEV`（不提供 fops->mmap） |
| `MODE_CREATE_DUMB` | `-EOPNOTSUPP` |

## 5. 测试计划

前置：`CONFIG_MALI_KBASE_DRM_RENDER=y`，`DRIVER_GEM` 已声明，模块经
`xz --check=crc32` 安装并重启。

用例（每次改动后重启，不 reload 驱动）：

1. **基础往返**：用 dma-heap 创建 dma-buf fd（`/dev/dma_heap/system`），
   `PRIME_FD_TO_HANDLE` 成功、handle 非 0；`GEM_CLOSE` 成功；
2. **重复导入**：同一 dma-buf fd 导入两次 → 两个 handle；交错关闭；
3. **跨进程**：进程 A 导入、把 handle 传给 B 的场景不在本阶段支持（handle 属于
   打开它的文件），改为验证两个进程各自导入同一 dma-buf；
4. **循环/泄漏**：1000 次导入+关闭，前后 kmemleak 与 `slabinfo` 对比、
   `dmesg` 无 refcount/BUG；
5. **unbind 生命周期**：持有 handle 时 unbind → 后续 ioctl `-ENODEV`，close 时
   GEM/上下文按序回收，无告警；随后重启；
6. **suspend/resume**：保持一个已导入的 handle，rtc1 唤醒后再次
   `PRIME_FD_TO_HANDLE`/`GEM_CLOSE` 正常；
7. **负向**：对 DEVICE 发 `PRIME_HANDLE_TO_FD`、`CREATE_DUMB`、`mmap`，
   确认返回错误而不是成功。

工具：`tools/drm/` 新增 `drm-prime-test.c`（dma-heap + 导入/关闭循环），
必要时用发行包自带的 `dma_buf_test_exporter` 作为导出端。

## 6. 风险与待验证点

1. **内部上下文的可用性**：已验证必须先在导入路径调用
   `kbase_device_firmware_init_once()`；否则 `kbase_create_context()` 在
   `kbase_csf_scheduler_context_init()` 触发 `WARN_ON(!kthread_running)` 并失败；
2. **VA 映射**：导入时使用 `BASE_MEM_IMPORT_HAVE_PAGES` 走 `kbase_gpu_mmap()`
   需要 kctx 的地址空间就绪；若失败，退回 `BASE_MEM_NEED_MMAP` + 惰性映射；
3. **共享导入上下文**：所有客户端共用一个 GPU 地址空间，见 2.4；
4. **驱动卸载**：持有 GEM 的 fd 会持有模块引用（fops->owner），卸载被阻止；
5. **性能**：每个文件一个 kbase context 会占用 MMU 页表与 CSF 结构，
   大量短连接客户端（如反复打开 render node 的合成器）需要测量开销；
6. **上游可接受性**：render node 只做导入、不做分配，属于最小能力集，
   便于后续与 Mesa/GBM 的真实需求对照。

## 7. 当前状态（2026-11-16）

已实现：

- `kbase_mem_from_umm()` 重构为 `kbase_mem_import_dma_buf()`，新增
  `kernel_import` 参数：内核导入使用 `CUSTOM_VA` 区并立即 `kbase_gpu_mmap()`，
  用户态导入保持原有 cookie/SAME_VA 行为；
- `drm/mali_kbase_drm_gem.{c,h}`：GEM 对象、`free` 回调、惰性导入上下文；
- `drm/mali_kbase_drm_prime.c`：`gem_prime_import` → UMM 导入 → GPU 映射；
- render node 声明 `DRIVER_RENDER | DRIVER_GEM`；导出、dumb、mmap 仍未实现。

构建验证：

| 内核 | 结果 |
| --- | --- |
| vanilla 6.6.89 arm64（`W=1`） | 通过 |
| 板端 7.3.1-edge-cixdev | 通过，三个新目标文件均编译链接成功 |

板端联调进展：

1. 首次 `PRIME_FD_TO_HANDLE` 失败（ENODEV），dmesg 显示
   `kbase_csf_scheduler_context_init()` 的 `WARN_ON(!kthread_running)`：
   导入上下文创建前没有加载 CSF 固件；已改为先调用
   `kbase_device_firmware_init_once()`，之后固件加载成功、上下文创建正常；
2. 随后 `kbase_mem_import_dma_buf()` 返回 NULL（`failed to import the dma-buf
   through the UMM path`）。已在该函数的各失败分支加上 `dev_err` 诊断
   （attach 失败、大小非法、region 分配失败、`kbase_alloc_create` 失败、
   flags 非法），等待复测定位；
3. 诊断版本安装期间板子因 WiFi（rtw89）断连后内核无响应而失联，需要断电重启，
   复测因此中断。

下一步：板子恢复后重跑 `drm-prime-test`，按诊断信息修正 UMM 导入路径，再跑完整
测试矩阵（重复导入、循环、unbind、suspend、负向用例）。

## 8. 板端测试结果（2026-11-16，7.3.1-edge-cixdev）

实现经过三轮板端修正后通过：

1. **内部上下文必须兼容模式**：64-bit context 在用户发起 JIT 之前没有
   `CUSTOM_VA` 区，`kbase_ctx_alloc_free_region(CUSTOM_VA_ZONE)` 会失败
   （`failed to allocate a region in zone 1`）。导入上下文改为
   `kbase_create_context(kbdev, is_compat=true, ...)`，从创建起就有
   `CUSTOM_VA` 区，导入即可立即映射；
2. **必须先加载 CSF 固件**：见 2.1；
3. **卸载顺序**：导入上下文如果留到 `kbase_device_term()` 之后才销毁，
   timeline/memory/misc 的 term 会检测到仍然存活的 context 并告警。现在
   `kbase_drm_render_term()` 在 `drm_dev_unplug()` 之后立即调用
   `kbase_drm_gem_term()`：先释放所有 GEM 对象的 region，再销毁导入上下文，
   然后才继续 kbase 的其余 term。仍持有 fd 的 GEM 对象只保留 wrapper，
   其 free 回调不再访问 kbase。

测试结果：

| 用例 | 结果 |
| --- | --- |
| 单次导入 + `GEM_CLOSE` | PASS（`imported=1 failed=0`） |
| 1000 次导入/关闭循环 | PASS（`imported=1000 failed=0`，耗时约 40 ms） |
| 8 进程并发，各 50 次 | PASS（8 × `imported=50 failed=0`） |
| 持有 GEM handle 时 unbind | PASS：`GEM_CLOSE` 返回 `ENODEV`，节点消失，**0 条 WARNING** |
| `PRIME_HANDLE_TO_FD` 导出 | 按预期拒绝（未实现 export） |

### 8.1 已知的既有问题：加载过固件后 unbind 会报 Bad page state

板端 unbind 用例中，只要 CSF 固件被加载过（`/dev/mali0` 被 open 过，或 render
node 做过一次导入），unbind 时 `kbase_csf_firmware_unload_term()` 释放固件内存池
页面会触发约 35 条：

```
BUG: Bad page state in process sh  pfn:...
page dumped because: nonzero private
__free_pages -> example_mgm_free_page -> kbase_mem_pool_free_page
 -> kbase_csf_firmware_unload_term -> kbase_device_term
```

在**完全没有 DRM 参与**的情况下（只 `exec 3</dev/mali0` 打开再关闭，然后 unbind）
可以同样复现，因此这是 DDK 在 7.3 上的既有问题，与 GEM/PRIME 代码无关，
需要单独作为驱动兼容性缺陷跟踪。它不影响正常的重启/关机路径，只影响运行中的
platform unbind。
