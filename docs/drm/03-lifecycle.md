# PR #3：生命周期与稳定性

本文描述第二阶段的引用模型、代码改动和板端测试方法。目标是在 PR #2 的
"设备模型"基础上，保证 DRM 设备、kbase 设备与打开的文件描述符在任何拆除顺序下
都不会互相访问已释放的内存。

## 1. 问题

PR #2 之后，DRM 设备可以比 platform device 活得更久：

- `kbase_platform_device_remove()` 会同步执行 `kbase_device_term()`（逆序拆掉
  `dev_init[]` 的所有资源），然后释放 `kbase_device`；
- 但此时仍可能有进程持有 `/dev/dri/renderD128` 的 fd。`drm_device` 本身是引用计数的，
  会在最后一个 fd 关闭前一直存在；如果它手里有 `kbdev` 指针，就会形成悬空引用。

## 2. 引用模型

```
platform device (CIXH5000:00)
  │  持有一个 kbdev 引用（probe 时取得，remove 时释放）
  ▼
kbase_device (kref)
  │  被 drm_device 再持有一个引用（kbase_drm_render_init 取得，
  │  kbase_drm_release 释放）
  ▼
drm_device (drm_dev_get/put)
  │  每个打开的 drm_file 持有一个引用（drm_open 取得，drm_release 释放）
  ▼
/dev/dri/renderD128 的 fd
```

`kbase_drm_render_term()` 只做两件事：`drm_dev_unplug()` 摘除节点（此后所有 ioctl
返回 `-ENODEV`），`drm_dev_put()` 释放驱动自己持有的那个 DRM 引用。它 **不** 释放
kbase 设备引用，后者由 `kbase_drm_release()`（DRM 设备最终释放时调用）负责。

### 2.1 引用计数时间线

| 事件 | kbdev refs | 说明 |
| --- | --- | --- |
| `kbase_platform_device_probe()` 成功 | 1 | platform driver 持有 |
| `kbase_drm_render_init()` 成功 | 2 | DRM 设备持有第二个引用 |
| 打开 render node fd | 2 | fd 只持有 drm_device 引用，不直接持有 kbdev |
| `kbase_platform_device_remove()` 执行 `term` | 2 | DRM 先 unplug，节点消失 |
| remove 末尾 `kbase_device_put()` | 1 | platform 引用释放，内存不释放（还有 fd） |
| 最后一个 fd `close()` | 0 | `drm_dev_release` → `kbase_drm_release` → put → `vfree(kbdev)` |

若 remove 时没有任何 fd 打开，最后一行在前一行同一时刻发生，行为与 PR #2 相同。

### 2.2 失败路径

| 失败点 | 行为 |
| --- | --- |
| `drm_dev_alloc()` 失败 | 不取 kbdev 引用，`dev_init[]` 回滚，kbdev 由 platform 引用释放 |
| `drm_dev_register()` 失败 | `drm_dev_put()` → `kbase_drm_release()` → 归还 kbdev 引用，回到 1 |
| `kbase_device_init()` 其它步骤失败 | 逆序 term，DRM 项（若已成功）先 unplug 并归还引用 |
| 持有 fd 时 unbind | 见上表；fd 的 ioctl 返回 `-ENODEV`，close 安全 |
| 模块卸载时有 fd | VFS 对 fops 持有模块引用，卸载被阻止 |

## 3. 代码改动

1. `struct kbase_device` 新增 `struct kref refcount`，初始化为 1（由
   `kbase_device_alloc()` 完成）。
2. `kbase_device_get()` / `kbase_device_put()` 取代原来的 `kbase_device_free()`，
   最后一个引用释放时才 `vfree()`。
3. `kbase_platform_device_remove()`：`kbase_device_term()` 之后改为
   `kbase_device_put()`，不再直接释放内存；注释说明内存可能晚于 remove 释放。
4. `mali_kbase_drm.c`：注册时 `drm->dev_private = kbdev` 并 `kbase_device_get()`；
   `drm_driver.release = kbase_drm_release` 在 DRM 设备最终释放时归还引用。
5. `kbase_drm_render_term()` 的 unplug 语义不变。

## 4. 板端测试

测试机仍是 debian-o6（7.3.1-edge-cixdev），模块用本分支源码构建，
`xz --check=crc32` 打包后安装并 **重启**（不做 rmmod/insmod）。

测试工具（`tools/drm/`）：

