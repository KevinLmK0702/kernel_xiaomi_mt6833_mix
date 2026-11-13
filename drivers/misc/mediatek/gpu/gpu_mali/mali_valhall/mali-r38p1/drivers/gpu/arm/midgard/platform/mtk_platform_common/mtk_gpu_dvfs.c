// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2026 Mix-Kernel
 *
 * Standalone MTK GPU DVFS governor.
 *
 * GED ships a DVFS governor but it never gets to run on this product: its
 * decision loop is gated on KPI frame data reported by the MTK composer,
 * which does not exist on an AOSP based ROM, and the fallback loading based
 * path depends on a state machine we cannot drive. Nothing else calls
 * mt_gpufreq_target() either, so the GPU simply stays at whatever OPP the
 * gpufreq default left behind and never scales.
 *
 * This governor is deliberately self contained:
 *   input  : kbase PM metrics (metrics.values.time_busy, updated on every
 *            job submit/complete event)
 *   output : mtk_common_gpufreq_commit()
 *
 * The OPP is chosen in kbase_platform_dvfs_event(), which kbase's own DVFS
 * timer calls every kbdev->pm.dvfs_period milliseconds. This file owns the
 * policy; GED's DVFS engine is deliberately left unregistered.
 *
 * The timer callback runs in softirq context and the commit path takes a
 * mutex, so the two are split: the timer decides and parks the OPP index,
 * a dedicated work item does the actual commit. Sampling for the GED
 * readers runs from its own delayed work. Nothing in the timer path may
 * sleep.
 */

#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/workqueue.h>

#include <mali_kbase.h>
#include <backend/gpu/mali_kbase_pm_defs.h>
#include <platform/mtk_platform_common.h>

#include <mtk_gpufreq.h>
#include <ged_kpi.h>

/* Sampling period. 50 ms keeps overhead negligible and is fast enough to
 * follow UI bursts. */
#define MTK_GOV_PERIOD_MS	50

/* Ignore samples shorter than this, the busy/idle ratio is too noisy. */
#define MTK_GOV_MIN_WINDOW_NS	(40ULL * 1000 * 1000)

/* time_busy is counted in units of (1 << 8) ns. */
#define MTK_GOV_TICK_SHIFT	8

#define MTK_GOV_EMA_WEIGHT	4

static struct delayed_work mtk_gov_work;
static struct work_struct mtk_gov_commit_work;
static bool mtk_gov_started;

static DEFINE_SPINLOCK(mtk_gov_lock);
static u32 mtk_gov_last_busy;
static ktime_t mtk_gov_last_ts;
static bool mtk_gov_have_last;
static unsigned int mtk_gov_util;	/* EMA smoothed, published for readers */

static unsigned int mtk_gov_avg;

/* Last OPP index we successfully committed. Handed back to the power
 * management code so that a GPU power on restores a sane frequency instead
 * of always jumping to the fastest OPP. */
static int mtk_gov_last_idx = -1;

/* Last OPP index the timer asked for, and the request the commit worker
 * has not picked up yet. The timer runs in softirq context and cannot
 * take the commit path's mutex, so it only parks the decision here. */
static int mtk_gov_req_idx = -1;
static int mtk_gov_pending_idx = -1;
static int mtk_gov_pending_prev;
static unsigned int mtk_gov_pending_load;
static unsigned int mtk_gov_pending_busy;
static int mtk_gov_pending_level;

/* GED KPI (frame timing) sampling state. */
static bool mtk_gov_kpi_alive;
static unsigned int mtk_gov_kpi_fps_prev;
static unsigned int mtk_gov_kpi_gt_prev;
static ktime_t mtk_gov_kpi_ts;

/* A frame based load older than this counts as "nothing is being drawn".
 *
 * GED refreshes gx_fps / gx_gpu_time_avg only once the accumulated frame
 * timestamps reach one second (GED_KPI_SEC_DIVIDER in ged_kpi.c), so the
 * values legitimately stay constant for a whole second. The window has to be
 * comfortably larger than that, otherwise every second is classified as
 * stale and the load keeps collapsing to zero. It also has to stay finite:
 * when frames stop arriving GED never refreshes these counters again, so the
 * last value would otherwise stick forever.
 */
#define MTK_GOV_KPI_STALE_MS	3000

static unsigned int mtk_gov_enable = 1;
module_param_named(mtk_gov_enable, mtk_gov_enable, uint, 0644);
MODULE_PARM_DESC(mtk_gov_enable, "Enable the standalone MTK GPU DVFS governor");

