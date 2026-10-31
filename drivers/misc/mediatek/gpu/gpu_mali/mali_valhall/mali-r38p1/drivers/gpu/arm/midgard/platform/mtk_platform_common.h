// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019 MediaTek Inc.
 */

#ifndef __MTK_PLATFORM_COMMON_H__
#define __MTK_PLATFORM_COMMON_H__

#include <linux/platform_device.h>

struct kbase_device *mtk_common_get_kbdev(void);

bool mtk_common_pm_is_mfg_active(void);
void mtk_common_pm_mfg_active(void);
void mtk_common_pm_mfg_idle(void);

void mtk_common_debug_dump(void);

int mtk_common_gpufreq_bringup(void);
int mtk_common_gpufreq_commit(int opp_idx);
int mtk_common_ged_dvfs_get_last_commit_idx(void);

/* Latest smoothed GPU utilisation (0..100), sampled by the DVFS governor. */
unsigned int mtk_common_gpu_util_get(void);

/* GED glue: publish the utilisation through mtk_get_gpu_loading(). */
void mtk_common_gpu_util_init(void);
void mtk_common_gpu_util_term(void);

/* Standalone GPU DVFS governor (does not use GED's governor). */
void mtk_common_gpu_dvfs_init(void);
void mtk_common_gpu_dvfs_term(void);

/* Last OPP index committed by the governor, -1 if it never committed. */
int mtk_common_gpu_last_idx(void);

/* True when GED's frame based policy owns the GPU OPP. */

/* Record an OPP index committed outside of this governor. */
void mtk_common_gpu_note_idx(int opp_idx);

int mtk_common_device_init(struct kbase_device *kbdev);
void mtk_common_device_term(struct kbase_device *kbdev);

#if IS_ENABLED(CONFIG_PROC_FS)
void mtk_common_procfs_init(void);
void mtk_common_procfs_exit(void);
#endif

int mtk_platform_device_init(struct kbase_device *kbdev);
void mtk_platform_device_term(struct kbase_device *kbdev);

#endif /* __MTK_PLATFORM_COMMON_H__ */

