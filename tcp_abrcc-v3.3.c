// SPDX-License-Identifier: GPL-2.0
/*
 * ABRCC v3.3: Brutal loss compensation + CTCP delay window + BBR-style filters
 *             + CUBIC probing + Veno loss discrimination + Westwood+ BWE + BBRv3 headroom
 *             + GECKO start burst + SALAMANDER ladder probe + HYBRIDIZATION low-speed probe
 *
 * Must be installed on the "data sender" end. Requires fq qdisc (or internal kernel pacing).
 *
 * v3.3 (vs v3.2) - v3.2 state machine untouched except where listed:
 *   FIXES
 *   1. CHIMERA BBR DRAIN really drains: the old code did r = max(r, ..) in every phase so the 0.75 gain
 *      never lowered anything. Phase 1 now caps the rate at 0.75 x bw for one round.
 *   2. CUBIC congestion event order: the loss level (ceiling), the Westwood+ cut and the CUBIC epoch are
 *      now all taken from the rate that was really on the wire (old), NOT from the rate already grown in
 *      this round (the old cut could even end above the old rate). Ceiling is set first, the cut second,
 *      and the ceiling does not regrow in the same round.
 *   3. Multi-flow minimum rate can no longer exceed the server budget: the floor is capped at the equal
 *      share target/n (hard budget wins over ABRCC_MIN_RATE and over a manual min_mbps); a flow that is
 *      not yet counted as active counts itself when sharing / flooring (n = nact + 1).
 *   4. lim_bps is u64 (u32 overflowed at 34 Gbit/s, target_mbps may be 100000). dr_bps is u64 too, the
 *      windowed-max bandwidth filter stores 64-byte units (u32 covers > 270 GB/s), cwnd math no longer
 *      overflows u64 for large rate x srtt x gain.
 *   NEW
 *   5. GECKO (default ON): first gecko_s (5) seconds after the handshake the flow runs at its full budget,
 *      loss ignored, and the best delivery rate is recorded. After that the rate is cut to
 *      max(peak/2, gecko_min_mbps (8)) (clamped to [floor, budget]); a path that never reached 8 Mbit/s is
 *      not cut below what it delivered. Loss/queue history of the burst is wiped.
 *   6. SALAMANDER (default ON, needs sal_on=1): ladder driven by the best delivery ever seen (gecko result
 *      included). best <30 Mbit/s: salamander OFF, HYBRIDIZATION runs instead. 30..50 -> goal 50, 50..100
 *      -> 100, 100..150 -> 150, 150..200 -> 200, >=200 done. A sprint starts only when the goal has not
 *      been reached for 10 min, then every 6-7 min, max 3 chances (refilled when a rung is reached), about
 *      30 s, linear ramp base->goal inside it plus the hybrid max(CTCP, CUBIC, Westwood+, BBR). Severe loss
 *      rise / queue delay / overload for ~2 RTT = failure: rate goes back to the pre-probe rate and is
 *      pinned there for 150-210 s. Every attempt is stored in a 3-entry record ring and logged; none of
 *      the existing reset paths (reprobe, heal, PROBE_RTT, fail_cnt, floor relax) touches this state.
 *   7. HYBRIDIZATION (part of sal_on): only while best <30 Mbit/s. Every 6 min (max 3 times, success or
 *      not) a ~20 s probe: CTCP step x4 whatever the loss, loss back-off suppressed, CUBIC + Westwood+ +
 *      BBR bw*gain candidates joined (max). Fails (rollback) only on severe queue delay / overload.
 *   CHIMERA stays in the code, default OFF (chimera=0).
 *
 * v3.2 notes (CUBIC from 0.2% loss, Veno verdict, Westwood+ cut, CHIMERA, manual min cap 15 Mbit/s) and
 * v3.1 notes (jitter handling, time based hold, budget-capped compensation) are unchanged.
 *
 * States:
 *   ST_CTCP : probes rate with CTCP delay window ticked by RTT (+ CUBIC curve from 0.2% loss)
 *   ST_HIGH : Brutal style, rate pulled toward (fair-share) target, bounded by learned ceil
 *
 * Usage: modprobe tcp_abrcc target_mbps=200 min_mbps=5
 *        sysctl -w net.core.default_qdisc=fq net.ipv4.tcp_congestion_control=abrcc
 *        Parameters: /sys/module/tcp_abrcc/parameters/
 */
#include <linux/module.h>
#include <linux/version.h>
#include <linux/math64.h>
#include <linux/slab.h>
#include <linux/random.h>
#include <linux/win_minmax.h>
#include <net/tcp.h>

/* cong_control signature changed starting from kernel 6.9; override with -DABRCC_NEW_CC_API=0/1 if backported */
#ifndef ABRCC_NEW_CC_API
# if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
#  define ABRCC_NEW_CC_API 1
# else
#  define ABRCC_NEW_CC_API 0
# endif
#endif

/* random number in [0, n) */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 2, 0)
# define abrcc_rand(n) get_random_u32_below(n)
#else
# define abrcc_rand(n) prandom_u32_max(n)
#endif

#define ABRCC_MIN_SAMPLES 100
#define ABRCC_MIN_CWND    4
#define ABRCC_MIN_RATE    62500ULL   /* 0.5 Mbit/s in bytes/s, absolute lower bound (below the budget share) */
#define ABRCC_BW_WIN      10         /* bandwidth max-filter window, in rounds */
#define ABRCC_BW_SHIFT    6          /* bw filter unit = 64 bytes/s, u32 covers > 270 GB/s */
#define ABRCC_BPS_CAP     (1ULL << 40) /* sanity cap of one delivery-rate sample, bytes/s */
#define ABRCC_HEAL_MS     5000       /* min time of stable HIGH before fail_cnt heals by one */
#define ABRCC_MIN_CAP_MBPS 15        /* manual min_mbps is limited to this total, always effective */
#define ABRCC_WMAX_CAP    (1ULL << 22) /* CUBIC window cap in packets */
#define ABRCC_CUB_T_CAP   300000     /* CUBIC time cap, ms */
#define ABRCC_SAL_RECS    3          /* salamander record ring = maximum chances */
#define ABRCC_SAL_OK_MS   1000       /* delivery must hold at the goal this long = success */
#define ABRCC_SAL_RETRY_MS 30000     /* retry delay after an aborted (not failed) probe */
#define ABRCC_HYB_CAP_MBPS 50        /* hybridization never probes above this rate */

enum { MODE_CTCP, MODE_BAND, MODE_HYBRID };
enum { ST_CTCP, ST_HIGH };
enum { SAL_IDLE, SAL_RUN };
enum { SAL_SPRINT, SAL_HYB };
enum { SAL_R_RUN, SAL_R_OK, SAL_R_LOSS, SAL_R_RTT, SAL_R_OVL, SAL_R_TIME, SAL_R_ABORT };

static const char * const abrcc_sal_kind_s[] = { "sprint", "hybrid" };
static const char * const abrcc_sal_res_s[] = {
	"running", "ok", "fail-loss", "fail-rtt", "fail-overload", "timeout", "abort"
};

static unsigned int target_mbps     = 200;  /* Server total maximum bandwidth (wire rate) */
static unsigned int min_mbps        = 0;    /* Manual total minimum, always effective, <= 15 (0 = automatic ladder) */
static unsigned int min_relax       = 0;    /* Allow floor to relax under persistent overload (default off) */
static unsigned int hold_drop_mbps  = 50;   /* HYBRID enters HIGH when CTCP reaches target-50 (scaled by share) */
static unsigned int loss_lo_bp      = 30;   /* 0.3%: below this = pure CTCP, no Brutal */
static unsigned int loss_hi_bp      = 100;  /* 1% */
static unsigned int gamma_pkts      = 30;   /* CTCP gamma */
static unsigned int cwnd_gain       = 20;   /* /10 */
static unsigned int q_tol_pct       = 25;   /* Queue tolerance: % of base_rtt */
static unsigned int q_tol_min_ms    = 10;
static unsigned int jit_pct         = 30;   /* Jitter threshold: % of base_rtt */
static unsigned int jit_min_ms      = 10;
static unsigned int overload_pct    = 60;   /* delivery < sending * 60% => overload */
static unsigned int max_comp_bp     = 5000; /* Loss compensation upper bound, 5000 = 50% */
static unsigned int fair_share      = 1;    /* Share target among active flows */
static unsigned int fair_idle_ms    = 2000; /* Flow idle longer than this leaves the share pool */
static unsigned int reprobe_s       = 20;   /* Force CTCP re-probe of HIGH flows every N s (0 = off) */
static unsigned int reprobe_rounds  = 8;    /* Rounds HIGH is blocked after a re-probe starts */
static unsigned int hold_rounds     = 8;    /* Base rounds CTCP must run before re-entering HIGH */
static unsigned int hold_min_ms     = 200;  /* Time floor of one hold unit (RTT-independent) */
static unsigned int hold_max_s      = 30;   /* Upper bound of the exponential hold */
static unsigned int prtt_s          = 10;   /* min_rtt refresh interval (0 = off, 60 s expiry kept) */
static unsigned int prtt_ms         = 200;  /* PROBE_RTT-lite duration */
static unsigned int ext_loss_bp     = 3000; /* Extreme congestion loss threshold (30%) */
/* v3.2 */
static unsigned int cubic_on        = 1;    /* CUBIC probing + Veno verdict + Westwood+ cut (0 = v3.1 behaviour) */
static unsigned int cub_loss_bp     = 20;   /* 0.2%: CUBIC joins CTCP from this loss (capped at loss_lo_bp) */
static unsigned int cub_beta        = 70;   /* CUBIC / Westwood+ cut floor, % of rate (50..87) */
static unsigned int veno_beta       = 3;    /* Veno backlog threshold, packets */
static unsigned int veno_pct        = 8;    /* Stable-path queue tolerance, % of base_rtt (min 2 ms) */
static unsigned int chimera         = 0;    /* Hybrid "all protocols probe" mode, DEFAULT OFF */
static unsigned int chimera_loss_bp = 1000; /* Peak loss that arms CHIMERA (10%) */
static unsigned int chimera_rounds  = 16;   /* Rounds of sustained random loss before CHIMERA starts */
/* v3.3 gecko */
static unsigned int gecko_on        = 1;    /* Start burst, default ON */
static unsigned int gecko_s         = 5;    /* Burst length after the handshake */
static unsigned int gecko_min_mbps  = 8;    /* Start rate after the burst: max(peak/2, this) */
/* v3.3 salamander + hybridization */
static unsigned int sal_on          = 1;    /* Ladder probe + low-speed hybridization, default ON */
static unsigned int sal_goal_mbps   = 200;  /* Top rung of the ladder (also capped by target / share) */
static unsigned int sal_off_mbps    = 30;   /* best below this: salamander off, hybridization on */
static unsigned int sal_idle_s      = 600;  /* Goal not reached for this long => a sprint may start */
static unsigned int sal_gap_min_s   = 360;  /* Sprint gap 6..7 min */
static unsigned int sal_gap_max_s   = 420;
static unsigned int sal_dur_s       = 30;   /* Sprint length */
static unsigned int sal_ramp_s      = 6;    /* Linear ramp base -> goal inside the sprint */
static unsigned int sal_tries       = 3;    /* Chances per window (max 3 = record ring size) */
static unsigned int sal_loss_bp     = 200;  /* Failure: loss rose by this much over the start level */
static unsigned int sal_rtt_pct     = 50;   /* Failure: queue delay >= this % of base_rtt (min q_tol_min_ms) */
static unsigned int sal_cool_min_s  = 150;  /* After a failure the rate is pinned to the pre-probe rate: 2:30 .. */
static unsigned int sal_cool_max_s  = 210;  /* .. 3:30 */
static unsigned int sal_reach_pct   = 95;   /* delivery >= this % of the goal counts as reached */
static unsigned int sal_hyb_gap_s   = 360;  /* Hybridization every 6 min */
static unsigned int sal_hyb_s       = 20;   /* Hybridization probe length */
static unsigned int sal_hyb_tries   = 3;    /* Max 3 hybridization probes, success or not */
static unsigned int sal_log         = 1;    /* Log every gecko / salamander / hybridization result */