static unsigned int mtk_gov_verbose;
module_param_named(mtk_gov_verbose, mtk_gov_verbose, uint, 0644);
MODULE_PARM_DESC(mtk_gov_verbose, "Log every frequency decision");

/* kbase updates time_busy from job events; the ratio against the real wall
 * clock is the load. time_idle is deliberately not used as a denominator:
 * when the GPU goes idle kbase stops updating it, which would freeze the
 * reading at its last value.
 */
static unsigned int mtk_gov_sample(struct kbase_device *kbdev)
{
	unsigned long flags;
	u32 busy_now, d_ticks;
	u64 d_busy_ns, d_wall_ns;
	ktime_t now;
	unsigned int util;

	now = ktime_get_raw();

	spin_lock_irqsave(&mtk_gov_lock, flags);

	if (!kbdev) {
		mtk_gov_util = 0;
		spin_unlock_irqrestore(&mtk_gov_lock, flags);
		return 0;
	}

	busy_now = kbdev->pm.backend.metrics.values.time_busy;
	/* u32 subtraction stays correct across counter wrap */
	d_ticks = busy_now - mtk_gov_last_busy;

	if (!mtk_gov_have_last) {
		mtk_gov_have_last = true;
		util = 0;
	} else {
		d_wall_ns = (u64)ktime_to_ns(ktime_sub(now, mtk_gov_last_ts));
		if (d_wall_ns < MTK_GOV_MIN_WINDOW_NS) {
			util = mtk_gov_util;
		} else {
			d_busy_ns = (u64)d_ticks << MTK_GOV_TICK_SHIFT;
			util = (d_busy_ns >= d_wall_ns) ? 100u :
			       (unsigned int)((100ULL * d_busy_ns) / d_wall_ns);
			mtk_gov_avg = (mtk_gov_avg * (MTK_GOV_EMA_WEIGHT - 1) +
				       util) / MTK_GOV_EMA_WEIGHT;
			util = mtk_gov_avg;
		}
	}

	mtk_gov_last_busy = busy_now;
	mtk_gov_last_ts = now;
	mtk_gov_util = util;

	spin_unlock_irqrestore(&mtk_gov_lock, flags);

	return util;
}

/* Frame aligned load, in percent: gpu_time(ns) * fps / 1e9.
 *
 * This is a far better estimate than the kbase busy ratio. kbase counts a
 * sample period as busy whenever the GPU had any job queued, which for a
 * Vulkan compositor is nearly always true, so the busy ratio saturates at
 * ~100% while the shader cores are only a third occupied.
 *
 * Returns false when no KPI data is available at all (e.g. an older ROM), in
 * which case the caller falls back to the busy ratio.
 */
static bool mtk_gov_load_kpi(unsigned int *load)
{
	unsigned int fps = ged_kpi_get_cur_fps();
	unsigned int gt = ged_kpi_get_cur_avg_gpu_time();
	ktime_t now = ktime_get_raw();
	u64 prod;

	if (fps && gt) {
		mtk_gov_kpi_alive = true;
		if (fps != mtk_gov_kpi_fps_prev || gt != mtk_gov_kpi_gt_prev) {
			mtk_gov_kpi_fps_prev = fps;
			mtk_gov_kpi_gt_prev = gt;
			mtk_gov_kpi_ts = now;
		}
	}

	if (!mtk_gov_kpi_alive)
		return false;

	/*
	 * No new frame for a while means GED is not producing KPI at all for
	 * what is on screen. Report the signal as unusable instead of zero: the
	 * caller then falls back to the kbase busy ratio, which still tracks the
	 * frames GED does not see (system UI animations have no GED client).
	 * Returning zero here used to pin the policy to the slowest level while
	 * the GPU was demonstrably busy, and the boost path then fought the load
	 * path for the same OPP.
	 */
	if (ktime_to_ms(ktime_sub(now, mtk_gov_kpi_ts)) > MTK_GOV_KPI_STALE_MS)
		return false;

	prod = (u64)gt * fps;
	*load = (prod >= 1000000000ULL) ? 100u :
		(unsigned int)(prod / 10000000ULL);
	return true;
}

static void mtk_gov_publish(unsigned int load)
{
	unsigned long flags;

	spin_lock_irqsave(&mtk_gov_lock, flags);
	mtk_gov_util = load;
	spin_unlock_irqrestore(&mtk_gov_lock, flags);
}

