// SPDX-License-Identifier: GPL-2.0
/*
 * ABRCC v2: Brutal loss compensation + minimum speed ladder + relative-RTT CTCP delay window
 *
 * Must be installed on the "data sender" end. Requires fq qdisc (or internal kernel pacing).
 *
 * States:
 *   ST_CTCP : Probes rate using CTCP delay window ticked by RTT (replaces old fixed-period PROBE)
 *   ST_HIGH : Brutal style, rate is pulled directly toward target (subject to ceil upper bound)
 *
 * State Transitions:
 *   - RTT jitter persistently excessive (delay signal untrustworthy) -> directly enter HIGH, abandon CTCP
 *   - Loss enters HYBRID (>1%) and CTCP probed rate reaches hold -> HIGH
 *   - Delay worsens / throughput overloaded for 8 consecutive rounds in HIGH -> revert to CTCP to re-probe
 *   - Loss drops back below HYBRID (with hysteresis) and delay is trustworthy -> revert to CTCP
 *
 * Loss: 5-second exponential decay window + smoothing + hysteresis; losses during RTO are not counted;
 *       Compensation upper bound defaults to 50% (can compensate even if loss > 10%).
 * Negative Feedback: Actual delivery rate is far below sending rate => overload => clamped to delivery_rate * 1.25
 *                    (Independent of RTT/loss, suitable for shallow buffer / rate limiter / AQM scenarios).
 *
 * Usage: modprobe tcp_abrcc target_mbps=200
 *        sysctl -w net.core.default_qdisc=fq net.ipv4.tcp_congestion_control=abrcc
 *        Parameters: /sys/module/tcp_abrcc/parameters/
 */
#include <linux/module.h>
#include <linux/version.h>
#include <linux/math64.h>
#include <net/tcp.h>

/* cong_control signature changed starting from kernel 6.9; if backported by a distro, can override with -DABRCC_NEW_CC_API=0/1 */
#ifndef ABRCC_NEW_CC_API
# if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
#  define ABRCC_NEW_CC_API 1
# else
#  define ABRCC_NEW_CC_API 0
# endif
#endif

#define ABRCC_MIN_SAMPLES 100
#define ABRCC_MIN_CWND    4

enum { MODE_CTCP, MODE_BAND, MODE_HYBRID };
enum { ST_CTCP, ST_HIGH };

static unsigned int target_mbps    = 200;  /* Maximum bandwidth */
static unsigned int hold_drop_mbps = 50;   /* Under HYBRID, enter HIGH when CTCP probes up to target-50 */
static unsigned int loss_lo_bp      = 50;   /* 0.5% */
static unsigned int loss_hi_bp      = 100;  /* 1% */
static unsigned int gamma_pkts     = 30;   /* CTCP gamma */
static unsigned int cwnd_gain      = 20;   /* /10 */
static unsigned int q_tol_pct      = 25;   /* Queue tolerance: % of base_rtt */
static unsigned int q_tol_min_ms   = 10;   /* Minimum queue tolerance */
static unsigned int jit_pct        = 30;   /* Jitter threshold: % of base_rtt */
static unsigned int jit_min_ms     = 10;   /* Minimum jitter threshold */
static unsigned int overload_pct   = 60;   /* Delivery rate < sending rate * 60% is considered overload */
static unsigned int max_comp_bp    = 5000; /* Upper limit for loss compensation (in basis points), 5000 = 50% */

module_param(target_mbps, uint, 0644);
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

/* Read once per ACK and perform range validation so arbitrary runtime parameter changes won't cause issues */
struct abrcc_cfg {
	u32 tgt, hold, lo, hi, gamma, gain;
	u32 qpct, qmin_us, jpct, jmin_us, ovl, comp;
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
}

struct abrcc {
	u64 rate_bps;         /* Current base rate (without loss compensation), bytes/sec */
	u64 ceil_bps;         /* Learned rate ceiling after overload, slowly recovering */
	u32 start_ms;
	u32 last_round_ms;
	u32 last_backoff_ms;
	u32 loss_sec;         /* Loss window: current second */
	u32 cur_acked, cur_lost;
	u32 hist_acked, hist_lost;   /* Decay history */
	u32 loss_s;           /* Smoothed loss rate (in basis points) */
	u32 jitter_us;        /* RTT jitter EWMA */
	u32 dr_bps;           /* Delivery rate EWMA, bytes/sec */
	u8 mode, state;
	u8 jit_rounds, bad_rounds, ovl_rounds, grace;
	u8 floor_pct, floor_bad, floor_ok;
	u8 app_lim;
};

static inline u64 mbps_to_bps(u32 m)
{
	return (u64)m * 125000ULL;
}

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