module_param(target_mbps, uint, 0644);
module_param(min_mbps, uint, 0644);
module_param(min_relax, uint, 0644);
module_param(hold_drop_mbps, uint, 0644);
module_param(loss_lo_bp, uint, 0644);
module_param(loss_hi_bp, uint, 0644);
module_param(gamma_pkts, uint, 0644);
module_param(cwnd_gain, uint, 0644);
module_param(q_tol_pct, uint, 0644);
module_param(q_tol_min_ms, uint, 0644);
module_param(jit_pct, uint, 0644);
module_param(jit_min_ms, uint, 0644);
module_param(overload_pct, uint, 0644);
module_param(max_comp_bp, uint, 0644);
module_param(fair_share, uint, 0644);
module_param(fair_idle_ms, uint, 0644);
module_param(reprobe_s, uint, 0644);
module_param(reprobe_rounds, uint, 0644);
module_param(hold_rounds, uint, 0644);
module_param(hold_min_ms, uint, 0644);
module_param(hold_max_s, uint, 0644);
module_param(prtt_s, uint, 0644);
module_param(prtt_ms, uint, 0644);
module_param(ext_loss_bp, uint, 0644);
module_param(cubic_on, uint, 0644);
module_param(cub_loss_bp, uint, 0644);
module_param(cub_beta, uint, 0644);
module_param(veno_beta, uint, 0644);
module_param(veno_pct, uint, 0644);
module_param(chimera, uint, 0644);
module_param(chimera_loss_bp, uint, 0644);
module_param(chimera_rounds, uint, 0644);
module_param(gecko_on, uint, 0644);
module_param(gecko_s, uint, 0644);
module_param(gecko_min_mbps, uint, 0644);
module_param(sal_on, uint, 0644);
module_param(sal_goal_mbps, uint, 0644);
module_param(sal_off_mbps, uint, 0644);
module_param(sal_idle_s, uint, 0644);
module_param(sal_gap_min_s, uint, 0644);
module_param(sal_gap_max_s, uint, 0644);
module_param(sal_dur_s, uint, 0644);
module_param(sal_ramp_s, uint, 0644);
module_param(sal_tries, uint, 0644);
module_param(sal_loss_bp, uint, 0644);
module_param(sal_rtt_pct, uint, 0644);
module_param(sal_cool_min_s, uint, 0644);
module_param(sal_cool_max_s, uint, 0644);
module_param(sal_reach_pct, uint, 0644);
module_param(sal_hyb_gap_s, uint, 0644);
module_param(sal_hyb_s, uint, 0644);
module_param(sal_hyb_tries, uint, 0644);
module_param(sal_log, uint, 0644);

/* Server-wide (module-wide) fairness bookkeeping */
static atomic_t abrcc_nact = ATOMIC_INIT(0);          /* active (busy) flows */
static atomic_t abrcc_nlim = ATOMIC_INIT(0);          /* flows limited by their own path */
static atomic64_t abrcc_lim_bps = ATOMIC64_INIT(0);   /* send-rate sum of limited flows, bytes/s */

/* Read once per ACK and range-validated so runtime parameter changes are safe */
struct abrcc_cfg {
	u32 tgt, hold, lo, hi, gamma, gain;
	u32 qpct, qmin_us, jpct, jmin_us, ovl, comp;
	u32 minm, relax, fair, idle_ms;
	u32 rp_ms, rp_rounds, hrounds, hmin_ms, hmax_ms;
	u32 prtt_int_ms, prtt_ms, ext;
	u32 cub, cublo, cubbeta, vbeta, vpct;
	u32 chim, chimbp, chimr;
	/* v3.3 */
	u32 gk, gkms, gkmin;
	u32 sal, salgoal, saloff, salidle_ms, salgmin_ms, salgmax_ms, saldur_ms, salramp_ms, saltries;
	u32 salloss, salrtt, salcmin_ms, salcmax_ms, salreach;
	u32 hybgap_ms, hybdur_ms, hybtries, sallog;
};

static void abrcc_cfg_load(struct abrcc_cfg *c)
{
	u32 d;

	c->tgt   = clamp_t(u32, READ_ONCE(target_mbps), 1, 100000);
	d        = READ_ONCE(hold_drop_mbps);
	c->hold  = d < c->tgt ? c->tgt - d : c->tgt / 2;
	c->hi    = clamp_t(u32, READ_ONCE(loss_hi_bp), 10, 5000);
	c->lo    = clamp_t(u32, READ_ONCE(loss_lo_bp), 1, c->hi);
	c->gamma = clamp_t(u32, READ_ONCE(gamma_pkts), 1, 10000);
	c->gain  = clamp_t(u32, READ_ONCE(cwnd_gain), 10, 100);
	c->qpct  = clamp_t(u32, READ_ONCE(q_tol_pct), 5, 400);
	c->qmin_us = clamp_t(u32, READ_ONCE(q_tol_min_ms), 1, 1000) * 1000;
	c->jpct  = clamp_t(u32, READ_ONCE(jit_pct), 5, 400);
	c->jmin_us = clamp_t(u32, READ_ONCE(jit_min_ms), 1, 1000) * 1000;
	c->ovl   = clamp_t(u32, READ_ONCE(overload_pct), 10, 95);
	c->comp  = clamp_t(u32, READ_ONCE(max_comp_bp), 0, 7500);
	/* manual minimum: hard-limited to 15 Mbit/s total, never above the target */
	c->minm  = min_t(u32, min_t(u32, READ_ONCE(min_mbps), ABRCC_MIN_CAP_MBPS), c->tgt);
	c->relax = !!READ_ONCE(min_relax);
	c->fair  = !!READ_ONCE(fair_share);
	c->idle_ms = clamp_t(u32, READ_ONCE(fair_idle_ms), 200, 60000);
	c->rp_ms = clamp_t(u32, READ_ONCE(reprobe_s), 0, 3600) * 1000;
	c->rp_rounds = clamp_t(u32, READ_ONCE(reprobe_rounds), 1, 64);
	c->hrounds = clamp_t(u32, READ_ONCE(hold_rounds), 1, 64);
	c->hmin_ms = clamp_t(u32, READ_ONCE(hold_min_ms), 10, 10000);
	c->hmax_ms = max_t(u32, clamp_t(u32, READ_ONCE(hold_max_s), 1, 600) * 1000, c->hmin_ms);
	c->prtt_int_ms = clamp_t(u32, READ_ONCE(prtt_s), 0, 3600) * 1000;
	c->prtt_ms = clamp_t(u32, READ_ONCE(prtt_ms), 50, 2000);
	c->ext   = clamp_t(u32, READ_ONCE(ext_loss_bp), c->hi, 9000);
	c->cub   = !!READ_ONCE(cubic_on);
	c->cublo = clamp_t(u32, READ_ONCE(cub_loss_bp), 1, c->hi);
	c->cubbeta = clamp_t(u32, READ_ONCE(cub_beta), 50, 87);
	c->vbeta = clamp_t(u32, READ_ONCE(veno_beta), 1, 1000);
	c->vpct  = clamp_t(u32, READ_ONCE(veno_pct), 1, 100);
	c->chim  = !!READ_ONCE(chimera);
	c->chimbp = clamp_t(u32, READ_ONCE(chimera_loss_bp), c->hi, 9000);
	c->chimr = clamp_t(u32, READ_ONCE(chimera_rounds), 4, 64);

	c->gk    = !!READ_ONCE(gecko_on);
	c->gkms  = clamp_t(u32, READ_ONCE(gecko_s), 1, 30) * 1000;
	c->gkmin = clamp_t(u32, READ_ONCE(gecko_min_mbps), 1, c->tgt);

	c->sal   = !!READ_ONCE(sal_on);
	c->salgoal = clamp_t(u32, READ_ONCE(sal_goal_mbps), 2, c->tgt);
	c->saloff  = clamp_t(u32, READ_ONCE(sal_off_mbps), 1, c->salgoal);
	c->salidle_ms = clamp_t(u32, READ_ONCE(sal_idle_s), 10, 86400) * 1000;
	c->salgmin_ms = clamp_t(u32, READ_ONCE(sal_gap_min_s), 10, 86400) * 1000;
	c->salgmax_ms = max_t(u32, clamp_t(u32, READ_ONCE(sal_gap_max_s), 10, 86400) * 1000, c->salgmin_ms);
	c->saldur_ms  = clamp_t(u32, READ_ONCE(sal_dur_s), 5, 300) * 1000;
	c->salramp_ms = min_t(u32, clamp_t(u32, READ_ONCE(sal_ramp_s), 1, 120) * 1000, c->saldur_ms / 2);
	c->saltries   = clamp_t(u32, READ_ONCE(sal_tries), 1, ABRCC_SAL_RECS);
	c->salloss    = clamp_t(u32, READ_ONCE(sal_loss_bp), 10, 5000);
	c->salrtt     = clamp_t(u32, READ_ONCE(sal_rtt_pct), 10, 400);
	c->salcmin_ms = clamp_t(u32, READ_ONCE(sal_cool_min_s), 1, 3600) * 1000;
	c->salcmax_ms = max_t(u32, clamp_t(u32, READ_ONCE(sal_cool_max_s), 1, 3600) * 1000, c->salcmin_ms);
	c->salreach   = clamp_t(u32, READ_ONCE(sal_reach_pct), 50, 100);
	c->hybgap_ms  = clamp_t(u32, READ_ONCE(sal_hyb_gap_s), 10, 86400) * 1000;
	c->hybdur_ms  = clamp_t(u32, READ_ONCE(sal_hyb_s), 5, 300) * 1000;
	c->hybtries   = clamp_t(u32, READ_ONCE(sal_hyb_tries), 1, ABRCC_SAL_RECS);
	c->sallog     = !!READ_ONCE(sal_log);
}