/*
 * Sampling work.
 *
 * This no longer decides anything: the OPP is chosen in
 * kbase_platform_dvfs_event(), which kbase calls from its own DVFS timer.
 * The work item only keeps the cached utilisation up to date for the GED
 * readers (/sys/kernel/ged/hal/gpu_utilization) and keeps running whether or
 * not any frame is being submitted.
 */
static void mtk_gov_work_fn(struct work_struct *work)
{
        struct kbase_device *kbdev = mtk_common_get_kbdev();
        unsigned int util, load;

        util = mtk_gov_sample(kbdev);
        if (!mtk_gov_load_kpi(&load))
                load = util;
        mtk_gov_publish(load);

        if (mtk_gov_started)
                schedule_delayed_work(&mtk_gov_work,
                                      msecs_to_jiffies(MTK_GOV_PERIOD_MS));
}

/* ------------------------------------------------------------------- */
/* OPP policy, driven by the kbase DVFS timer                          */
/* ------------------------------------------------------------------- */

/*
 * Every knob below is a module parameter so the behaviour can be tuned on the
 * device without rebuilding. The defaults follow a port of this same design on
 * another MTK GPU, but the numbers depend on the workload and the panel and
 * are not universal.
 */

/* Level thresholds, in percent, high to low. Level 0 is the fastest OPP. */
static int mtk_dvfs_thr0 = 70;
static int mtk_dvfs_thr1 = 50;
static int mtk_dvfs_thr2 = 30;
static int mtk_dvfs_thr3 = 15;
module_param_named(mtk_dvfs_thr0, mtk_dvfs_thr0, int, 0644);
module_param_named(mtk_dvfs_thr1, mtk_dvfs_thr1, int, 0644);
module_param_named(mtk_dvfs_thr2, mtk_dvfs_thr2, int, 0644);
module_param_named(mtk_dvfs_thr3, mtk_dvfs_thr3, int, 0644);

/*
 * Consecutive samples that must agree before the level changes. At the 20 ms
 * sampling period the defaults mean 40 ms to speed up and 600 ms to slow
 * down, so a short spike raises the OPP at once while a brief idle period
 * does not drop it.
 */
static int mtk_dvfs_up_confirm = 2;
static int mtk_dvfs_down_confirm = 30;
module_param_named(mtk_dvfs_up_confirm, mtk_dvfs_up_confirm, int, 0644);
module_param_named(mtk_dvfs_down_confirm, mtk_dvfs_down_confirm, int, 0644);

/*
 * 1 = use the GED KPI frame load (gpu_time * fps) as the policy input,
 * 0 = use the kbase busy ratio.
 *
 * The KPI load is the default because it is the only signal that carries a
 * usable magnitude. kbase clamps its per-window active counter to the window
 * length:
 *
 *     gpu_active_counter = MIN(gpu_active_counter, ns_time);
 *     time_busy += gpu_active_counter;
 *
 * so a window in which any slot was continuously active reads 100%, no matter
 * how little of the shader cores was actually occupied. On a Vulkan
 * compositor that is the normal case, which is why the busy ratio parks at
 * 90-100% and a governor fed with it sits at the fastest OPP.
 *
 * The KPI drawback is the update rate: gx_fps / gx_gpu_time_avg refresh once
 * per second and freeze entirely when no frames are submitted. The stale
 * timeout in mtk_gov_load_kpi() and the step limit below are what keep that
 * staircase from turning into a sawtooth.
 */
static unsigned int mtk_dvfs_use_kpi = 1;
module_param_named(mtk_dvfs_use_kpi, mtk_dvfs_use_kpi, uint, 0644);

/*
 * How much of the OPP table the four steps may use, in percent. 100 spreads
 * the levels from index 0 to the slowest OPP; lower values keep the whole
 * range closer to the fast end, which can help on a table whose slowest OPP
 * is unusably slow for compositing.
 */
static int mtk_dvfs_level_span = 100;
module_param_named(mtk_dvfs_level_span, mtk_dvfs_level_span, int, 0644);

/*
 * Minimum time to stay on a level before another change is allowed. This
 * caps the switching rate regardless of how fast or how noisy the input
 * signal is, which matters because FPSGO can step the KPI load by tens of
 * percent in a single update.
 */
static int mtk_dvfs_hold_ms = 200;
module_param_named(mtk_dvfs_hold_ms, mtk_dvfs_hold_ms, int, 0644);

