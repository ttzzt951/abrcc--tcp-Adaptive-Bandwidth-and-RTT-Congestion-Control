// SPDX-License-Identifier: GPL-2.0
/*
 * ABRCC: tcp-brutal 的最低速度/丢包补偿 + CTCP 的延迟窗口探测
 *
 * 必须装在"发送数据的一端"(例如 VPS 中继),对公网服务器无效。
 *
 * 丢包率(5 秒滑动窗口,与 Brutal 相同)决定三种模式:
 *   loss <  0.5%      : 完全不听丢包,只用 CTCP 延迟窗口,速率不低于最低速度
 *   0.5% <= loss <= 1%: 保留 CTCP 的丢包退让(每秒最多一次 x7/8),但不低于最低速度
 *   loss >  1%        : Brutal + CTCP 混合
 *                       PROBE: 每个周期半周期探测台阶,成功则抬高台阶,失败则回落(上下起伏)
 *                       HIGH : RTT <= rtt_thresh_ms 时立刻拉到 target_mbps;
 *                              RTT 超过时按 CTCP 方式降速,但不低于 target_mbps - hold_drop_mbps
 *
 * 最低速度阶梯(target 为用户填写的带宽,单位 Mbps):
 *   0~20s : target>=200 ->10, >=100 ->7, 其它 ->5
 *   20s   : target>500 ->40, >=200 ->20, >=100 ->15, 其它 ->10
 *   20~60s: 从 20s 的值线性升到 60s 的值
 *   >=60s : min(max(target/4, 20s 的值), 50)   (target=200 时为 50)
 *
 * 用法: insmod abrcc.ko target_mbps=200
 *       sysctl -w net.ipv4.tcp_congestion_control=abrcc
 *       参数可在 /sys/module/abrcc/parameters/ 里运行时修改(全局生效)
 */
#include <linux/module.h>
#include <linux/version.h>
#include <linux/math64.h>
#include <net/tcp.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 18, 0)
static inline u32 tcp_snd_cwnd(const struct tcp_sock *tp) { return tp->snd_cwnd; }
static inline void tcp_snd_cwnd_set(struct tcp_sock *tp, u32 val) { tp->snd_cwnd = val; }
#endif

#define ABRCC_SLOTS        5
#define ABRCC_MIN_SAMPLES  50
#define ABRCC_MIN_ACK_BP   8000   /* 补偿上限与 Brutal 相同: 最多放大 1/0.8 */
#define ABRCC_MIN_CWND     4

enum { MODE_CTCP, MODE_BAND, MODE_HYBRID };
enum { ST_PROBE, ST_HIGH };

static unsigned int target_mbps    = 200;  /* 用户填写的最大带宽 */
static unsigned int rtt_thresh_ms  = 60;   /* RTT 稳定线 */
static unsigned int hold_drop_mbps = 50;   /* RTT 变差时最低保持 target - 50 */
static unsigned int loss_lo_bp     = 50;   /* 0.5% (万分比) */
static unsigned int loss_hi_bp     = 100;  /* 1% */
static unsigned int gamma_pkts     = 30;   /* CTCP gamma: 队列里积压多少包算拥塞 */
static unsigned int cwnd_gain      = 20;   /* /10, Brutal 默认 15, 推荐 20 */
static unsigned int probe_cycle_ms = 1000; /* PROBE 周期 */

module_param(target_mbps, uint, 0644);
module_param(rtt_thresh_ms, uint, 0644);
module_param(hold_drop_mbps, uint, 0644);
module_param(loss_lo_bp, uint, 0644);
module_param(loss_hi_bp, uint, 0644);
module_param(gamma_pkts, uint, 0644);
module_param(cwnd_gain, uint, 0644);
module_param(probe_cycle_ms, uint, 0644);

struct abrcc_slot {
	u32 sec;
	u32 acked;
	u32 losses;
};

struct abrcc {
	u64 ctcp_bps;           /* CTCP/HIGH 模式的当前速率, 字节/秒 */
	u32 start_ms;
	u32 last_round_ms;
	u32 last_backoff_ms;
	u32 probe_t0_ms;
	u32 level_mbps;         /* PROBE 当前探测台阶 */
	u8 mode;
	u8 state;
	u8 burst_bad;
	struct abrcc_slot slots[ABRCC_SLOTS];
};

static inline u64 mbps_to_bps(u32 m)
{
	return (u64)m * 125000ULL;
}

static inline u32 tgt_mbps(void)
{
	return max_t(u32, READ_ONCE(target_mbps), 1);
}