/* One salamander / hybridization attempt (kept in a ring of ABRCC_SAL_RECS) */
struct abrcc_sal_rec {
	u32 t_ms;        /* start (tcp clock, ms) */
	u32 dur_ms;      /* real duration */
	u32 base_mbps;   /* rate before the probe = rollback rate */
	u32 goal_mbps;
	u32 peak_mbps;   /* best delivery rate seen during the probe */
	u32 loss_bp;     /* worst smoothed loss */
	u32 q_ms;        /* worst queue delay */
	u8 kind, res;
};

/* Per-connection state, kzalloc'ed (too big for icsk_ca_priv); the priv area only holds the pointer */
struct abrcc {
	u64 rate_bps;         /* Current base rate (without loss compensation), bytes/s */
	u64 ceil_bps;         /* Learned ceiling after overload, slowly recovering */
	u64 west_bps;         /* Westwood+ bandwidth estimate (per-round delivery, 7/8 filter), bytes/s */
	u64 lim_bps;          /* Contribution to abrcc_lim_bps while lim == 1 (u64 since v3.3) */
	u64 dr_bps;           /* Delivery rate EWMA, bytes/s (u64 since v3.3) */
	u64 gk_peak_bps;      /* gecko: best delivery rate of the start burst */
	u64 sal_best_bps;     /* best delivery rate ever seen (gecko included): drives the ladder */
	u64 sal_base_bps;     /* rate right before the running / last probe */
	u64 sal_goal_bps;     /* target of the running probe */
	struct minmax bw;     /* Windowed max of delivery rate (bandwidth estimate), 64 B/s units */
	u32 start_ms;
	u32 last_round_ms;
	u32 last_backoff_ms;
	u32 last_busy_ms;
	u32 reprobe_stamp;    /* Time HIGH was entered (or CTCP started) */
	u32 hold_until_ms;    /* HIGH may not be entered before this time */
	u32 heal_stamp;       /* Last time fail_cnt healed / HIGH started */
	u32 min_rtt_us, min_rtt_stamp, pr_min_us, pr_end_ms;
	u32 rnd_min_us;       /* Minimum RTT sample of the current round (U32_MAX = none) */
	u32 last_rtt_us;      /* Previous RTT sample, for jitter */
	u32 round_cnt;
	u32 loss_sec;
	u32 cur_acked, cur_lost;
	u32 hist_acked, hist_lost;
	u32 loss_s;           /* Smoothed loss rate (basis points) */
	u32 loss_pk;          /* Slowly decaying peak of the instantaneous loss (basis points) */
	u32 jitter_us;
	u32 wmax_pkts;        /* CUBIC: window (packets) at the last congestion event */
	u32 cub_epoch_ms;     /* CUBIC: epoch start */
	u32 cub_k_ms;         /* CUBIC: time to reach wmax again */
	u32 bwe_stamp_ms;     /* Westwood+: start of the current measuring round */
	u32 rnd_acked;        /* Westwood+: packets acked since bwe_stamp_ms */
	/* v3.3 salamander + hybridization (outside every other reset path) */
	u32 sal_rung;         /* last ladder rung seen (Mbit/s, 0 = hybrid tier, U32_MAX = finished) */
	u32 sal_ref_ms;       /* last time a rung was reached (or gecko end) */
	u32 sal_last_ms;      /* end of the last probe */
	u32 sal_gap_ms;       /* random sprint gap (6..7 min) */
	u32 sal_start_ms;
	u32 sal_ok_ms, sal_bad_ms, sal_alim_ms;   /* 0 = not set */
	u32 sal_cool_until_ms;
	u32 sal_loss0;        /* loss at probe start */
	struct abrcc_sal_rec sal_rec[ABRCC_SAL_RECS];
	u8 mode, state;
	u8 jit_rounds, bad_rounds, ovl_rounds, ext_rounds, grace;
	u8 floor_pct, floor_bad, floor_ok;
	u8 sat_rounds;        /* Consecutive rounds where delivery < 90% of sending: bw estimate is a real capacity */
	u8 fail_cnt, good_high;
	u8 comp_pct;          /* Congestion factor of loss compensation, 0..100 */
	u8 app_lim, active, lim, in_prtt, extreme;
	u8 cub_on, cub_valid; /* CUBIC probing active / epoch valid */
	u8 rand_pct;          /* Veno verdict: 100 = random (router/WiFi) loss, 0 = congestion loss */
	u8 chim, chim_cnt;    /* CHIMERA active / arming counter */
	u8 gecko;             /* start burst running */
	u8 sal_st, sal_kind, sal_used, hyb_used, sal_cool, sal_cur, sal_ri;
};

/* Context of one round handed to the salamander code */
struct abrcc_rctx {
	u64 B, F;
	u32 mss, srtt_us, now_ms, base_us, q_us;
	bool overload, rtt_ok;
};

static inline struct abrcc *abrcc_get(struct sock *sk)
{
	struct abrcc **p = inet_csk_ca(sk);

	return *p;
}

static inline u64 mbps_to_bps(u32 m)
{
	return (u64)m * 125000ULL;
}

static inline u32 bps_to_mbps(u64 b)
{
	return (u32)min_t(u64, div_u64(b, 125000ULL), U32_MAX);
}

/* Bandwidth filter in 64-byte units (u32 filter, no overflow for 100 Gbit/s targets) */
static inline u64 abrcc_bw(const struct abrcc *a)
{
	return (u64)minmax_get(&a->bw) << ABRCC_BW_SHIFT;
}

static inline u32 abrcc_stamp(u32 now_ms)
{
	return now_ms ? now_ms : 1;
}

static u32 abrcc_rand_ms(u32 lo, u32 hi)
{
	return hi > lo ? lo + abrcc_rand(hi - lo + 1) : lo;
}

/* ---------- fair share bookkeeping ---------- */

static void abrcc_set_lim(struct abrcc *a, bool lim, u64 bps)
{
	if (a->lim && !lim) {
		atomic_dec(&abrcc_nlim);
		atomic64_sub(a->lim_bps, &abrcc_lim_bps);
		a->lim_bps = 0;
		a->lim = 0;
	} else if (!a->lim && lim) {
		atomic_inc(&abrcc_nlim);
		atomic64_add(bps, &abrcc_lim_bps);
		a->lim_bps = bps;
		a->lim = 1;
	} else if (a->lim) {
		atomic64_add((s64)bps - (s64)a->lim_bps, &abrcc_lim_bps);
		a->lim_bps = bps;
	}
}

static void abrcc_set_active(struct abrcc *a, bool on)
{
	if (on == !!a->active)
		return;
	a->active = on;
	if (on) {
		atomic_inc(&abrcc_nact);
	} else {
		atomic_dec(&abrcc_nact);
		abrcc_set_lim(a, false, 0);
	}
}

/* Flows competing for the budget: an inactive flow that is about to send counts itself (v3.3) */
static u32 abrcc_nflows(const struct abrcc *a)
{
	return max_t(s32, atomic_read(&abrcc_nact) + (a->active ? 0 : 1), 1);
}

/* Equal share of the server budget: nothing, the floor included, may go above it (v3.3) */
static u64 abrcc_eq_bps(const struct abrcc *a, const struct abrcc_cfg *c)
{
	u32 n = c->fair ? abrcc_nflows(a) : 1;

	return max_t(u64, div_u64(mbps_to_bps(c->tgt), n), 1);
}

/* Absolute minimum rate: 0.5 Mbit/s, but never above the equal share (many flows, tiny target) */
static u64 abrcc_min_rate(const struct abrcc *a, const struct abrcc_cfg *c)
{
	return min_t(u64, ABRCC_MIN_RATE, abrcc_eq_bps(a, c));
}

/* Per-flow bandwidth bound: equal share, leftover of path-limited flows redistributed */
static u64 abrcc_share_bps(const struct abrcc *a, const struct abrcc_cfg *c)
{
	u64 T = mbps_to_bps(c->tgt);
	s64 n, nl, lim, rest;
	u64 sh, eq;

	if (!c->fair)
		return T;
	n   = abrcc_nflows(a);
	nl  = atomic_read(&abrcc_nlim);
	lim = atomic64_read(&abrcc_lim_bps);
	if (a->lim) {                       /* exclude ourselves: we compete for the unlimited pool */
		nl--;
		lim -= a->lim_bps;
	}
	if (nl < 0)
		nl = 0;
	if (lim < 0)
		lim = 0;
	if (nl >= n)
		nl = n - 1;
	eq   = div64_u64(T, n);
	rest = (s64)T - lim;
	sh   = rest > 0 ? div64_u64(rest, n - nl) : eq;
	sh   = max(sh, eq);
	return min(sh, T);
}

