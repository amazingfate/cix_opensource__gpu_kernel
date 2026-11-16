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

### 2.1 每个 DRM 文件一个 kbase 上下文

```c
struct kbase_drm_file {
	struct kbase_context *kctx;
};

static int kbase_drm_open(struct drm_device *dev, struct drm_file *file)
{
	struct kbase_device *kbdev = dev->dev_private;
	struct kbase_drm_file *kfile;

	kfile = kzalloc(sizeof(*kfile), GFP_KERNEL);
	if (!kfile)
		return -ENOMEM;

	kfile->kctx = kbase_create_context(kbdev, false, 0,
					   KBASE_API_VERSION(0, 0), NULL);
	if (!kfile->kctx) {
		kfree(kfile);
		return -ENOMEM;
	}

	file->driver_priv = kfile;
	return 0;
}

static void kbase_drm_postclose(struct drm_device *dev, struct drm_file *file)
{
	struct kbase_drm_file *kfile = file->driver_priv;

	if (!kfile)
		return;
	kbase_destroy_context(kfile->kctx);
	kfree(kfile);
}
```

- `kbase_create_context()` 在 `context/` 里是 `KBASE_EXPORT_SYMBOL`，`filp` 传 NULL
  表示内部上下文（与 `kbase_file_create()` 的调用方式一致）；
- `drm_driver.postclose` 在 DRM core 释放该文件的所有 GEM handle **之后**调用
  （`drm_release()` 先 `drm_gem_release()` 再 `postclose`），因此此时该文件的
  GEM 对象已全部释放，`kbase_destroy_context()` 不会遇到仍被引用的 region；
- 上下文销毁由 kbase 的既有逻辑完成（MMU 地址空间、CSF 状态、debugfs 等）。

`KBASE_API_VERSION(0, 0)` 的选择需要在实现时确认：内部上下文不使用 UMD 的
feature gating，若发现某处检查 `kctx->api_version`，改为传入当前 UK 版本。

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

### 2.4 在 import 回调里拿到"当前文件"

`gem_prime_import(dev, dma_buf)` 没有 `drm_file` 参数，而导入必须发生在
**打开 render node 的那个进程**的 kbase context 里。可选方案：

| 方案 | 做法 | 评价 |
| --- | --- | --- |
| A（推荐） | 用 `drm_file` 的 `driver_priv` 保存全局"当前导入者" | 需要把 ioctl 路径与文件关联，实现简单但有并发歧义 |
| B | 自定义 fops 的 `unlocked_ioctl` 包装 `drm_ioctl`，在入口设置 `current->kbase_drm_file` | 依赖线程局部状态，可重入 |
| C | 每个 DRM 文件使用独立 `drm_device` | 不符合 DRM 模型，否决 |

实现时优先采用 B：在 `kbase_drm_ioctl()` 里从 `file->private_data` 取出
`struct drm_file *`，通过 `drm_file` → `driver_priv` → `kctx` 设置到
`current` 的任务局部变量；`gem_prime_import` 里读取。若实现复杂度过高，退回
"每个 DRM 设备一个导入用 kctx"（A'），并在文档中标注这是共享地址空间的临时方案。

> 这一条是本 PR 最主要的设计不确定点，实现前先在板端验证
> `kbase_create_context(NULL filp)` + `kbase_mem_import_dma_buf()` 能正常工作。

## 3. 生命周期与引用

新增的引用关系（在 PR #3 的基础上）：

```
drm_file ──1:1──> kbase_drm_file ──> kbase_context ──> kbdev（弱引用，kbdev 由 drm_device 保活）
drm_file ──handle──> kbase_drm_gem ──> kbase_va_region ──> dma_buf（+attachment/sgt）
```

顺序保证：

1. `drm_release()`：先释放该文件的全部 handle → GEM free → region free →
   `dma_buf_detach/put`；然后调用 `postclose` → `kbase_destroy_context()`；
2. 若同一 dma-buf 被另一个文件/进程重复导入，各自持有独立的 attachment 与 region，
   引用计数互不影响；
3. `postclose` 中 `WARN_ON(!list_empty(&kctx->reg_rbtree))` 之类的自检（若代价可接受）
   用来发现遗留 region；
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

1. **内部上下文的可用性**：`kbase_create_context(..., filp=NULL, api_version)`
   是否能在没有 `/dev/mali0` 文件状态机的情况下正常建 MMU/CSF 状态，
   需先在板端用最小模块验证；
2. **VA 映射**：导入时使用 `BASE_MEM_IMPORT_HAVE_PAGES` 走 `kbase_gpu_mmap()`
   需要 kctx 的地址空间就绪；若失败，退回 `BASE_MEM_NEED_MMAP` + 惰性映射；
3. **并发导入的"当前文件"定位**（2.4），实现前先确定方案；
4. **驱动卸载**：持有 GEM 的 fd 会持有模块引用（fops->owner），卸载被阻止；
5. **性能**：每个文件一个 kbase context 会占用 MMU 页表与 CSF 结构，
   大量短连接客户端（如反复打开 render node 的合成器）需要测量开销；
6. **上游可接受性**：render node 只做导入、不做分配，属于最小能力集，
   便于后续与 Mesa/GBM 的真实需求对照。