- `drm-info`：节点身份与能力探测；
- `drm-hold`：打开并保持 fd，可 fork 子进程继承 fd，支持在持有期间执行 ioctl；
- `03-lifecycle-test.sh <case>`：每个 case 前后统计内核 WARNING/BUG/oops 数量，
  有任何新增即判失败。

规定：**每个用例一次启动；unbind 用例每次启动只做一次，做完立即重启。**

### 4.1 结果（2026-11-16，refcount 版本）

| 用例 | 内容 | 结果 |
| --- | --- | --- |
| `loop` | 1000 次 open/close | PASS，0 失败，0 新增告警 |
| `multi` | 16 个进程同时打开并各执行一次 ioctl | PASS，16 次 ioctl 成功，0 新增告警 |
| `fork` | 父进程持有 fd，子进程继承后关闭，父进程继续使用 | PASS，父进程 ioctl 正常，0 新增告警 |
| `unbind` | 持有 fd，`echo CIXH5000:00 > .../mali/unbind` | PASS，节点消失，持有的 fd ioctl 返回 `ENODEV(19)`，close 正常，0 新增告警 |
| `reset` | 持有 fd，`echo 1 > /sys/kernel/debug/mali0/reset` | PASS，dmesg 记录 `Preparing to soft-reset GPU` / `Reset complete`，fd ioctl 仍成功，节点仍在，0 新增告警 |
| `suspend` | 持有 RTC 唤醒（+30s）后 `systemctl suspend`（s2idle） | **未通过/待定位**：挂起后 5 分钟内板子未恢复 SSH 与 ICMP，需要断电重启。详见 4.2 |

unbind 之后重启，系统恢复正常：mali_kbase 由 udev 自动加载，`renderD128` 重新出现，
`drm-info` 读到 `driver: mali-kbase`，dmesg 只有既有的 workqueue 告警
（`kbase_reset_gpu_init()` 在 7.3 上未使用 `WQ_PERCPU/WQ_UNBOUND`，与本改动无关）。

## 5. 尚未完成

- suspend/resume：串口日志确认挂起入口路径完整（驱动 suspend → 次核下线 → PSCI
  `FFA_SUSPEND` 收到响应），失败在唤醒侧；需要先确认 RTC alarm 本身能否触发，
  再带串口重跑 s2idle/deep 两种模式的唤醒测试；
- 初始化中途失败的故障注入（需要在每个注入点各做一次重启）；
- unbind 后重新绑定的行为不在本阶段验证范围内（会触发已知的 re-probe 问题，
  按约定不在板子上做）；
- 内核无 KASAN 构建，UAF 只能靠 refcount/警告和 `unbind` 用例间接覆盖。

### 4.2 suspend 串口日志（2026-11-16）

板子的串口在 `/dev/ttyUSB0`（115200），主机侧抓到的第二次挂起（`PM: suspend entry (deep)`）
日志显示内核挂起路径是完整的：

```
[  163.750789] PM: suspend entry (deep)
[  163.875677] Freezing user space processes
[  163.875677] Freezing user space processes completed (elapsed 0.007 seconds)
[  163.883508] Freezing remaining freezable tasks
[  163.884792] Freezing remaining freezable tasks completed (elapsed 0.001 seconds)
...
[  164.075281] linlondp CIXH5010:00: linlondp_pm_suspend
[  164.244857] cdns-usbssp ... enter D3 succeed
[  164.576281] sky1-pcie-cix sky1-pcie.2.auto: sky1_pcie_suspend_noirq
[  164.639128] Disabling non-boot CPUs ...
[  164.644687] psci: CPU11 killed (polled 0 ms)
...
NOTICE:  Send CMD: FFA_SUSPEND
NOTICE:  Send CMD: 0x82000007
NOTICE:  Got SUSPEND RSP, size 13
```

随后串口静默。也就是说：

- 设备驱动（DPU、USB、PCIe）的 suspend 回调都正常返回，kbase 也没有打印
  "Abort suspend as GPU suspension failed"；
- 次核下线、PSCI 进入固件挂起（`FFA_SUSPEND` 收到响应）都完成；
- 问题在 **唤醒侧**：没有 resume 输出。

第一次尝试（s2idle + RTC +30s）同样没有恢复，说明 RTC 唤醒没有生效或固件没有恢复。
后续排查方向：先在正常运行状态下确认 RTC alarm 能触发（不挂起），再带串口重跑
s2idle + RTC 唤醒，观察是否需要 `FFA_RESUME` 之类的固件输出；必要时对比 deep 模式。