/* ---------- floor (minimum rate) ---------- */

static u32 abrcc_floor_mbps(u32 tgt, u32 t)
{
	u32 f0  = tgt >= 200 ? 10 : (tgt >= 100 ? 7 : 5);
	u32 f20 = tgt > 500 ? 40 : (tgt >= 200 ? 20 : (tgt >= 100 ? 15 : 10));
	u32 f60 = min_t(u32, max_t(u32, tgt / 4, f20), 50);
	u32 f;

	if (t < 20)
		f = f0;
	else if (t < 60)
		f = f20 + (f60 - f20) * (t - 20) / 40;
	else
		f = f60;
	return min_t(u32, f, tgt);
}

/*
 * Floor: manual min_mbps if set (always effective, <= 15 Mbit/s total), else the ladder. Divided among
 * active flows, relaxed 100%->50%->25% under persistent overload (only if min_relax / automatic mode).
 * In automatic mode the floor never stays above 75% of a proven capacity.
 * v3.3: the result is hard-capped at the equal share target/n, so the sum of all floors <= target.
 */
static u64 abrcc_floor(const struct abrcc *a, const struct abrcc_cfg *c, u32 now_ms)
{
	u64 f, cap = abrcc_eq_bps(a, c);
	u32 n = 1;

	if (c->minm)
		f = mbps_to_bps(c->minm);
	else
		f = mbps_to_bps(abrcc_floor_mbps(c->tgt, (now_ms - a->start_ms) / 1000));
	if (c->fair)
		n = abrcc_nflows(a);
	f = div_u64(f, n);
	f = f * a->floor_pct / 100;
	if (!c->minm && a->sat_rounds >= 3) {
		u64 bw = abrcc_bw(a);

		f = min_t(u64, f, max_t(u64, bw * 3 / 4, ABRCC_MIN_RATE));
	}
	f = max_t(u64, f, abrcc_min_rate(a, c));
	return min_t(u64, f, cap);
}

static inline u32 abrcc_base_rtt(const struct abrcc *a, u32 srtt_us)
{
	u32 b = a->min_rtt_us;

	return (!b || b > srtt_us) ? srtt_us : b;
}

/* ---------- loss window ---------- */

static void abrcc_loss_add(struct sock *sk, struct abrcc *a, u32 sec,
			   u32 acked, u32 lost)
{
	u32 gap = sec - a->loss_sec;

	if (gap) {
		if (gap >= 8) {
			a->hist_acked = a->hist_lost = 0;
		} else {
			a->hist_acked += a->cur_acked;
			a->hist_lost  += a->cur_lost;
			while (gap--) {
				a->hist_acked -= a->hist_acked / 4;
				a->hist_lost  -= a->hist_lost / 4;
			}
		}
		a->cur_acked = a->cur_lost = 0;
		a->loss_sec = sec;
	}
	a->cur_acked += acked;
	/* During RTO the entire window is marked lost; do not count it */
	if (inet_csk(sk)->icsk_ca_state != TCP_CA_Loss)
		a->cur_lost += lost;
}

static u32 abrcc_loss_inst(const struct abrcc *a)
{
	u64 ack  = (u64)a->hist_acked + a->cur_acked;
	u64 lost = (u64)a->hist_lost + a->cur_lost;

	if (ack + lost < ABRCC_MIN_SAMPLES)
		return 0;
	return (u32)div64_u64(lost * 10000ULL, ack + lost);
}

/* ---------- loss compensation (bounded by congestion factor and server budget) ---------- */

/* Compensation in basis points: off in pure CTCP, extreme congestion, PROBE_RTT; scaled by comp_pct */
static u32 abrcc_comp_bp(const struct abrcc *a, const struct abrcc_cfg *c)
{
	u32 comp;

	if (a->mode == MODE_CTCP || a->extreme || a->in_prtt)
		return 0;
	comp = min_t(u32, a->loss_s, c->comp);
	return comp * a->comp_pct / 100;
}

/* Wire rate = compensated rate, HARD capped by the flow's budget bound B (sum of B <= target) */
static u64 abrcc_send_bps(u64 r, u32 comp, u64 B)
{
	u64 send = div_u64(r * 10000ULL, 10000 - comp);

	return min_t(u64, send, B);
}

/* ---------- CUBIC (rate domain) + Westwood+ cut ---------- */

/* Integer cube root by binary search: largest m with m^3 <= x (x < 2^63) */
static u32 abrcc_cbrt(u64 x)
{
	u32 lo = 0, hi = 1U << 21, mid;

	while (lo < hi) {
		mid = lo + (hi - lo + 1) / 2;
		if ((u64)mid * mid * mid <= x)
			lo = mid;
		else
			hi = mid - 1;
	}
	return lo;
}

static inline u64 abrcc_rate_to_pkts(u64 r, u32 srtt_us, u32 mss)
{
	return min_t(u64, div64_u64(r * srtt_us, 1000000ULL * mss), ABRCC_WMAX_CAP);
}

/* Start a CUBIC epoch without a congestion event: plateau at the current window, then convex growth */
static void abrcc_cubic_start(struct abrcc *a, u64 r, u32 mss, u32 srtt_us, u32 now_ms)
{
	a->wmax_pkts = (u32)abrcc_rate_to_pkts(r, srtt_us, mss);
	a->cub_k_ms = 0;
	a->cub_epoch_ms = now_ms;
	a->cub_valid = 1;
}

/* Congestion event: remember wmax (fast convergence), K = cbrt((wmax - w_after_cut) / 0.4) */
static void abrcc_cubic_event(struct abrcc *a, const struct abrcc_cfg *c, u64 r, u64 r_new,
			      u32 mss, u32 srtt_us, u32 now_ms)
{
	u64 w  = abrcc_rate_to_pkts(r, srtt_us, mss);
	u64 wc = abrcc_rate_to_pkts(r_new, srtt_us, mss);
	u64 wm = w;

	if (a->cub_valid && w < a->wmax_pkts)
		wm = w * (100 + c->cubbeta) / 200;      /* fast convergence */
	a->wmax_pkts = (u32)wm;
	a->cub_k_ms = min_t(u32, abrcc_cbrt((wm > wc ? wm - wc : 0) * 2500000000ULL),
			    ABRCC_CUB_T_CAP);
	a->cub_epoch_ms = now_ms;
	a->cub_valid = 1;
}

/* W(t) = 0.4 * (t - K)^3 + wmax, evaluated one RTT ahead, converted back to a rate (bytes/s) */
static u64 abrcc_cubic_rate(const struct abrcc *a, u32 mss, u32 srtt_us, u32 now_ms)
{
	u64 t = (u64)(now_ms - a->cub_epoch_ms) + srtt_us / 1000;
	u64 k = a->cub_k_ms, d, off, w = a->wmax_pkts;

	if (t > ABRCC_CUB_T_CAP)
		t = ABRCC_CUB_T_CAP;
	if (t >= k) {
		d = t - k;
		off = div64_u64(d * d * d * 4ULL, 10000000000ULL);
		w += off;
	} else {
		d = k - t;
		off = div64_u64(d * d * d * 4ULL, 10000000000ULL);
		w = off >= w ? 0 : w - off;
	}
	return div64_u64(w * mss * 1000000ULL, srtt_us);
}

/*
 * Congestion-loss event (Westwood+ x CUBIC), v3.3 order, everything from the rate that was really on the
 * wire when the loss was seen (rl), never from this round's grown candidate:
 *   1. the ceiling remembers rl (BBRv3 inflight_hi) before anything is cut
 *   2. Westwood+ cut: BWE clamped to [cub_beta, 7/8] of rl
 *   3. CUBIC epoch from rl -> new rate
 * The caller must not regrow the ceiling in the same round.
 */
static u64 abrcc_loss_cut(struct abrcc *a, const struct abrcc_cfg *c, u64 rl, u32 mss,
			  u32 srtt_us, u32 now_ms, u64 F, u64 *ceil)
{
	u64 hi = rl * 7 / 8, lo = rl * c->cubbeta / 100, nr;

	if (lo > hi)
		lo = hi;
	nr = a->west_bps ? clamp_t(u64, a->west_bps, lo, hi) : hi;
	nr = max_t(u64, nr, F);
	*ceil = max_t(u64, min_t(u64, *ceil, rl), nr);
	abrcc_cubic_event(a, c, rl, nr, mss, srtt_us, now_ms);
	return nr;
}

/*
 * Hybrid candidate = MAX of CTCP result r, CUBIC curve, Westwood+ BWE*1.25 and BBR-style bw*g.
 * Used by CHIMERA, by the salamander sprint and by hybridization.
 */
static u64 abrcc_hybrid(struct abrcc *a, u64 r, u32 mss, u32 srtt_us, u32 now_ms, u32 g)
{
	u64 cand = r, bw = abrcc_bw(a);

	if (!a->cub_valid)
		abrcc_cubic_start(a, r, mss, srtt_us, now_ms);
	cand = max_t(u64, cand, abrcc_cubic_rate(a, mss, srtt_us, now_ms));
	if (a->west_bps)
		cand = max_t(u64, cand, a->west_bps + a->west_bps / 4);
	if (bw)
		cand = max_t(u64, cand, bw * g / 100);
	return cand;
}

/* ---------- hold / back-off (time based, independent of RTT) ---------- */

static inline bool abrcc_hold_active(const struct abrcc *a, u32 now_ms)
{
	return (s32)(a->hold_until_ms - now_ms) > 0;
}

static u32 abrcc_hold_ms(const struct abrcc_cfg *c, u32 srtt_us, u32 rounds, u8 shift)
{
	u64 ms = div_u64((u64)rounds * srtt_us, 1000);

	ms = max_t(u64, ms, c->hmin_ms);
	ms <<= shift;
	return (u32)min_t(u64, ms, c->hmax_ms);
}

static void abrcc_extend_hold(struct abrcc *a, u32 now_ms, u32 ms)
{
	u32 until = now_ms + ms;

	if (!abrcc_hold_active(a, now_ms) || (s32)(until - a->hold_until_ms) > 0)
		a->hold_until_ms = until;
}