/* Minimum speed; when persistent overload prevents transmission even at the floor, relax the floor as 100% -> 50% -> 25% */
static u64 abrcc_floor(const struct abrcc *a, u32 tgt, u32 now_ms)
{
	u64 f = mbps_to_bps(abrcc_floor_mbps(tgt, (now_ms - a->start_ms) / 1000));

	return max_t(u64, f * a->floor_pct / 100, mbps_to_bps(1));
}

static inline u32 abrcc_base_rtt(const struct tcp_sock *tp, u32 srtt_us)
{
	u32 b = tcp_min_rtt(tp);

	return (!b || b > srtt_us) ? srtt_us : b;
}

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
	/* During RTO, the entire window is marked lost; do not count it to avoid misjudgment as high random loss */
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

/* Once per RTT */
static void abrcc_round(struct abrcc *a, const struct abrcc_cfg *c, u32 mss,
			u32 srtt_us, u32 base_us, u32 now_ms)
{
	u64 B = mbps_to_bps(c->tgt);
	u64 F = abrcc_floor(a, c->tgt, now_ms);
	u64 H = min_t(u64, mbps_to_bps(c->hold), B);
	u64 r = clamp_t(u64, a->rate_bps, F, B);
	u64 ceil = clamp_t(u64, a->ceil_bps, F, B);
	u64 old = r;
	u32 tol  = max_t(u32, (u64)base_us * c->qpct / 100, c->qmin_us);
	u32 jthr = max_t(u32, (u64)base_us * c->jpct / 100, c->jmin_us);
	bool rtt_ok = srtt_us <= base_us + tol;        /* Queuing delay relative to base_rtt */
	bool jit_hi = a->jitter_us > jthr;
	bool delay_bad, overload = false, bad;

	/* Loss: smoothing + hysteresis */
	a->loss_s = (a->loss_s * 3 + abrcc_loss_inst(a)) / 4;
	if (a->loss_s > c->hi ||
	    (a->mode == MODE_HYBRID && a->loss_s > c->hi * 7 / 10))
		a->mode = MODE_HYBRID;
	else if (a->loss_s >= c->lo ||
		 (a->mode != MODE_CTCP && a->loss_s >= c->lo * 7 / 10))
		a->mode = MODE_BAND;
	else
		a->mode = MODE_CTCP;

	/* RTT jitter persistently large => delay signal untrustworthy */
	if (jit_hi)
		a->jit_rounds = min_t(u8, a->jit_rounds + 1, 16);
	else if (a->jit_rounds)
		a->jit_rounds--;
	delay_bad = a->jit_rounds >= 3;

	/* Delivery rate overload judgment (skip grace period after acceleration and app-limited) */
	if (a->grace)
		a->grace--;
	else if (!a->app_lim && a->dr_bps)
		overload = (u64)a->dr_bps * 100 < r * c->ovl;
	bad = overload || (!delay_bad && !rtt_ok);

	if (delay_bad)
		a->state = ST_HIGH;                    /* Abandon CTCP */
	else if (a->state == ST_HIGH && a->mode != MODE_HYBRID)
		a->state = ST_CTCP;

	if (a->state == ST_CTCP) {
		u64 win  = div64_u64(r * srtt_us, 1000000ULL * mss);
		u64 diff = div64_u64(r * (u64)(srtt_us - base_us), 1000000ULL * mss);

		a->bad_rounds = 0;
		if (diff < c->gamma) {
			if (!a->app_lim) {
				u64 incr;

				win = min_t(u64, win, 1ULL << 20);
				incr = int_sqrt((unsigned long)win *
						int_sqrt((unsigned long)win)) / 8;
				incr = max_t(u64, incr, 1);
				if (a->mode == MODE_HYBRID)
					incr *= 4;         /* High loss is mostly random loss, probe faster */
				r += div64_u64(incr * mss * 1000000ULL, srtt_us);
			}
		} else {
			r = div64_u64(r * base_us, srtt_us);
		}
		if (a->mode == MODE_BAND && now_ms - a->last_backoff_ms >= 1000) {
			r = r * 7 / 8;
			a->last_backoff_ms = now_ms;
		}
		if (a->mode == MODE_HYBRID && r >= H)
			a->state = ST_HIGH;
	} else {
		if (bad)
			a->bad_rounds = min_t(u8, a->bad_rounds + 1, 255);
		else
			a->bad_rounds = 0;

		if (a->bad_rounds >= 3) {
			r = r * 7 / 8;
			if (!delay_bad && a->bad_rounds >= 8)
				a->state = ST_CTCP;     /* Revert to re-probe */
		} else if (!a->app_lim && !bad) {
			if (a->mode == MODE_HYBRID) {
				r = min_t(u64, B, ceil);        /* Brutal-style max out */
			} else if (a->mode == MODE_BAND &&
				   now_ms - a->last_backoff_ms >= 1000) {
				r = r * 7 / 8;
				a->last_backoff_ms = now_ms;
			} else {
				r = min_t(u64, r + r / 2, ceil);
			}
		}
	}

	/* Delivery rate negative feedback: independent of RTT / loss */
	if (overload) {
		if (a->ovl_rounds < 255)
			a->ovl_rounds++;
	} else {
		a->ovl_rounds = 0;
	}
	if (a->ovl_rounds >= 3) {
		r = min_t(u64, r, max_t(u64, (u64)a->dr_bps + a->dr_bps / 4, F));
		ceil = r;
	} else if (!a->app_lim && !overload && ceil < B) {
		ceil = min_t(u64, B, ceil + ceil / 16 + 1);
	}

	/* Floor relaxes during persistent overload, recovers once cleared */
	if (!a->app_lim) {
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
	F = abrcc_floor(a, c->tgt, now_ms);
	a->rate_bps = clamp_t(u64, r, F, B);
	a->ceil_bps = ceil;
}

static void abrcc_main(struct sock *sk, const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct abrcc *a = inet_csk_ca(sk);
	struct abrcc_cfg c;
	u32 mss = max_t(u32, tp->mss_cache, 536);
	u32 now_ms = (u32)div_u64(tp->tcp_mstamp, USEC_PER_MSEC);
	u32 srtt_us = max_t(u32, tp->srtt_us >> 3, 1000);
	u32 base_us = abrcc_base_rtt(tp, srtt_us);
	u32 ack_bp;
	u64 B, F, r, send, cwnd;

	abrcc_cfg_load(&c);

	if (rs->delivered >= 0) {
		u32 sec = (u32)div_u64(tp->tcp_mstamp, USEC_PER_SEC);

		abrcc_loss_add(sk, a, sec, rs->acked_sacked, max(rs->losses, 0));
		a->app_lim = rs->is_app_limited;

		if (rs->rtt_us > 0 && tp->srtt_us) {
			u32 s = tp->srtt_us >> 3, m = (u32)rs->rtt_us;
			u32 d = m > s ? m - s : s - m;

			a->jitter_us = a->jitter_us - (a->jitter_us >> 3) + (d >> 3);
		}
		/* Delivery rate sample: non-app-limited and sampling interval >= min_rtt */
		if (!rs->is_app_limited && rs->delivered > 0 &&
		    rs->interval_us > 0 && (u64)rs->interval_us >= base_us) {
			u64 bps = div64_u64((u64)rs->delivered * mss * USEC_PER_SEC,
					    rs->interval_us);
			s64 delta = (s64)min_t(u64, bps, U32_MAX) - (s64)a->dr_bps;

			a->dr_bps = (u32)((s64)a->dr_bps + div_s64(delta, 8));
		}
	}

	if ((u64)(now_ms - a->last_round_ms) * 1000ULL >= srtt_us) {
		a->last_round_ms = now_ms;
		abrcc_round(a, &c, mss, srtt_us, base_us, now_ms);
	}

	B = mbps_to_bps(c.tgt);
	F = abrcc_floor(a, c.tgt, now_ms);
	r = clamp_t(u64, a->rate_bps, F, B);

	/* Brutal loss compensation, upper bound determined by max_comp_bp */
	ack_bp = 10000 - min_t(u32, a->loss_s, c.comp);
	send = div_u64(r * 10000ULL, ack_bp);

	cwnd = div64_u64(send * srtt_us * c.gain, 10ULL * 1000000ULL * mss);
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
	struct abrcc *a = inet_csk_ca(sk);
	struct abrcc_cfg c;
	u32 now_ms = (u32)div_u64(tp->tcp_mstamp, USEC_PER_MSEC);

	BUILD_BUG_ON(sizeof(struct abrcc) > ICSK_CA_PRIV_SIZE);
	memset(a, 0, sizeof(*a));
	abrcc_cfg_load(&c);

	a->start_ms = a->last_round_ms = a->last_backoff_ms = now_ms;
	a->loss_sec = (u32)div_u64(tp->tcp_mstamp, USEC_PER_SEC);
	a->floor_pct = 100;
	a->rate_bps = mbps_to_bps(abrcc_floor_mbps(c.tgt, 0));
	a->ceil_bps = mbps_to_bps(c.tgt);
	a->mode = MODE_CTCP;
	a->state = ST_CTCP;

	if (sk->sk_pacing_status == SK_PACING_NONE)
		cmpxchg(&sk->sk_pacing_status, SK_PACING_NONE, SK_PACING_NEEDED);
	tp->snd_ssthresh = TCP_INFINITE_SSTHRESH;
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
MODULE_DESCRIPTION("ABRCC v2: Brutal compensation + floor ladder + relative-RTT CTCP");