/*
 * Exponential smoothing weight for the utilisation input.
 *
 * One 20 ms sample window only covers one or two frames at 90 Hz, so the
 * raw busy ratio is bimodal: a window that happens to sit across a frame
 * reads close to 100%, the next one close to 0%. Feeding that straight
 * into the level lookup makes the OPP slam between the two ends roughly
 * once per frame. The weight is the effective window in samples, so 4
 * means about 80 ms. 1 disables smoothing.
 */
static int mtk_dvfs_ema = 4;
module_param_named(mtk_dvfs_ema, mtk_dvfs_ema, int, 0644);

/*
 * Maximum number of levels a single decision may move. With the default
 * of 1 and a 200 ms hold time a full swing from the slowest to the
 * fastest level takes at least 800 ms, which turns what would be a hard
 * 390 <-> 1068 MHz toggle into a ramp. 0 removes the limit.
 */
static int mtk_dvfs_step_limit = 1;
module_param_named(mtk_dvfs_step_limit, mtk_dvfs_step_limit, int, 0644);

/*
 * Busy ratio floor.
 *
 * Two signals are available and they fail in opposite directions.
 *
 * The KPI load (gpu_time * fps) has the right magnitude but only refreshes
 * once GED has accumulated a full second of frame timestamps, and it does not
 * cover scenes GED has no client for at all - system UI animations being the
 * obvious case.
 *
 * kbase's own busy ratio reacts within one 20 ms window but is close to
 * binary: a window either sits over a frame or it does not, so it cannot size
 * the load.
 *
 * So use the busy ratio only as a floor. While its smoothed value stays above
 * mtk_dvfs_boost_util the load is raised to mtk_dvfs_boost_load, which pushes
 * the policy towards the fast end without letting it bypass the step limit.
 * An earlier revision jumped straight to a fixed level instead, and because
 * the raw ratio alternates between 0 and 100 that jump was undone by the load
 * driven path a moment later, producing a 0 <-> 11 (1068 / 955 MHz) flap.
 *
 * Set mtk_dvfs_boost_util to 0 to run on the KPI load alone.
 */
static int mtk_dvfs_boost_util = 80;
module_param_named(mtk_dvfs_boost_util, mtk_dvfs_boost_util, int, 0644);
MODULE_PARM_DESC(mtk_dvfs_boost_util,
                 "Smoothed kbase busy ratio, in percent, that floors the load (0 = off)");

/*
 * Smoothing weight for the busy ratio.
 *
 * One 20 ms window covers one or two frames at 60 Hz, so the raw ratio
 * alternates between 0 and 100 and a single window is not evidence of
 * anything. Averaging over a few windows keeps the reaction time short while
 * requiring the work to be sustained.
 */
static int mtk_dvfs_boost_ema = 8;
module_param_named(mtk_dvfs_boost_ema, mtk_dvfs_boost_ema, int, 0644);
MODULE_PARM_DESC(mtk_dvfs_boost_ema, "Busy ratio smoothing weight, in samples");

/* Load, in percent, that the floor applies while the busy ratio stays high. */
static int mtk_dvfs_boost_load = 70;
module_param_named(mtk_dvfs_boost_load, mtk_dvfs_boost_load, int, 0644);
MODULE_PARM_DESC(mtk_dvfs_boost_load, "Load forced while the busy ratio stays high");


/*
 * Commit worker.
 *
 * kbase calls kbase_platform_dvfs_event() straight from its DVFS hrtimer,
 * that is, from softirq context, while mtk_common_gpufreq_commit() takes a
 * mutex. Sleeping there schedules while atomic and panics:
 *
 *   __schedule_bug <- __schedule <- __mutex_lock <- mutex_lock
 *   <- kbase_platform_dvfs_event <- kbase_pm_get_dvfs_action
 *   <- dvfs_callback <- __hrtimer_run_queues
 *
 * It reproduces easily when the timer happens to fire while another thread
 * sits inside mt_gpufreq_target():
 *
 *   mt_gpufreq_target -> ged_log_buf_print2 -> (irq) -> dvfs_callback
 *
 * The policy still runs in the timer, only the commit is handed over to a
 * worker, which also lets repeated decisions coalesce into one commit.
 */