/* Leave HIGH. On failure the hold time doubles each time (hold, 2x, 4x.. up to hold_max_s) */
static void abrcc_to_ctcp(struct abrcc *a, const struct abrcc_cfg *c, u32 now_ms,
			  u32 srtt_us, bool fail)
{
	a->state = ST_CTCP;
	a->reprobe_stamp = now_ms;
	a->heal_stamp = now_ms;
	a->bad_rounds = 0;
	a->good_high = 0;
	if (fail)
		a->fail_cnt = min_t(u8, a->fail_cnt + 1, 5);
	abrcc_extend_hold(a, now_ms, abrcc_hold_ms(c, srtt_us, c->hrounds, a->fail_cnt));
}

/* ---------- GECKO: start burst ---------- */

/*
 * End of the first gecko_s seconds: cut from the burst rate to max(peak/2, gecko_min_mbps), clamped to
 * [floor, budget]. A path that never reached gecko_min_mbps is not cut below what it delivered. The loss /
 * queue history of the burst is wiped, then the normal machinery (CTCP slow start from here) takes over.
 */
static void abrcc_gecko_end(struct sock *sk, struct abrcc *a, const struct abrcc_cfg *c, u32 now_ms)
{
	u64 T = mbps_to_bps(c->tgt);
	u64 F = abrcc_floor(a, c, now_ms);
	u64 B = max_t(u64, min_t(u64, abrcc_share_bps(a, c), T), F);
	u64 peak = a->gk_peak_bps, gmin = mbps_to_bps(c->gkmin), st;

	if (peak < gmin)
		st = max_t(u64, peak, F);               /* below 8 Mbit/s: do not cut */
	else
		st = max_t(u64, peak / 2, gmin);
	st = clamp_t(u64, st, F, B);

	a->gecko = 0;
	a->rate_bps = st;
	a->ceil_bps = B;
	a->cur_acked = a->cur_lost = a->hist_acked = a->hist_lost = 0;
	a->loss_s = a->loss_pk = 0;
	a->mode = MODE_CTCP;
	a->state = ST_CTCP;
	a->grace = 3;
	a->ovl_rounds = a->sat_rounds = a->ext_rounds = a->extreme = 0;
	a->comp_pct = 100;
	a->rand_pct = 50;
	a->cub_on = a->cub_valid = 0;
	a->jit_rounds = 0;
	a->rnd_acked = 0;
	a->bwe_stamp_ms = now_ms;
	a->rnd_min_us = U32_MAX;
	a->hold_until_ms = a->reprobe_stamp = a->heal_stamp = now_ms;
	a->sal_best_bps = max_t(u64, a->sal_best_bps, peak);
	a->sal_ref_ms = a->sal_last_ms = now_ms;
	if (c->sallog)
		pr_info_ratelimited("abrcc: gecko port %u->%u peak %u Mbit/s (goal %u %s) start %u Mbit/s\n",
				    sk->sk_num, ntohs(sk->sk_dport), bps_to_mbps(peak), c->salgoal,
				    peak * 100 >= mbps_to_bps(c->salgoal) * c->salreach ?
				    "reached" : "not reached", bps_to_mbps(st));
}

/* ---------- SALAMANDER (ladder sprint) + HYBRIDIZATION (low-speed probe) ---------- */

/*
 * Ladder rung from the best delivery ever seen (gecko result included), compared with a reach margin:
 *   < sal_off (30)  : 0  -> salamander off, hybridization
 *   < 50 -> 50, < 100 -> 100, < 150 -> 150, < 200 -> 200 (rungs capped at sal_goal)
 *   >= sal_goal     : U32_MAX -> finished
 */
static u32 abrcc_sal_rung(const struct abrcc *a, const struct abrcc_cfg *c)
{
	u32 em = (u32)min_t(u64, div_u64(a->sal_best_bps * 100, 125000ULL * c->salreach), U32_MAX);
	u32 g;

	if (em < c->saloff)
		return 0;
	if (em >= c->salgoal)
		return U32_MAX;
	g = em < 50 ? 50 : (em < 100 ? 100 : (em < 150 ? 150 : 200));
	return min_t(u32, g, c->salgoal);
}

static void abrcc_sal_end(struct sock *sk, struct abrcc *a, const struct abrcc_cfg *c, u8 res, u32 now_ms)
{
	struct abrcc_sal_rec *rec = &a->sal_rec[a->sal_cur];

	rec->res = res;
	rec->dur_ms = now_ms - rec->t_ms;
	a->sal_st = SAL_IDLE;
	a->sal_last_ms = now_ms;
	a->sal_gap_ms = abrcc_rand_ms(c->salgmin_ms, c->salgmax_ms);
	a->sal_ok_ms = a->sal_bad_ms = a->sal_alim_ms = 0;
	if (c->sallog)
		pr_info_ratelimited("abrcc: %s %s port %u->%u base %u goal %u peak %u Mbit/s loss %u bp queue %u ms dur %u ms used %u/%u hyb %u/%u\n",
				    abrcc_sal_kind_s[rec->kind], abrcc_sal_res_s[res], sk->sk_num,
				    ntohs(sk->sk_dport), rec->base_mbps, rec->goal_mbps, rec->peak_mbps,
				    rec->loss_bp, rec->q_ms, rec->dur_ms, a->sal_used, c->saltries,
				    a->hyb_used, c->hybtries);
}

/* One round of a running probe; returns the new rate */
static u64 abrcc_sal_run(struct sock *sk, struct abrcc *a, const struct abrcc_cfg *c,
			 const struct abrcc_rctx *x, u64 r, u64 old, u64 *ceil)
{
	struct abrcc_sal_rec *rec = &a->sal_rec[a->sal_cur];
	bool sprint = a->sal_kind == SAL_SPRINT, sevl, sevq, sevo;
	u32 now = x->now_ms, t = now - a->sal_start_ms;
	u32 dur = sprint ? c->saldur_ms : c->hybdur_ms;
	u32 need = max_t(u32, 200, x->srtt_us / 500);      /* a signal must last ~2 RTT, >= 200 ms */
	u64 base = a->sal_base_bps, goal = a->sal_goal_bps, sched, cand;
	u64 qthr = max_t(u64, (u64)x->base_us * c->salrtt / 100, c->qmin_us);
	u8 why;

	/* record */
	if (!a->app_lim && a->dr_bps)
		rec->peak_mbps = max_t(u32, rec->peak_mbps, bps_to_mbps(a->dr_bps));
	rec->loss_bp = max_t(u32, rec->loss_bp, a->loss_s);
	rec->q_ms = max_t(u32, rec->q_ms, x->q_us / 1000);

	/* abort (not a failure, chance refunded): flow idle or app-limited for long, nothing measurable */
	if (a->app_lim) {
		if (!a->sal_alim_ms)
			a->sal_alim_ms = abrcc_stamp(now);
	} else {
		a->sal_alim_ms = 0;
	}
	if (!a->active || (a->sal_alim_ms && now - a->sal_alim_ms >= 1000 + need)) {
		r = max_t(u64, base, x->F);
		if (sprint && a->sal_used)
			a->sal_used--;
		else if (!sprint && a->hyb_used)
			a->hyb_used--;
		abrcc_sal_end(sk, a, c, SAL_R_ABORT, now);
		a->sal_last_ms = now + ABRCC_SAL_RETRY_MS - (sprint ? a->sal_gap_ms : c->hybgap_ms);
		return r;
	}

	/* failure: severe loss rise (sprint only, hybridization ignores loss) / queue delay / overload */
	sevl = a->extreme || (sprint && (a->state == ST_HIGH ||
				a->loss_s >= a->sal_loss0 + c->salloss));
	sevq = x->q_us >= qthr;
	sevo = x->overload;
	if (sevl || sevq || sevo) {
		if (!a->sal_bad_ms)
			a->sal_bad_ms = abrcc_stamp(now);
		if (now - a->sal_bad_ms >= need) {
			why = sevl ? SAL_R_LOSS : (sevq ? SAL_R_RTT : SAL_R_OVL);
			r = max_t(u64, base, x->F);             /* back to the pre-probe rate */
			*ceil = min_t(u64, *ceil, r);
			if (sprint) {                           /* pinned there for 2:30 .. 3:30 */
				a->sal_cool = 1;
				a->sal_cool_until_ms = now + abrcc_rand_ms(c->salcmin_ms, c->salcmax_ms);
			}
			a->cub_valid = 0;
			abrcc_sal_end(sk, a, c, why, now);
			return r;
		}
	} else {
		a->sal_bad_ms = 0;
	}

	/* success: delivery holds at the goal (sprint) */
	if (sprint && !a->app_lim && a->dr_bps && a->dr_bps * 100 >= goal * c->salreach) {
		if (!a->sal_ok_ms)
			a->sal_ok_ms = abrcc_stamp(now);
		if (now - a->sal_ok_ms >= ABRCC_SAL_OK_MS) {
			*ceil = max_t(u64, *ceil, r);
			abrcc_sal_end(sk, a, c, SAL_R_OK, now);
			return r;
		}
	} else {
		a->sal_ok_ms = 0;
	}

	/* time is up without a severe signal: keep what the path proved, never go below the start rate */
	if (t >= dur) {
		r = max_t(u64, max_t(u64, base, x->F), min_t(u64, r, abrcc_bw(a)));
		*ceil = min_t(u64, *ceil, r);
		abrcc_sal_end(sk, a, c,
			      (!sprint && rec->peak_mbps >= c->saloff) ? SAL_R_OK : SAL_R_TIME, now);
		return r;
	}

	/* drive: sprint = linear ramp base->goal + hybrid max; hybridization = hybrid max only (CTCP x4 is in
	 * the CTCP branch, loss back-off is suppressed there) */
	cand = abrcc_hybrid(a, r, x->mss, x->srtt_us, now, 125);
	cand = min_t(u64, cand, old * 2);
	if (sprint) {
		u64 span = goal > base ? goal - base : 0;
		u32 el = min_t(u32, t, c->salramp_ms);

		sched = c->salramp_ms ? base + div64_u64(span * el, c->salramp_ms) : goal;
		cand = max_t(u64, sched, cand);
	}
	r = max_t(u64, r, min_t(u64, cand, goal));
	return r;
}

