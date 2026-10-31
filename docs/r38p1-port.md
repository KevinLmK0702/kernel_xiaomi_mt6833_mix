# Mali r38p1 + Vulkan 1.3 on MT6833 (evergo / everpal)

## 状态

内核侧已可完整编译：`mali-r38p1` 作为 built-in 驱动链接进 `vmlinux` /
`Image.gz`。用户态仍是 r32p1，需要按下面的步骤替换成 r38p1 的 G57 MC2 blob
后才会真正报 Vulkan 1.3。

## 内核侧改动

| 文件 | 说明 |
|---|---|
| `drivers/misc/mediatek/gpu/gpu_mali/mali_valhall/mali-r38p1/` | 新增 DDK 树（r38p1-01eac0） |
| `arch/arm64/configs/evergo_defconfig` | `CONFIG_MTK_GPU_VERSION="mali valhall r38p1"` |
| `arch/arm64/configs/everpal_defconfig` | 同上 |
| `build_gpu.sh` | 只重编 GPU 子树，便于迭代 |

### 源码来源

- 核心 DDK：`Content-Creation-Club/android_kernel_xiaomi_dash_mtk_modules`
  （分支 `dash-w-oss`，`gpu/gpu_mali/mali_valhall/mali-r38p1`）。干净的 MTK
  树，无 Exynos 补丁，`MALI_RELEASE_NAME="r38p1-01eac0"`。
- MTK 平台层：`Addster09/android_kernel_xiaomi_mt6833` 的 r37p0 提交
  （`gold-s-oss`）中的 `platform/mt6853`、`platform/mtk_platform_common`、
  `Kbuild-mtk-custom-env`。本机 `CONFIG_MTK_PLATFORM="mt6853"`，所以走
  `mt6853` 平台目录。
- 补充头文件：`mtk_gpu_trace.h`（dash 树）、`mtk_gpu_power_sspm_ipi.h`
  （本机 r32p1 树）。

### 4.14 适配

- `mali_kbase_vinstr.c`：`gpufreq_get_cur_*` 按 `CONFIG_MTK_GPUFREQ_V2`
  分支，本机走 `mt_gpufreq_get_cur_freq/volt()`（gpufreq v1）。
- `mali_kbase_mem_linux.c`：去掉不存在的 `<mtk_heap.h>`；
  `is_support_secure_handle()` / `dmabuf_to_secure_handle()` 以 no-op 桩替代
  （SVP/受保护内存不可用，见“已知限制”）。
- `platform/mtk_platform_common/mtk_gpu_power_sspm_ipi.c`：SWPM/LTR 用 no-op
  桩（r32p1 的实现依赖 r32 专有 vinstr API）。
- `mali_malisw.h`：`fallthrough` 宏加 `#ifndef` 保护。
- `platform/mtk_mfg_counter.h`：补 `struct GPU_PMU;` 前向声明。
- `Makefile`：`-Wno-error=typedef-redefinition -Wno-error=macro-redefined
  -Wno-error=visibility`（4.14 内核已有这些 typedef，clang 会告警）。
- BORE 调度器：其 Kconfig 默认 `y`，但 backport 缺少 `reweight_task()`，会
  导致 vmlinux 链接失败。已通过 rebase 整体 drop BORE 引入提交
  (`63dd7d036af6`)，不再需要任何 scheduler 改动。

## 构建

```bash
./build.sh          # 完整构建并打包 AnyKernel3
# 或只重编 GPU：
./build_gpu.sh
```

产物：`out/arch/arm64/boot/Image.gz`（r38p1 已在内）。

## 用户态替换（r38p1 / Vulkan 1.3）

目前公开可得、且明确面向 Mali-G57 MC2 的 r38p1 用户态包是 dobrogrind 的
`Mali_38p1_VK_1-3`（Magisk 模块），最简单的应用方式是
`Swaggyxren/MaliVK13Patcher`：它直接把 blob 写进 `vendor.img`。

需要替换/新增的文件（64 位；32 位按 ROM 情况一并处理）：

```
/vendor/lib64/egl/libGLES_mali.so
/vendor/lib64/hw/vulkan.mali.so
/vendor/lib64/libarm_egl_properties_sysprop.so
/vendor/lib64/libarm_gralloc_properties_sysprop.so
/vendor/lib64/libgpumem.so
/vendor/lib64/libgpuservice.so
/vendor/lib64/libgpuwork.so
/vendor/lib64/liblibarm_mali_config_sysprops.so
```

注意路径：先看 `adb shell ls /vendor/lib64/egl/`。如果 ROM 使用
`/vendor/lib64/egl/mt6853/`，blob 要放到该子目录（或同时放一份到
`egl/`）。

### 属性

把 `files/r38p1-egl.prop` 追加到 `/vendor/build.prop`
（`ro.vendor.arm.egl.configs.*` 缺了会在 `eglInitialize` 阶段崩）。

### Vulkan 声明

修改 `/vendor/etc/permissions/android.hardware.vulkan.version.xml`：

```xml
<permissions>
    <feature name="android.hardware.vulkan.version">
        <version>4206592</version> <!-- 1.3.0 -->
    </feature>
</permissions>
```

`android.software.vulkan.deqp.level.xml` 建议保持 ROM 原值；r38p1 是 2022
年的驱动，不要照抄 r54 的 2025-03-01 等级。

### SELinux

```bash
chcon u:object_r:same_process_hal_file:s0 /vendor/lib64/egl/libGLES_mali.so ...
chcon u:object_r:vendor_configs_file:s0 /vendor/etc/permissions/*.xml
```

改 `vendor.img` 需要关闭 verity：

```bash
fastboot flash vbmeta --disable-verity --disable-verification vbmeta.img
```

## 验收

```bash
adb shell dmesg | grep -iE "mali|kbase"        # 应出现 r38p1
adb shell cat /sys/class/misc/mali0/device/gpuinfo
adb shell dumpsys SurfaceFlinger | grep -o "v1\.r38p1-[0-9a-f]*"
adb shell pm list features | grep vulkan.version   # 4206592
adb shell getprop ro.vendor.arm.egl.configs.r8_g8_b8_a8_32bit_fixed.hal_format
```

再跑 Vulkan CTS / deqp / 3DMark，并回归相机、视频、待机。

## 已知限制

- **SWPM/LTR 关闭**：`MTKGPUPower_model_*` 是 no-op，功耗模型/部分频率统计
  不可用，不影响基本渲染。
- **受保护内存（SVP）不可用**：`is_support_secure_handle()` 恒为 false，
  protected buffer 会退化为 `sg_phys()`。不要用它跑 DRM/SVP 视频。
- **用户态/内核必须同版本**：内核是 r38p1-01eac0（MTK 分支）。若 EGL 初始化
  失败，先看 `dmesg` 是否有 `Kernel version mismatch`；必要时换用与你
  userspace 完全同源的 r38p1 内核树。
- **BORE 已移除**：`CONFIG_SCHED_BORE` 默认 `y`，其 backport 缺少
  `reweight_task()`，会链接失败；已用 rebase drop 掉 BORE 提交。若以后
  重新引入 BORE，需要补 `reweight_task()`。