static void mtk_gov_commit_work_fn(struct work_struct *work)
{
        unsigned long flags;
        int idx, prev, level;
        unsigned int load, busy;

        spin_lock_irqsave(&mtk_gov_lock, flags);
        idx = mtk_gov_pending_idx;
        prev = mtk_gov_pending_prev;
        load = mtk_gov_pending_load;
        busy = mtk_gov_pending_busy;
        level = mtk_gov_pending_level;
        mtk_gov_pending_idx = -1;
        spin_unlock_irqrestore(&mtk_gov_lock, flags);

        if (idx < 0)
                return;

        if (mtk_common_gpufreq_commit(idx) == 0) {
                mtk_common_gpu_note_idx(idx);
                if (mtk_gov_verbose)
                        pr_info("mtk_gov: idx %d->%d load=%u busy=%u lvl=%d\n",
                                prev, idx, load, busy, level);
        }
}

static int mtk_dvfs_level_of(unsigned int load)
{
        if (load >= mtk_dvfs_thr0)
                return 0;
        if (load >= mtk_dvfs_thr1)
                return 1;
        if (load >= mtk_dvfs_thr2)
                return 2;
        if (load >= mtk_dvfs_thr3)
                return 3;
        return 4;
}

/*
 * Called by the kbase DVFS timer every kbdev->pm.dvfs_period milliseconds
 * (20 ms by default, tunable through /sys/class/misc/mali0/device/dvfs_period).
 *
 * The signature must match the !MALI_USE_CSF prototype declared in
 * backend/gpu/mali_kbase_pm_internal.h.
 */
int kbase_platform_dvfs_event(struct kbase_device *kbdev, u32 utilisation,
                              u32 util_gl_share, u32 util_cl_share[2])
{
        static int cur_level = -1;
        static int up_cnt, down_cnt;
        static ktime_t last_change;
        static unsigned int ema;
        static bool ema_started;
        static unsigned int boost_ema;
        static bool boost_ema_started;
        unsigned int load;
        ktime_t now;
        bool dwell_ok;
        int num, bottom, level, target, ceiling, prev, step;

        (void)util_gl_share;
        (void)util_cl_share;

        /* mtk_gov_started also guarantees the commit work is initialised. */
        if (!mtk_gov_started || !mtk_gov_enable || !kbdev ||
            !mtk_common_pm_is_mfg_active())
                return 0;

        if (mtk_dvfs_boost_ema > 1) {
                if (!boost_ema_started) {
                        boost_ema = utilisation;
                        boost_ema_started = true;
                } else {
                        boost_ema = (boost_ema *
                                     (unsigned int)(mtk_dvfs_boost_ema - 1) +
                                     utilisation) /
                                    (unsigned int)mtk_dvfs_boost_ema;
                }
        } else {
                boost_ema = utilisation;
        }

        if (mtk_dvfs_use_kpi) {
                if (!mtk_gov_load_kpi(&load))
                        load = utilisation;
        } else if (mtk_gov_started) {
                load = mtk_common_gpu_util_get();
        } else {
                load = utilisation;
        }

        if (mtk_dvfs_ema > 1) {
                if (!ema_started) {
                        ema = load;
                        ema_started = true;
                } else {
                        ema = (ema * (unsigned int)(mtk_dvfs_ema - 1) + load) /
                              (unsigned int)mtk_dvfs_ema;
                }
                load = ema;
        }

        /*
         * Floor the load with the busy ratio. Deliberately applied after the
         * load smoothing so the floor keeps its full reaction speed, and
         * deliberately expressed as a load rather than as a level so it goes
         * through the same step/dwell pipeline as everything else.
         */
        if (mtk_dvfs_boost_util > 0 && mtk_dvfs_boost_load > 0 &&
            boost_ema >= (unsigned int)mtk_dvfs_boost_util &&
            load < (unsigned int)mtk_dvfs_boost_load)
                load = (unsigned int)mtk_dvfs_boost_load;

        num = (int)mt_gpufreq_get_dvfs_table_num();
        if (num < 2)
                return 0;

        bottom = num - 1;
        level = mtk_dvfs_level_of(load);

        /*
         * Start from the slowest level so the first decision is always a
         * commit and the GPU never stays at whatever OPP the boot flow left
         * behind.
         */
        if (cur_level < 0) {
                cur_level = 4;
                last_change = ktime_get_raw();
        }

        now = ktime_get_raw();

        dwell_ok = ktime_to_ms(ktime_sub(now, last_change)) >=
                   max(mtk_dvfs_hold_ms, 0);
        step = (mtk_dvfs_step_limit > 0) ? mtk_dvfs_step_limit : 4;

        if (level < cur_level) {
                down_cnt = 0;
                /*
                 * Speeding up deliberately ignores the dwell time: a scene
                 * that needs more GPU time cannot wait, and the asymmetry is
                 * what keeps the OPP from hunting. Slowing down is the
                 * direction that needs damping, and it keeps both the dwell
                 * and the longer confirm count.
                 */
                if (++up_cnt >= max(mtk_dvfs_up_confirm, 1)) {
                        up_cnt = 0;
                        cur_level -= min(cur_level - level, step);
                        last_change = now;
                }
        } else if (level > cur_level) {
                up_cnt = 0;
                if (++down_cnt >= max(mtk_dvfs_down_confirm, 1) && dwell_ok) {
                        down_cnt = 0;
                        cur_level += min(level - cur_level, step);
                        last_change = now;
                }
        } else {
                up_cnt = 0;
                down_cnt = 0;
        }

        if (mtk_dvfs_level_span > 0 && mtk_dvfs_level_span < 100)
                target = cur_level * bottom * mtk_dvfs_level_span / 400;
        else
                target = cur_level * bottom / 4;

        if (target < 0)
                target = 0;
        if (target > bottom)
                target = bottom;

        /*
         * Thermal / power ceiling. Index 0 is the fastest OPP, so the ceiling
         * is a lower bound on the index.
         */
        ceiling = (int)mt_gpufreq_get_cur_ceiling_idx();
        if (ceiling > 0 && ceiling < target)
                target = ceiling;

        /*
         * Only park the decision here. Compare against the last requested
         * index rather than the last committed one, because the worker may
         * not have run yet and the timer would otherwise queue the same
         * commit every 20 ms.
         */
        if (target != mtk_gov_req_idx) {
                unsigned long flags;

                mtk_gov_req_idx = target;
                spin_lock_irqsave(&mtk_gov_lock, flags);
                mtk_gov_pending_idx = target;
                mtk_gov_pending_prev = mtk_common_gpu_last_idx();
                mtk_gov_pending_load = load;
                mtk_gov_pending_busy = boost_ema;
                mtk_gov_pending_level = cur_level;
                spin_unlock_irqrestore(&mtk_gov_lock, flags);
                schedule_work(&mtk_gov_commit_work);
        }

        return 1;
}