/* Called once per round (after the state machine, before negative feedback); returns the new rate */
static u64 abrcc_sal(struct sock *sk, struct abrcc *a, const struct abrcc_cfg *c,
		     const struct abrcc_rctx *x, u64 r, u64 old, u64 *ceil)
{
	struct abrcc_sal_rec *rec;
	u32 now = x->now_ms, rung, ref;
	u64 goal;
	bool hyb;

	if (!c->sal) {
		a->sal_st = SAL_IDLE;
		a->sal_cool = 0;
		return r;
	}

	if (!a->app_lim && a->dr_bps)
		a->sal_best_bps = max_t(u64, a->sal_best_bps, a->dr_bps);

	/* rung reached: the window restarts and the chances are refilled */
	rung = abrcc_sal_rung(a, c);
	if (rung != a->sal_rung) {
		if (rung > a->sal_rung) {
			a->sal_ref_ms = now;
			a->sal_used = 0;
		}
		a->sal_rung = rung;
	}

	if (a->sal_st == SAL_RUN)
		return abrcc_sal_run(sk, a, c, x, r, old, ceil);

	/* after a failed sprint the rate stays at the pre-probe rate */
	if (a->sal_cool) {
		if ((s32)(a->sal_cool_until_ms - now) > 0) {
			u64 cap = max_t(u64, a->sal_base_bps, x->F);

			*ceil = min_t(u64, *ceil, cap);
			return min_t(u64, r, cap);
		}
		a->sal_cool = 0;
	}

	if (rung == U32_MAX)                            /* top of the ladder already reached */
		return r;
	hyb = rung == 0;
	goal = hyb ? min_t(u64, mbps_to_bps(ABRCC_HYB_CAP_MBPS), x->B) :
		     min_t(u64, mbps_to_bps(rung), x->B);

	if (hyb ? a->hyb_used >= c->hybtries : a->sal_used >= c->saltries)
		return r;
	ref = (s32)(a->sal_last_ms - a->sal_ref_ms) > 0 ? a->sal_last_ms : a->sal_ref_ms;
	if ((s32)(now - (ref + (hyb ? c->hybgap_ms : a->sal_gap_ms))) < 0)
		return r;
	if (!hyb && now - a->sal_ref_ms < c->salidle_ms)  /* sprint: goal not reached for 10 min */
		return r;
	if (a->app_lim || !a->active || a->in_prtt || a->extreme || a->state != ST_CTCP)
		return r;
	if (!hyb && (a->mode != MODE_CTCP || !x->rtt_ok))   /* sprint only on a clean path */
		return r;
	if (old + old / 16 >= goal)                     /* no headroom */
		return r;

	/* start */
	a->sal_st = SAL_RUN;
	a->sal_kind = hyb ? SAL_HYB : SAL_SPRINT;
	a->sal_base_bps = old;
	a->sal_goal_bps = goal;
	a->sal_start_ms = now;
	a->sal_loss0 = a->loss_s;
	a->sal_ok_ms = a->sal_bad_ms = a->sal_alim_ms = 0;
	if (hyb)
		a->hyb_used++;
	else
		a->sal_used++;
	a->cub_valid = 0;                               /* fresh CUBIC epoch from the start rate */
	a->sal_cur = a->sal_ri;
	a->sal_ri = (a->sal_ri + 1) % ABRCC_SAL_RECS;
	rec = &a->sal_rec[a->sal_cur];
	memset(rec, 0, sizeof(*rec));
	rec->t_ms = now;
	rec->kind = a->sal_kind;
	rec->base_mbps = bps_to_mbps(old);
	rec->goal_mbps = bps_to_mbps(goal);
	rec->res = SAL_R_RUN;
	return r;
}

/* ---------- once per RTT ---------- */