static inline u32 hold_mbps(u32 tgt)
{
	u32 d = READ_ONCE(hold_drop_mbps);

	return tgt > d ? tgt - d : tgt / 2;
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

static u32 abrcc_loss_bp(struct abrcc *a, struct tcp_sock *tp)
{
	u32 sec = (u32)div_u64(tp->tcp_mstamp, USEC_PER_SEC);
	u32 acked = 0, losses = 0;
	int i;

	for (i = 0; i < ABRCC_SLOTS; i++) {
		if (sec - a->slots[i].sec < ABRCC_SLOTS) {
			acked += a->slots[i].acked;
			losses += a->slots[i].losses;
		}
	}
	if (acked + losses < ABRCC_MIN_SAMPLES)
		return 0;
	return (u32)div64_u64((u64)losses * 10000ULL, (u64)acked + losses);
}

static void abrcc_probe_round(struct abrcc *a, bool rtt_ok, u32 f_mbps,
			      u32 hold, u32 now_ms)
{
	u32 cyc = max_t(u32, READ_ONCE(probe_cycle_ms), 200);
	u32 phase = now_ms - a->probe_t0_ms;

	/* 探测半周期以及之后的 1/4 周期内, RTT 变差都算探测失败 */
	if (phase < cyc * 3 / 4 && !rtt_ok)
		a->burst_bad = 1;

	if (phase >= cyc) {
		if (!a->burst_bad) {
			a->level_mbps += max_t(u32, f_mbps, a->level_mbps / 2);
			if (a->level_mbps >= hold)
				a->state = ST_HIGH;      /* 台阶够高: 像 Brutal 一样拉上去 */
		} else {
			a->level_mbps = max_t(u32, f_mbps, a->level_mbps * 3 / 4);
		}
		a->burst_bad = 0;
		a->probe_t0_ms = now_ms;
	}
}

/* 每个 RTT 一次 */
static void abrcc_round(struct sock *sk, struct abrcc *a, u32 loss_bp,
			u32 srtt_us, u32 base_us, u32 now_ms)
{
	struct tcp_sock *tp = tcp_sk(sk);
	u32 mss = max_t(u32, tp->mss_cache, 536);
	u32 tgt = tgt_mbps();
	u32 f_mbps = abrcc_floor_mbps(tgt, (now_ms - a->start_ms) / 1000);
	u64 F = mbps_to_bps(f_mbps), B = mbps_to_bps(tgt);
	u32 hold = hold_mbps(tgt);
	u64 H = mbps_to_bps(hold);
	bool rtt_ok = srtt_us <= READ_ONCE(rtt_thresh_ms) * 1000;
	u64 r = max_t(u64, a->ctcp_bps, F);
	u64 win, diff, incr;

	/* CTCP 的延迟窗口, 换算成速率: diff = 估计排队的包数 */
	win  = div64_u64(r * srtt_us, 1000000ULL * mss);
	diff = div64_u64(r * (u64)(srtt_us - base_us), 1000000ULL * mss);
	if (diff < READ_ONCE(gamma_pkts)) {
		/* dwnd 增量 = alpha * win^k, alpha=1/8, k=0.75 */
		incr = int_sqrt(win * int_sqrt(win)) / 8;
		if (!incr)
			incr = 1;
		r += div64_u64(incr * mss * 1000000ULL, srtt_us);
	} else {
		/* dwnd -= zeta * diff, 等价于 rate *= base_rtt / rtt */
		r = div64_u64(r * base_us, srtt_us);
	}

	if (loss_bp > READ_ONCE(loss_hi_bp)) {
		a->mode = MODE_HYBRID;
		if (a->state == ST_PROBE) {
			abrcc_probe_round(a, rtt_ok, f_mbps, hold, now_ms);
			r = F;
		} else if (rtt_ok) {
			r = B;                               /* Brutal 式瞬间拉满 */
		} else {
			r = max_t(u64, min_t(u64, r, a->ctcp_bps), H); /* 只减不增, 不低于 B-50 */
		}
	} else {
		a->mode = loss_bp >= READ_ONCE(loss_lo_bp) ? MODE_BAND : MODE_CTCP;
		if (a->mode == MODE_BAND && now_ms - a->last_backoff_ms >= 1000) {
			r = r * 7 / 8;                       /* CTCP 的丢包退让, 每秒一次 */
			a->last_backoff_ms = now_ms;
		}
	}
	a->ctcp_bps = clamp_t(u64, r, F, B);
}

static u64 abrcc_cur_rate(struct abrcc *a, u32 now_ms, u64 F, u64 B)
{
	u64 r;

	if (a->mode == MODE_HYBRID && a->state == ST_PROBE) {
		u32 cyc = max_t(u32, READ_ONCE(probe_cycle_ms), 200);
		bool burst = (now_ms - a->probe_t0_ms) < cyc / 2;

		r = burst ? mbps_to_bps(a->level_mbps) : F;
	} else {
		r = a->ctcp_bps;
	}
	return clamp_t(u64, r, F, B);
}

static void abrcc_update(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct abrcc *a = inet_csk_ca(sk);
	u32 now_ms = (u32)div_u64(tp->tcp_mstamp, USEC_PER_MSEC);
	u32 srtt_us = max_t(u32, tp->srtt_us >> 3, 1000);
	u32 base_us = tcp_min_rtt(tp);
	u32 tgt = tgt_mbps();
	u64 B = mbps_to_bps(tgt);
	u64 F = mbps_to_bps(abrcc_floor_mbps(tgt, (now_ms - a->start_ms) / 1000));
	u32 loss_bp = abrcc_loss_bp(a, tp);
	u32 mss = max_t(u32, tp->mss_cache, 536);
	u32 ack_bp;
	u64 r, send, cwnd;

	if (!base_us || base_us > srtt_us)
		base_us = srtt_us;

	if ((u64)(now_ms - a->last_round_ms) * 1000ULL >= srtt_us) {
		a->last_round_ms = now_ms;
		abrcc_round(sk, a, loss_bp, srtt_us, base_us, now_ms);
	}

	r = abrcc_cur_rate(a, now_ms, F, B);

	/* Brutal 的丢包补偿: send = rate / ack_rate */
	ack_bp = 10000 - min_t(u32, loss_bp, 10000 - ABRCC_MIN_ACK_BP);
	send = div_u64(r * 10000ULL, ack_bp);

	cwnd = div64_u64(send * srtt_us * READ_ONCE(cwnd_gain),
			 10ULL * 1000000ULL * mss);
	cwnd = clamp_t(u64, cwnd, ABRCC_MIN_CWND, tp->snd_cwnd_clamp);
	tcp_snd_cwnd_set(tp, (u32)cwnd);
	WRITE_ONCE(sk->sk_pacing_rate, min_t(u64, send, sk->sk_max_pacing_rate));
}

static void abrcc_init(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct abrcc *a = inet_csk_ca(sk);
	u32 now_ms = (u32)div_u64(tp->tcp_mstamp, USEC_PER_MSEC);
	u32 f0;

	BUILD_BUG_ON(sizeof(struct abrcc) > ICSK_CA_PRIV_SIZE);
	memset(a, 0, sizeof(*a));

	f0 = abrcc_floor_mbps(tgt_mbps(), 0);
	a->start_ms = a->last_round_ms = a->last_backoff_ms = a->probe_t0_ms = now_ms;
	a->level_mbps = f0;
	a->ctcp_bps = mbps_to_bps(f0);
	a->mode = MODE_CTCP;
	a->state = ST_PROBE;

	if (sk->sk_pacing_status == SK_PACING_NONE)
		cmpxchg(&sk->sk_pacing_status, SK_PACING_NONE, SK_PACING_NEEDED);
	tp->snd_ssthresh = TCP_INFINITE_SSTHRESH;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
static void abrcc_cong_control(struct sock *sk, u32 ack, int flag,
			       const struct rate_sample *rs)
#else
static void abrcc_cong_control(struct sock *sk, const struct rate_sample *rs)
#endif
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct abrcc *a = inet_csk_ca(sk);
	struct abrcc_slot *s;
	u32 sec;

	if (rs->delivered < 0 || rs->interval_us <= 0)
		return;

	sec = (u32)div_u64(tp->tcp_mstamp, USEC_PER_SEC);
	s = &a->slots[sec % ABRCC_SLOTS];
	if (s->sec == sec) {
		s->acked += rs->acked_sacked;
		s->losses += rs->losses;
	} else {
		s->sec = sec;
		s->acked = rs->acked_sacked;
		s->losses = rs->losses;
	}
	abrcc_update(sk);
}

static u32 abrcc_ssthresh(struct sock *sk)
{
	return tcp_sk(sk)->snd_ssthresh;
}

static u32 abrcc_undo_cwnd(struct sock *sk)
{
	return tcp_snd_cwnd(tcp_sk(sk));
}

static struct tcp_congestion_ops abrcc_ops = {
	.flags        = TCP_CONG_NON_RESTRICTED,
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
MODULE_DESCRIPTION("ABRCC: Brutal floor/compensation + CTCP delay window");
