// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2026 Mix-Kernel
 *
 * MTK GED GPU utilisation reporting.
 *
 * The r38p1 MTK platform layer was shipped trimmed down: it keeps the
 * gpufreq commit wrapper but never registers
 * ged_dvfs_cal_gpu_utilization_ex_fp with GED the way r32p1 did. As a
 * result gpu_av_loading stays at 0 and /sys/kernel/ged/hal/gpu_utilization
 * always reported "0 0 100".
 *
 * The actual sampling lives in mtk_gpu_dvfs.c, which already has to read the
 * kbase PM metrics for the governor. This file is just the GED callback
 * shim: it publishes that value so the sysfs node and anything else going
 * through mtk_get_gpu_loading() sees the same number the governor used.
 */

#include <linux/string.h>

#include <mali_kbase.h>
#include <platform/mtk_platform_common.h>

#include <ged_dvfs.h>

/* Provided by GED (drivers/misc/mediatek/gpu/ged/src/ged_dvfs.c). */
extern void (*ged_dvfs_cal_gpu_utilization_ex_fp)(unsigned int *pui32Loading,
						  unsigned int *pui32Block,
						  unsigned int *pui32Idle,
						  void *Util_Ex);

static void mtk_common_cal_gpu_utilization_ex(unsigned int *pui32Loading,
					      unsigned int *pui32Block,
					      unsigned int *pui32Idle,
					      void *Util_Ex)
{
	struct GpuUtilization_Ex *util_ex = Util_Ex;
	unsigned int util = mtk_common_gpu_util_get();

	if (pui32Loading)
		*pui32Loading = util;
	if (pui32Idle)
		*pui32Idle = 100u - util;
	/* pui32Block belongs to GED's own state machine, leave it alone. */

	if (util_ex) {
		util_ex->util_active = util;
		util_ex->util_3d = util;
		util_ex->util_ta = 0;
		util_ex->util_compute = 0;
	}
}

void mtk_common_gpu_util_init(void)
{
	ged_dvfs_cal_gpu_utilization_ex_fp = mtk_common_cal_gpu_utilization_ex;
	pr_info("@%s: GED gpu util callback registered\n", __func__);
}

void mtk_common_gpu_util_term(void)
{
	ged_dvfs_cal_gpu_utilization_ex_fp = NULL;
}