static void abrcc_round(struct sock *sk, struct abrcc *a, const struct abrcc_cfg *c, u32 mss,
			u32 srtt_us, u32 now_ms)
{
	u64 T = mbps_to_bps(c->tgt);
	u64 F, B, H, Bg, r, ceil, old, nq;
	u32 base_us, tol, jthr, rtt_sig, comp, li, cublo, q_us, vtol;
	bool rtt_ok, jit_hi, delay_bad, cong, bad, reprobe, stable;
	bool overload = false, sat = false, sample = false, cut_now = false, hyb, salrun;
	struct abrcc_rctx x;

	a->round_cnt++;

	/* Idle flows leave the fair-share pool */
	if (a->active && now_ms - a->last_busy_ms > c->idle_ms)
		abrcc_set_active(a, false);

	if (a->gecko)                                   /* start burst: rate is forced in abrcc_main */
		return;

	/* PROBE_RTT-lite: refresh min_rtt on long connections */
	if (a->in_prtt) {
		if ((s32)(now_ms - a->pr_end_ms) < 0)
			return;                         /* rate is halved in abrcc_main */
		a->in_prtt = 0;
		if (a->pr_min_us != U32_MAX)
			a->min_rtt_us = a->pr_min_us;
		a->min_rtt_stamp = now_ms;
		a->rnd_min_us = U32_MAX;
		a->bwe_stamp_ms = now_ms;               /* Westwood+: drop the halved-rate interval */
		a->rnd_acked = 0;
	} else if (c->prtt_int_ms && now_ms - a->min_rtt_stamp > c->prtt_int_ms) {
		if (a->app_lim || !a->active) {
			a->min_rtt_stamp = now_ms;
		} else if (a->sal_st != SAL_RUN) {      /* a running probe finishes first */
			a->in_prtt = 1;
			a->pr_min_us = U32_MAX;
			a->pr_end_ms = now_ms + max_t(u32, c->prtt_ms, srtt_us / 1000);
			return;
		}
	} else if (!c->prtt_int_ms && now_ms - a->min_rtt_stamp > 60000) {
		a->min_rtt_us = srtt_us;                /* plain 60 s expiry when PROBE_RTT is off */
		a->min_rtt_stamp = now_ms;
	}

	/* Westwood+ bandwidth estimate: delivered bytes of the last round, low-pass filtered 7/8 */
	{
		u64 dt_us = (u64)(now_ms - a->bwe_stamp_ms) * 1000ULL;

		if (dt_us && !a->app_lim) {
			u64 bk = div64_u64((u64)a->rnd_acked * mss * 1000000ULL, dt_us);

			a->west_bps = a->west_bps ? (a->west_bps * 7 + bk) / 8 : bk;
		}
		a->rnd_acked = 0;
		a->bwe_stamp_ms = now_ms;
	}

	F = abrcc_floor(a, c, now_ms);
	B = max_t(u64, min_t(u64, abrcc_share_bps(a, c), T), F);
	H = div_u64(B * c->hold, c->tgt);
	r = clamp_t(u64, a->rate_bps, F, B);
	ceil = clamp_t(u64, a->ceil_bps, F, B);
	old = r;
	base_us = abrcc_base_rtt(a, srtt_us);
	tol  = max_t(u32, (u64)base_us * c->qpct / 100, c->qmin_us);
	jthr = max_t(u32, (u64)base_us * c->jpct / 100, c->jmin_us);
	jit_hi = a->jitter_us > jthr;
	hyb = a->sal_st == SAL_RUN && a->sal_kind == SAL_HYB;   /* hybridization: loss does not back off */

	/* Loss: smoothing + hysteresis. < lo => pure CTCP */
	li = abrcc_loss_inst(a);
	a->loss_pk = max_t(u32, li, a->loss_pk - a->loss_pk / 16);
	a->loss_s = (a->loss_s * 3 + li) / 4;
	if (a->loss_s > c->hi ||
	    (a->mode == MODE_HYBRID && a->loss_s > c->hi * 7 / 10))
		a->mode = MODE_HYBRID;
	else if (a->loss_s >= c->lo ||
		 (a->mode != MODE_CTCP && a->loss_s >= c->lo * 7 / 10))
		a->mode = MODE_BAND;
	else
		a->mode = MODE_CTCP;

	/* CUBIC probing window: from cub_loss_bp (default 0.2%) with 70% hysteresis */
	cublo = min_t(u32, c->cublo, c->lo);
	if (c->cub && (a->loss_s >= cublo || (a->cub_on && a->loss_s >= cublo * 7 / 10))) {
		if (!a->cub_on)
			a->cub_valid = 0;               /* fresh epoch on the rising edge */
		a->cub_on = 1;
	} else {
		a->cub_on = 0;
	}

	/* RTT jitter persistently large => srtt untrustworthy, use the per-round minimum RTT instead */
	if (jit_hi)
		a->jit_rounds = min_t(u8, a->jit_rounds + 1, 16);
	else if (a->jit_rounds)
		a->jit_rounds--;
	delay_bad = a->jit_rounds >= 3;

	/*
	 * Delay signal. Normal: srtt. High jitter: per-round min RTT (noise does not raise the minimum,
	 * a standing queue does). The delay-based logic therefore keeps working under jitter.
	 */
	rtt_sig = srtt_us;
	if (delay_bad && a->rnd_min_us != U32_MAX)
		rtt_sig = max_t(u32, a->rnd_min_us, base_us);
	a->rnd_min_us = U32_MAX;
	rtt_ok = rtt_sig <= base_us + tol;
	q_us = rtt_sig > base_us ? rtt_sig - base_us : 0;
	nq = div64_u64(r * q_us, 1000000ULL * mss);     /* Veno backlog N, packets */

	/* Delivery rate: overload (<ovl%), saturation (<90%) and bandwidth estimate */
	if (a->grace) {
		a->grace--;
	} else if (!a->app_lim && a->dr_bps) {
		sample = true;
		overload = a->dr_bps * 100 < r * c->ovl;
		sat = a->dr_bps * 100 < r * 90;
	}
	if (!a->app_lim && a->dr_bps)
		minmax_running_max(&a->bw, ABRCC_BW_WIN, a->round_cnt,
				   (u32)min_t(u64, a->dr_bps >> ABRCC_BW_SHIFT, U32_MAX));
	if (sat)
		a->sat_rounds = min_t(u8, a->sat_rounds + 1, 255);
	else if (sample && a->sat_rounds)
		a->sat_rounds--;

	/* Congestion evidence independent of random loss (valid under jitter too, see rtt_sig) */
	cong = overload || !rtt_ok;
	bad  = cong || (c->comp && a->loss_s > c->comp);

	/*
	 * Veno-style verdict (only while loss is present): random router/WiFi loss vs congestion loss.
	 * Stable path (low jitter, e.g. Ethernet): tight queue tolerance, so congestion shows early and a
	 * shallow-buffer drop with a delivery shortfall is still called congestion. Jittery path (WiFi):
	 * the normal tolerance, loss without queue growth stays "random".
	 */
	stable = !a->jit_rounds && a->jitter_us <= jthr / 3;
	vtol = stable ? max_t(u32, (u64)base_us * c->vpct / 100, 2000) : tol;
	if (a->cub_on) {
		u64 expd = div_u64(r * (10000 - min_t(u32, a->loss_s, 5000)), 10000);
		bool qgrow  = nq >= c->vbeta && q_us > vtol;
		bool lowdel = sample && a->dr_bps * 100 < expd * 85;
		bool cgst   = cong || qgrow || lowdel;
		int rp = a->rand_pct;

		rp += ((cgst ? 0 : 100) - rp) / 4;
		a->rand_pct = (u8)rp;
	}

	/* Compensation congestion factor: stop amplifying congestion loss, keep it for random loss */
	if (cong)
		a->comp_pct /= 2;
	else
		a->comp_pct = min_t(u8, a->comp_pct + 10, 100);

	/* Extreme congestion: huge loss together with overload / queue growth */
	if (a->loss_s >= c->ext && cong)
		a->ext_rounds = min_t(u8, a->ext_rounds + 1, 255);
	else if (a->ext_rounds)
		a->ext_rounds--;
	a->extreme = a->ext_rounds >= 2;

	/* CHIMERA arming (default off): sustained peak loss + random verdict + no congestion evidence */
	if (c->chim && a->loss_pk >= c->chimbp && a->rand_pct >= 60 && !cong && !a->extreme) {
		a->chim_cnt = min_t(u32, (u32)a->chim_cnt + 1, c->chimr + 8);
	} else if (!c->chim || a->extreme || cong || a->rand_pct <= 40) {
		a->chim_cnt = 0;
	} else if (a->chim_cnt) {
		a->chim_cnt--;                          /* peak faded: leave slowly */
	}
	a->chim = c->chim && a->chim_cnt >= c->chimr;

	comp = abrcc_comp_bp(a, c);
	Bg = max_t(u64, div_u64(B * (10000 - comp), 10000), F);   /* budget-aware HIGH target */

	/* Reprobe due: evaluated independently of jitter / app-limited so it cannot be starved */
	reprobe = a->state == ST_HIGH && c->rp_ms &&
		  now_ms - a->reprobe_stamp >= c->rp_ms;

	/* ----- state transitions (jitter never forces a state) ----- */
	if (a->state == ST_HIGH && a->mode != MODE_HYBRID) {
		abrcc_to_ctcp(a, c, now_ms, srtt_us, false);    /* loss dropped: back to classic CTCP */
	} else if (a->state == ST_HIGH && a->extreme) {
		abrcc_to_ctcp(a, c, now_ms, srtt_us, true);
	} else if (reprobe) {
		/* Long connection: old judgements may be stale, force a fresh CTCP probe */
		a->fail_cnt >>= 1;                              /* soften, never fully erase the back-off */
		abrcc_to_ctcp(a, c, now_ms, srtt_us, false);
		abrcc_extend_hold(a, now_ms, abrcc_hold_ms(c, srtt_us, c->rp_rounds, 0));
		a->floor_pct = 100;
		a->floor_bad = a->floor_ok = 0;
		a->ovl_rounds = 0;
	}

	if (a->state == ST_CTCP) {
		u64 win  = div64_u64(r * srtt_us, 1000000ULL * mss);
		u64 diff = div64_u64(r * (u64)(rtt_sig - min(base_us, rtt_sig)), 1000000ULL * mss);

		a->bad_rounds = 0;
		if (diff < c->gamma) {
			if (!a->app_lim) {
				u64 incr;

				win = min_t(u64, win, 1ULL << 20);
				incr = int_sqrt((unsigned long)win *
						int_sqrt((unsigned long)win)) / 8;
				incr = max_t(u64, incr, 1);
				if (a->mode == MODE_HYBRID || hyb)
					incr *= 4;      /* high loss / hybridization probe: CTCP x4 */
				r += div64_u64(incr * mss * 1000000ULL, srtt_us);

				/* CUBIC joins CTCP from ~0.2% loss: the larger target wins, +50%/round max */
				if (a->cub_on && rtt_ok) {
					u64 rcub, cap;

					if (!a->cub_valid)
						abrcc_cubic_start(a, r, mss, srtt_us, now_ms);
					rcub = abrcc_cubic_rate(a, mss, srtt_us, now_ms);
					cap  = (a->mode == MODE_HYBRID && a->rand_pct >= 60) ?
					       old * 2 : old + old / 2;
					if (a->mode != MODE_HYBRID)     /* < 1% loss: stay fair, honour the ceiling */
						rcub = min_t(u64, rcub, ceil);
					r = max_t(u64, r, min_t(u64, rcub, cap));
				}
			}
		} else {
			r = div64_u64(r * base_us, rtt_sig);
		}

		/* CHIMERA: CTCP, CUBIC, Westwood+ and BBR-style bw*gain probe together, follow the max */
		if (a->chim && !a->extreme && !a->app_lim) {
			u32 ph = a->round_cnt & 7;

			if (ph == 1) {
				/* BBR DRAIN (v3.3: really drains): 0.75 x bw for one round, whatever CTCP / CUBIC
				 * asked for. The old r = max(r, ..) in every phase could never lower anything. */
				u64 bw = abrcc_bw(a);

				r = min_t(u64, r, max_t(u64, (bw ? bw : old) * 3 / 4, F));
			} else {
				/* ph 0 = probe-up (1.25), others cruise (1.0) */
				u64 cand = abrcc_hybrid(a, r, mss, srtt_us, now_ms, ph == 0 ? 125 : 100);

				r = max_t(u64, r, min_t(u64, cand, old * 2));
			}
		}

		if (hyb) {
			/* hybridization probe: no loss-based back-off at all */
		} else if (a->cub_on && a->mode != MODE_HYBRID) {
			/* Veno verdict decides: random router/WiFi loss -> no back-off, congestion -> cut */
			if (now_ms - a->last_backoff_ms >= 1000) {
				if (a->rand_pct <= 40) {
					/* v3.3 order: ceiling, cut and CUBIC epoch all from the rate really sent */
					r = abrcc_loss_cut(a, c, old, mss, srtt_us, now_ms, F, &ceil);
					a->last_backoff_ms = now_ms;
					cut_now = true;
				} else if (a->rand_pct < 60 && a->mode == MODE_BAND) {
					r = r * 7 / 8;          /* undecided: v3.1 behaviour */
					a->last_backoff_ms = now_ms;
				}
			}
		} else if (a->mode == MODE_BAND && now_ms - a->last_backoff_ms >= 1000) {
			r = r * 7 / 8;
			a->last_backoff_ms = now_ms;
		}
		/* The ONLY way into HIGH: CTCP + high loss + reached H + hold expired */
		if (a->mode == MODE_HYBRID && r >= H && !abrcc_hold_active(a, now_ms) &&
		    !a->extreme) {
			a->state = ST_HIGH;
			a->reprobe_stamp = now_ms;
			a->heal_stamp = now_ms;
			a->bad_rounds = 0;
			a->good_high = 0;
		}
	} else {
		if (bad) {
			a->bad_rounds = min_t(u8, a->bad_rounds + 1, 255);
			a->good_high = 0;
		} else {
			a->bad_rounds = 0;
			if (a->good_high < 255)
				a->good_high++;
			/* Stable HIGH heals the back-off counter: >= 64 good rounds AND >= 5 s */
			if (a->good_high >= 64 && now_ms - a->heal_stamp >= ABRCC_HEAL_MS) {
				a->good_high = 0;
				a->heal_stamp = now_ms;
				if (a->fail_cnt)
					a->fail_cnt--;
			}
		}

		if (a->bad_rounds >= 3) {
			r = r * 7 / 8;
			abrcc_to_ctcp(a, c, now_ms, srtt_us, true);  /* fast fallback + exponential hold */
		} else if (!a->app_lim && !bad) {
			if (a->mode == MODE_HYBRID) {
				r = min_t(u64, Bg, ceil);       /* Brutal-style max out, within the budget */
			} else if (a->mode == MODE_BAND &&
				   now_ms - a->last_backoff_ms >= 1000) {
				r = r * 7 / 8;
				a->last_backoff_ms = now_ms;
			} else {
				r = min_t(u64, r + r / 2, ceil);
			}
		}
	}

	/* ----- v3.3 salamander + hybridization (own state, own timers, no existing reset touches it) ----- */
	x.B = B;
	x.F = F;
	x.mss = mss;
	x.srtt_us = srtt_us;
	x.now_ms = now_ms;
	x.base_us = base_us;
	x.q_us = q_us;
	x.overload = overload;
	x.rtt_ok = rtt_ok;
	r = abrcc_sal(sk, a, c, &x, r, old, &ceil);
	salrun = a->sal_st == SAL_RUN;

	/* ----- negative feedback / bandwidth estimate / ceiling learning ----- */
	if (overload) {
		if (a->ovl_rounds < 255)
			a->ovl_rounds++;
	} else {
		a->ovl_rounds = 0;
	}

	if (a->extreme) {
		r = min_t(u64, r, max_t(u64, a->dr_bps, F));
		ceil = r;
	} else if (a->ovl_rounds >= 3 && !salrun) {
		r = min_t(u64, r, max_t(u64, a->dr_bps + a->dr_bps / 4, F));
		ceil = r;
	} else if (a->sat_rounds >= 6 && !a->app_lim && !salrun) {
		/* Path proven capacity-limited: never run far above measured bandwidth */
		u64 bw = abrcc_bw(a);
		u64 cap = max_t(u64, bw + bw / 4, F);

		r = min_t(u64, r, cap);
		ceil = min_t(u64, ceil, cap);
	} else if (!a->app_lim && !overload && ceil < B && !cut_now) {
		ceil = min_t(u64, B, ceil + ceil / 16 + 1);
	}

	/* Floor relaxes during persistent overload, recovers once cleared */
	if (!a->app_lim && (!c->minm || c->relax)) {
		if (overload && r <= F + F / 8) {
			a->floor_ok = 0;
			if (++a->floor_bad >= 16) {
				a->floor_bad = 0;
				a->floor_pct = max_t(u8, a->floor_pct / 2, 25);
			}
		} else if (!overload) {
			a->floor_bad = 0;
			if (a->floor_pct < 100 && ++a->floor_ok >= 16) {
				a->floor_ok = 0;
				a->floor_pct = min_t(u8, a->floor_pct * 2, 100);
			}
		}
	}

	if (r > old + old / 4)
		a->grace = 3;
	F = abrcc_floor(a, c, now_ms);
	B = max_t(u64, B, F);
	a->rate_bps = clamp_t(u64, r, F, B);
	a->ceil_bps = ceil;

	/* Flows bottlenecked by their own path release the rest of their share to others.
	 * The contribution is the WIRE rate (with compensation), so the budget stays exact. */
	comp = abrcc_comp_bp(a, c);
	abrcc_set_lim(a, a->active && ceil * 100 < B * (a->lim ? 90 : 75),
		      abrcc_send_bps(a->rate_bps, comp, B));
}

