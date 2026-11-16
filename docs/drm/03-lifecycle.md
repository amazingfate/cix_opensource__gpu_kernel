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
| `suspend` | 持有 RTC 唤醒后 `systemctl suspend`（deep 与 s2idle 各一次） | PASS（使用 `rtc1` rx8900 alarm）：自动唤醒，节点与 `drm-info` 正常，0 新增告警；早期失败是测试脚本误用 `rtc0`（EFI RTC）所致，见 4.3 |
| `init-fail` | 注入 `kbase_drm_render_init()` 返回 `-ENODEV` | PASS：逆序回滚完整，`/dev/mali0` 与 DRM 节点均不存在，恢复后一切正常，见 4.4 |

unbind 之后重启，系统恢复正常：mali_kbase 由 udev 自动加载，`renderD128` 重新出现，
`drm-info` 读到 `driver: mali-kbase`，dmesg 只有既有的 workqueue 告警
（`kbase_reset_gpu_init()` 在 7.3 上未使用 `WQ_PERCPU/WQ_UNBOUND`，与本改动无关）。

## 5. 尚未完成

- suspend/resume：deep 与 s2idle 均已通过（rtc1 唤醒）；尚未测试"挂起时 GPU 上有
  活跃 UMD 任务"的场景，需要真实的 Mali UMD 负载配合；
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
- 问题只在 **唤醒侧**。

### 4.3 根因：用错了 RTC

板子有两个 RTC：

| 设备 | 驱动 | 说明 |
| --- | --- | --- |
| `rtc0` | `rtc-efi` | EFI 固件 RTC，写 `wakealarm` 会成功返回，但 **不能唤醒 SoC** |
| `rtc1` | `rtc-rx8900`（I2C） | 硬件 RTC，alarm 通过 GPIO 中断（IRQ 139，`cdns-gpio`）上报，`power/wakeup=enabled` |

最初两次测试都写的是 `rtc0/wakealarm`，所以板子一直睡到手动按电源键。
先在不挂起的状态验证 `rtc1`：arm alarm 后 `wakealarm` 被消费，IRQ 139 计数从 2 涨到 142，
说明 rx8900 的 alarm 中断正常。随后用 `rtc1` 重跑：

| 尝试 | 模式 | 唤醒源 | 结果 |
| --- | --- | --- | --- |
| 第三次 | deep | rtc1 +60s | **PASS**：自动唤醒，节点恢复，`drm-info` 正常，0 新增告警 |
| 第四次 | s2idle | rtc1 +60s | **PASS**：同上 |

唤醒后的补充验证：

- `1000` 次循环改跑 100 次（含 resume 后）全部成功；
- resume 后再次触发 GPU 软复位：`Resetting GPU` / `Reset complete`，节点与 fd 正常；
- 过程中没有新的内核告警（仅启动时既有的 workqueue WARN）。

观察到的两个平台现象（与本改动无关）：

1. 挂起期间系统墙上时钟不前进：`date` 在 suspend 前后相同，直到 NTP 重新校时
   （内核没有在 resume 时从 RTC 回写系统时间）；
2. `rtc1/wakealarm` 在唤醒后仍显示原值（rx8900 驱动不自动清除），测试脚本在每次
   arm 前先写 0 清掉。

结论：suspend/resume 在 kbase + DRM render node 场景下工作正常，之前的"挂死"是
测试脚本用错 RTC 导致的假象；`03-lifecycle-test.sh` 的 suspend 用例已改为默认使用
`rtc1`（可用 `RTC_DEV=` 覆盖），并在 arm 后校验 alarm 确实生效。

### 4.4 初始化失败回滚（故障注入）

在板端构建树里临时让 `kbase_drm_render_init()` 直接返回 `-ENODEV`（仅测试用，
不提交），安装并重启，验证 `dev_init[]` 的逆序回滚：

```
mali CIXH5000:00: DRM render node registration failed error = -19
mali CIXH5000:00: sky1_gpu_detach_pd
mali CIXH5000:00: Device initialization failed
```

结果：

- `/dev/mali0` 不存在，`/dev/dri` 下没有新的 render/primary 节点（没有半初始化设备）；
- 电源域已 detach，probe 返回失败；
- 恢复真实模块并重启后，`renderD128`、`/dev/mali0`、`drm-info` 全部恢复正常。

这验证了 PR #2 要求 10/11：注册失败不会留下半初始化设备，回滚路径覆盖 DRM 项。

### 4.5 补充：加载过固件后的 unbind

PR #3 的 unbind 用例是在 CSF 固件从未加载的情况下做的（当时 render node 没有使用
GPU 的路径）。PR #4 的导入路径会调用 `kbase_device_firmware_init_once()`，一旦固件
加载过，运行中 unbind 时 `kbase_csf_firmware_unload_term()` 会释放固件内存池页面并
触发 `BUG: Bad page state ... nonzero private`（约 35 条）。

该问题在**没有任何 DRM 参与**时也能复现：只用 `exec 3</dev/mali0` 打开/关闭
`/dev/mali0` 触发固件加载，再 unbind 即可。因此属于 DDK 在 7.3 上的既有缺陷，
与本项目的 DRM/GEM 代码无关，详见 `docs/drm/04-gem-prime.md` 8.1。