void mtk_common_gpu_dvfs_init(void)
{
	int num, i;

	if (mtk_gov_started)
		return;

	num = (int)mt_gpufreq_get_dvfs_table_num();
	pr_info("@%s: standalone governor, %d OPPs\n", __func__, num);

	for (i = 0; i < num && i < 64; i++)
		pr_debug("@%s: opp[%2d] = %u kHz / %u uV\n", __func__, i,
			mt_gpufreq_get_freq_by_idx(i),
			mt_gpufreq_get_volt_by_idx(i));

	mtk_gov_avg = 0;
	mtk_gov_util = 0;
	mtk_gov_have_last = false;
	mtk_gov_last_idx = -1;
	mtk_gov_req_idx = -1;
	mtk_gov_pending_idx = -1;
	mtk_gov_kpi_alive = false;

	INIT_DELAYED_WORK(&mtk_gov_work, mtk_gov_work_fn);
	INIT_WORK(&mtk_gov_commit_work, mtk_gov_commit_work_fn);
	mtk_gov_started = true;
	schedule_delayed_work(&mtk_gov_work,
			      msecs_to_jiffies(MTK_GOV_PERIOD_MS));
}

void mtk_common_gpu_dvfs_term(void)
{
	if (!mtk_gov_started)
		return;

	mtk_gov_started = false;
	cancel_delayed_work_sync(&mtk_gov_work);
	cancel_work_sync(&mtk_gov_commit_work);
}

/* Latest smoothed utilisation, for the GED side readers. */
unsigned int mtk_common_gpu_util_get(void)
{
	unsigned long flags;
	unsigned int v;

	spin_lock_irqsave(&mtk_gov_lock, flags);
	v = mtk_gov_util;
	spin_unlock_irqrestore(&mtk_gov_lock, flags);

	return v;
}

/* Last OPP index committed by the governor, or -1 if it never committed.
 * Used by the power management callback to restore a sensible frequency on
 * GPU power on. */
int mtk_common_gpu_last_idx(void)
{
	return mtk_gov_last_idx;
}

/* Record an OPP index that somebody else committed, so the power management
 * restore path brings the GPU back up at the frequency that is actually in
 * use instead of the fastest OPP. */
void mtk_common_gpu_note_idx(int opp_idx)
{
	if (opp_idx >= 0)
		mtk_gov_last_idx = opp_idx;
}
