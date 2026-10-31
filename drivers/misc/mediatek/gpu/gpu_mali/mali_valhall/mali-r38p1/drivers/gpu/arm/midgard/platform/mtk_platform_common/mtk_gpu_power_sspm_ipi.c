// SPDX-License-Identifier: GPL-2.0
/*
 * Minimal stubs for the MTK GPU power-model SSPM interface.
 *
 * The full implementation (platform/mtk_platform_common/mtk_gpu_power_sspm_ipi.c
 * in the r32p1 tree) depends on r32-specific kbase vinstr APIs that are not
 * present in r38p1. The power model is only used for profiling (SWPM/LTR), so
 * no-op stubs are sufficient to bring up the driver.
 */

#include "mtk_gpu_power_sspm_ipi.h"

int MTKGPUPower_model_init(void)
{
	return 0;
}

void MTKGPUPower_model_sspm_enable(void)
{
}

void MTKGPUPower_model_start(unsigned int interval_ns)
{
	(void)interval_ns;
}

void MTKGPUPower_model_start_swpm(unsigned int interval_ns)
{
	(void)interval_ns;
}

void MTKGPUPower_model_stop(void)
{
}

void MTKGPUPower_model_suspend(void)
{
}

void MTKGPUPower_model_resume(void)
{
}