static void abrcc_main(struct sock *sk, const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct abrcc *a = abrcc_get(sk);
	struct abrcc_cfg c;
	u32 mss, now_ms, srtt_us, base_us, comp;
	u64 T, B, F, r, send, cwnd;

	if (unlikely(!a)) {             /* allocation failed in init: degraded but functional */
		tp->snd_cwnd = min_t(u32, tp->snd_cwnd_clamp, 1000);
		return;
	}

	abrcc_cfg_load(&c);
	mss = max_t(u32, tp->mss_cache, 536);
	now_ms = (u32)div_u64(tp->tcp_mstamp, USEC_PER_MSEC);
	srtt_us = max_t(u32, tp->srtt_us >> 3, 1000);

	if (rs->delivered >= 0) {
		u32 sec = (u32)div_u64(tp->tcp_mstamp, USEC_PER_SEC);

		abrcc_loss_add(sk, a, sec, rs->acked_sacked, max(rs->losses, 0));
		a->rnd_acked += max(rs->acked_sacked, 0);       /* Westwood+ input */
		a->app_lim = rs->is_app_limited;

		if (!rs->is_app_limited && rs->delivered > 0) {
			a->last_busy_ms = now_ms;
			if (!a->active)
				abrcc_set_active(a, true);
		}

		if (rs->rtt_us > 0) {
			u32 m = (u32)rs->rtt_us;

			/* Jitter = |difference of consecutive samples|: slow queue growth is not jitter */
			if (a->last_rtt_us) {
				u32 d = m > a->last_rtt_us ? m - a->last_rtt_us : a->last_rtt_us - m;

				a->jitter_us = a->jitter_us - (a->jitter_us >> 3) + (d >> 3);
			}
			a->last_rtt_us = m;
			if (m < a->rnd_min_us)
				a->rnd_min_us = m;
			if (!a->min_rtt_us || m <= a->min_rtt_us) {
				a->min_rtt_us = m;
				a->min_rtt_stamp = now_ms;
			}
			if (a->in_prtt && m < a->pr_min_us)
				a->pr_min_us = m;
		}

		/* Delivery rate sample: non-app-limited and interval >= base rtt */
		base_us = abrcc_base_rtt(a, srtt_us);
		if (!rs->is_app_limited && rs->delivered > 0 &&
		    rs->interval_us > 0 && (u64)rs->interval_us >= base_us) {
			u64 bps = div64_u64((u64)rs->delivered * mss * USEC_PER_SEC,
					    rs->interval_us);
			s64 delta = (s64)min_t(u64, bps, ABRCC_BPS_CAP) - (s64)a->dr_bps;

			a->dr_bps = (u64)((s64)a->dr_bps + div_s64(delta, 8));
			if (a->gecko)           /* gecko record: best delivery rate of the start burst */
				a->gk_peak_bps = max_t(u64, a->gk_peak_bps, a->dr_bps);
		}
	}

	/* GECKO: the burst ends gecko_s seconds after the handshake */
	if (a->gecko && now_ms - a->start_ms >= c.gkms)
		abrcc_gecko_end(sk, a, &c, now_ms);

	if ((u64)(now_ms - a->last_round_ms) * 1000ULL >= srtt_us) {
		a->last_round_ms = now_ms;
		abrcc_round(sk, a, &c, mss, srtt_us, now_ms);
	}

	T = mbps_to_bps(c.tgt);
	F = abrcc_floor(a, &c, now_ms);
	B = max_t(u64, min_t(u64, abrcc_share_bps(a, &c), T), F);
	r = clamp_t(u64, a->rate_bps, F, B);
	if (a->gecko)                   /* start burst: full budget at once, loss ignored */
		r = B;
	else if (a->in_prtt)            /* PROBE_RTT-lite: drain the queue; a manual minimum stays effective */
		r = max_t(u64, r / 2, c.minm ? F : abrcc_min_rate(a, &c));

	/* Brutal compensation, bounded: congestion factor inside comp, wire rate hard-capped at B */
	comp = a->gecko ? 0 : abrcc_comp_bp(a, &c);
	send = abrcc_send_bps(r, comp, B);

	/* cwnd = send * srtt * gain / 10, with 0.1-packet precision and no u64 overflow at 100 Gbit/s */
	cwnd = div64_u64(send * srtt_us, 100000ULL * mss) * c.gain / 100;
	cwnd = clamp_t(u64, cwnd, ABRCC_MIN_CWND, tp->snd_cwnd_clamp);
	tp->snd_cwnd = (u32)cwnd;
	WRITE_ONCE(sk->sk_pacing_rate, min_t(u64, send, sk->sk_max_pacing_rate));
}

#if ABRCC_NEW_CC_API
static void abrcc_cong_control(struct sock *sk, u32 ack, int flag,
			       const struct rate_sample *rs)
#else
static void abrcc_cong_control(struct sock *sk, const struct rate_sample *rs)
#endif
{
	abrcc_main(sk, rs);
}

static void abrcc_init(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct abrcc **p = inet_csk_ca(sk);
	struct abrcc *a;
	struct abrcc_cfg c;
	u32 now_ms = (u32)div_u64(tp->tcp_mstamp, USEC_PER_MSEC);

	BUILD_BUG_ON(sizeof(struct abrcc *) > ICSK_CA_PRIV_SIZE);

	a = kzalloc(sizeof(*a), GFP_ATOMIC);
	*p = a;
	if (sk->sk_pacing_status == SK_PACING_NONE)
		cmpxchg(&sk->sk_pacing_status, SK_PACING_NONE, SK_PACING_NEEDED);
	tp->snd_ssthresh = TCP_INFINITE_SSTHRESH;
	if (!a)
		return;

	abrcc_cfg_load(&c);
	a->start_ms = a->last_round_ms = a->last_backoff_ms = now_ms;
	a->last_busy_ms = a->reprobe_stamp = a->min_rtt_stamp = now_ms;
	a->hold_until_ms = a->heal_stamp = now_ms;
	a->bwe_stamp_ms = now_ms;
	a->rnd_min_us = U32_MAX;
	a->comp_pct = 100;
	a->rand_pct = 50;               /* Veno verdict starts undecided */
	a->loss_sec = (u32)div_u64(tp->tcp_mstamp, USEC_PER_SEC);
	a->floor_pct = 100;
	a->mode = MODE_CTCP;
	a->state = ST_CTCP;
	minmax_reset(&a->bw, 0, 0);
	abrcc_set_active(a, true);
	a->sal_ref_ms = a->sal_last_ms = now_ms;
	a->sal_gap_ms = abrcc_rand_ms(c.salgmin_ms, c.salgmax_ms);
	a->ceil_bps = mbps_to_bps(c.tgt);
	if (c.gk) {
		u64 F = abrcc_floor(a, &c, now_ms);

		a->gecko = 1;           /* first seconds: full budget, loss ignored, peak recorded */
		a->rate_bps = max_t(u64, min_t(u64, abrcc_share_bps(a, &c), mbps_to_bps(c.tgt)), F);
	} else {
		a->rate_bps = abrcc_floor(a, &c, now_ms);       /* start at the (manual or automatic) floor */
	}
}

static void abrcc_release(struct sock *sk)
{
	struct abrcc **p = inet_csk_ca(sk);
	struct abrcc *a = *p;

	if (!a)
		return;
	abrcc_set_active(a, false);     /* also removes the limited-flow contribution */
	*p = NULL;
	kfree(a);
}

static u32 abrcc_ssthresh(struct sock *sk)
{
	return tcp_sk(sk)->snd_ssthresh;
}

static u32 abrcc_undo_cwnd(struct sock *sk)
{
	return tcp_sk(sk)->snd_cwnd;
}

static struct tcp_congestion_ops abrcc_ops = {
	.name         = "abrcc",
	.owner        = THIS_MODULE,
	.init         = abrcc_init,
	.release      = abrcc_release,
	.cong_control = abrcc_cong_control,
	.ssthresh     = abrcc_ssthresh,
	.undo_cwnd    = abrcc_undo_cwnd,
};

static int __init abrcc_register(void)
{
	return tcp_register_congestion_control(&abrcc_ops);
}

static void __exit abrcc_unregister(void)
{
	tcp_unregister_congestion_control(&abrcc_ops);
}

module_init(abrcc_register);
module_exit(abrcc_unregister);

MODULE_AUTHOR("ABRCC");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("ABRCC v3.3: Brutal + CTCP + CUBIC + Veno + Westwood+ + fair share + gecko start + salamander/hybridization probe");
