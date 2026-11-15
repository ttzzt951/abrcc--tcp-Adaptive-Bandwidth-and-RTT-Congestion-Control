# ABRCC v3.3 Modification Manual (Ubuntu)

Covers every option of `tcp_abrcc.c` v3.3 on stock Ubuntu (22.04 / 24.04): install, enable / disable, maximum and minimum bandwidth, every module parameter, and every feature switch. ABRCC is a **sender-side** congestion control: install it on the server that sends the data. I have not compiled it against your kernel, so test on a non-production machine first.

## 1. Build and install

```
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
mkdir -p ~/abrcc && cd ~/abrcc      # put tcp_abrcc.c here
```

Create `Makefile` (the recipe lines must start with a TAB):

```
obj-m += tcp_abrcc.o
all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules
clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
```

Build, install and load:

```
make
sudo install -D -m 644 tcp_abrcc.ko /lib/modules/$(uname -r)/extra/tcp_abrcc.ko
sudo depmod -a
sudo modprobe tcp_abrcc target_mbps=200 min_mbps=5
```

Notes:

- Rebuild and reinstall after every kernel update (the module is tied to the running kernel).
- Secure Boot rejects unsigned modules. Sign the module with a MOK key, or disable Secure Boot.
- The `cong_control` API change in kernel 6.9 is auto-detected. If a vendor kernel backported it, force it by adding `ccflags-y += -DABRCC_NEW_CC_API=1` (or `=0`) to the Makefile.

## 2. Enable and disable ABRCC

ABRCC needs the `fq` qdisc (or the kernel internal pacing).

```
sysctl net.ipv4.tcp_available_congestion_control      # abrcc must be listed
sudo sysctl -w net.core.default_qdisc=fq
sudo sysctl -w net.ipv4.tcp_congestion_control=abrcc
tc qdisc show dev eth0                                # optional: confirm fq on the NIC
sudo tc qdisc replace dev eth0 root fq                # optional: apply fq now
```

Make it permanent:

```
echo tcp_abrcc | sudo tee /etc/modules-load.d/abrcc.conf
printf 'net.core.default_qdisc=fq\nnet.ipv4.tcp_congestion_control=abrcc\n' | sudo tee /etc/sysctl.d/99-abrcc.conf
sudo sysctl --system
```

New connections use ABRCC; existing connections keep their old algorithm until they are re-opened.

Other ways to select it:

- Per application: `setsockopt(fd, IPPROTO_TCP, TCP_CONGESTION, "abrcc", 5)`. Non-root programs also need `abrcc` in `net.ipv4.tcp_allowed_congestion_control`.
- Per route: `ip route replace <existing route> congctl abrcc`.

Disable ABRCC:

```
sudo sysctl -w net.ipv4.tcp_congestion_control=cubic    # (or bbr)
sudo rm /etc/modules-load.d/abrcc.conf /etc/sysctl.d/99-abrcc.conf
sudo rmmod tcp_abrcc          # only works when no connection uses it any more
```

## 3. How to change a parameter

All parameters are module parameters (mode 0644, root can write).

**At runtime (instant, all connections, lost at reboot / module reload):**

```
cat /sys/module/tcp_abrcc/parameters/target_mbps
echo 500 | sudo tee /sys/module/tcp_abrcc/parameters/target_mbps
grep -H . /sys/module/tcp_abrcc/parameters/*          # list everything
```

Values are re-read and range-checked on every ACK, so out-of-range values are clamped, never dangerous. Use `echo 0|1` for switches (any non-zero value means on).

**Permanent (applied whenever the module loads):**

```
sudo tee /etc/modprobe.d/abrcc.conf <<'EOF'
options tcp_abrcc target_mbps=500 min_mbps=5 gecko_on=1 sal_on=1
EOF
sudo update-initramfs -u        # only needed if the module is loaded from the initramfs
```

Units: `bp` = basis points (100 bp = 1%). `Mbit/s` values are wire rate (bits per second, not bytes).

## 4. Maximum and minimum bandwidth

### 4.1 Maximum: `target_mbps`

Total wire-rate budget of the whole server, default 200, range 1..100000 Mbit/s. With `fair_share=1` the budget is shared by the active flows, and the sum of all flows (loss compensation included) is hard-capped at it.

| Uplink | Suggested `target_mbps` |
| --- | --- |
| 100 Mbit/s | 90 to 95 |
| 1 Gbit/s | 900 to 950 |
| 10 Gbit/s | 9000 to 9500 |

Rule of thumb: 90 to 95% of the real bottleneck. Setting it higher than the link makes ABRCC overshoot and the overload logic fight back.

```
echo 950 | sudo tee /sys/module/tcp_abrcc/parameters/target_mbps
```

Related: `hold_drop_mbps` (HIGH mode entry point is target minus this value, scaled by share), `sal_goal_mbps` and `gecko_min_mbps` are clamped to `target_mbps`.

### 4.2 Minimum: `min_mbps`

- `min_mbps=0` (default): automatic ladder, split among active flows. Per target: start floor 10 Mbit/s (target >= 200), 7 (>= 100), else 5; rising over 20 to 60 s to `min(max(target/4, 20/15/10), 50)` Mbit/s. In automatic mode the floor never stays above 75% of a proven capacity.
- `min_mbps=1..15`: manual total minimum, always effective (also at 0% loss and in PROBE\_RTT). Hard-limited to **15 Mbit/s total** (compile-time constant `ABRCC_MIN_CAP_MBPS`), divided among active flows, never above `target_mbps`.
- `min_relax=1` lets the floor relax 100% to 50% to 25% under persistent overload (default 0, so the manual floor stays).
- v3.3 budget rule: the floor is always capped at `target / number_of_flows`, so many flows can never exceed the server budget. The absolute floor is 0.5 Mbit/s, also capped by that share.

```
echo 10 | sudo tee /sys/module/tcp_abrcc/parameters/min_mbps
```

### 4.3 Start rate: `gecko_min_mbps`

After the 5 second start burst the rate is cut to `max(peak/2, gecko_min_mbps)` (default 8 Mbit/s).

## 5. Feature switches (on / off)

| Feature | Parameter | Off | On | Default |
| --- | --- | --- | --- | --- |
| GECKO start burst (first seconds at full budget) | `gecko_on` | 0 | 1 | 1 |
| SALAMANDER ladder probe + HYBRIDIZATION low-speed probe | `sal_on` | 0 | 1 | 1 |
| CUBIC probing + Veno verdict + Westwood+ cut (0 = exact v3.1 loss behaviour) | `cubic_on` | 0 | 1 | 1 |
| CHIMERA (all-protocol probe under sustained random loss) | `chimera` | 0 | 1 | 0 |
| Fair share among flows | `fair_share` | 0 | 1 | 1 |
| Brutal loss compensation | `max_comp_bp` | 0 | 5000 | 5000 |
| PROBE\_RTT-lite (min RTT refresh) | `prtt_s` | 0 | seconds | 10 |
| Periodic CTCP re-probe of HIGH flows | `reprobe_s` | 0 | seconds | 20 |
| Floor relaxation | `min_relax` | 0 | 1 | 0 |
| Log gecko / salamander / hybrid results to dmesg | `sal_log` | 0 | 1 | 1 |

Brutal HIGH mode cannot be switched off completely. It is entered only at loss above `loss_hi_bp` (default 1%); set `loss_hi_bp=5000` (the maximum, 50%) to make it practically unreachable.

## 6. Parameter reference

### 6.1 Bandwidth and fairness

| Parameter | Default | Range | Meaning |
| --- | --- | --- | --- |
| `target_mbps` | 200 | 1..100000 | Server total maximum (wire rate), Mbit/s |
| `min_mbps` | 0 | 0..15 (and <= target) | Manual total minimum, 0 = automatic ladder |
| `min_relax` | 0 | 0/1 | Allow floor to relax under persistent overload |
| `fair_share` | 1 | 0/1 | Share the target among active flows |
| `fair_idle_ms` | 2000 | 200..60000 | A flow idle longer than this leaves the share pool |
| `hold_drop_mbps` | 50 | any | HIGH entry point = target minus this (scaled by share) |

### 6.2 Loss and mode thresholds

| Parameter | Default | Range | Meaning |
| --- | --- | --- | --- |
| `loss_lo_bp` | 30 | 1..loss\_hi | Below this loss (0.3%) = pure CTCP, no Brutal |
| `loss_hi_bp` | 100 | 10..5000 | Above this (1%) = HYBRID (Brutal) mode |
| `max_comp_bp` | 5000 | 0..7500 | Loss compensation upper bound (5000 = 50%), 0 = none |
| `ext_loss_bp` | 3000 | loss\_hi..9000 | Extreme congestion loss (30%) |

### 6.3 CTCP delay window and RTT logic

| Parameter | Default | Range | Meaning |
| --- | --- | --- | --- |
| `gamma_pkts` | 30 | 1..10000 | CTCP gamma (queued packets before delay back-off) |
| `cwnd_gain` | 20 | 10..100 | cwnd = rate x RTT x gain/10 |
| `q_tol_pct` | 25 | 5..400 | Queue tolerance, % of base RTT |
| `q_tol_min_ms` | 10 | 1..1000 | Minimum queue tolerance, ms |
| `jit_pct` | 30 | 5..400 | Jitter threshold, % of base RTT |
| `jit_min_ms` | 10 | 1..1000 | Minimum jitter threshold, ms |
| `overload_pct` | 60 | 10..95 | Delivery below this % of sending = overload |
| `prtt_s` | 10 | 0..3600 | Min RTT refresh interval, 0 = off (60 s expiry kept) |
| `prtt_ms` | 200 | 50..2000 | PROBE\_RTT-lite duration |

### 6.4 HIGH (Brutal) state timing

| Parameter | Default | Range | Meaning |
| --- | --- | --- | --- |
| `reprobe_s` | 20 | 0..3600 | Force CTCP re-probe of HIGH flows every N s, 0 = off |
| `reprobe_rounds` | 8 | 1..64 | Rounds HIGH stays blocked after a re-probe |
| `hold_rounds` | 8 | 1..64 | Base rounds CTCP must run before re-entering HIGH |
| `hold_min_ms` | 200 | 10..10000 | Time floor of one hold unit |
| `hold_max_s` | 30 | 1..600 | Upper bound of the exponential hold |

### 6.5 CUBIC, Veno, Westwood+, CHIMERA (v3.2)

| Parameter | Default | Range | Meaning |
| --- | --- | --- | --- |
| `cubic_on` | 1 | 0/1 | CUBIC + Veno + Westwood+ cut, 0 = v3.1 behaviour |
| `cub_loss_bp` | 20 | 1..loss\_hi (and <= loss\_lo) | CUBIC joins CTCP from this loss (0.2%) |
| `cub_beta` | 70 | 50..87 | Cut floor, % of rate |
| `veno_beta` | 3 | 1..1000 | Veno backlog threshold, packets |
| `veno_pct` | 8 | 1..100 | Stable-path queue tolerance, % of base RTT (min 2 ms) |
| `chimera` | 0 | 0/1 | CHIMERA mode, default off |
| `chimera_loss_bp` | 1000 | loss\_hi..9000 | Peak loss that arms CHIMERA (10%) |
| `chimera_rounds` | 16 | 4..64 | Rounds of sustained random loss before CHIMERA starts |

### 6.6 GECKO (v3.3)

| Parameter | Default | Range | Meaning |
| --- | --- | --- | --- |
| `gecko_on` | 1 | 0/1 | Start burst on/off |
| `gecko_s` | 5 | 1..30 | Burst length in seconds after the handshake |
| `gecko_min_mbps` | 8 | 1..target | Start rate after the burst = max(peak/2, this) |

Behaviour: for `gecko_s` seconds the flow runs at its full budget and ignores loss; the best delivery rate is recorded; then the rate is cut to `max(peak/2, gecko_min_mbps)` (a path that never reached that value is not cut below what it delivered). The burst's loss and queue history is wiped.

### 6.7 SALAMANDER and HYBRIDIZATION (v3.3)

Ladder from the best delivery rate ever seen (gecko result included): below `sal_off_mbps` (30) = salamander off, hybridization runs; 30 to 50 targets 50; 50 to 100 targets 100; 100 to 150 targets 150; 150 to 200 targets 200; above that the ladder is finished. The rungs 50/100/150/200 are fixed in the code; `sal_goal_mbps` only caps them.

| Parameter | Default | Range | Meaning |
| --- | --- | --- | --- |
| `sal_on` | 1 | 0/1 | Salamander + hybridization on/off |
| `sal_goal_mbps` | 200 | 2..target | Top rung of the ladder |
| `sal_off_mbps` | 30 | 1..goal | Below this best rate: hybridization instead of salamander |
| `sal_idle_s` | 600 | 10..86400 | Goal not reached for this long before a sprint may start (10 min) |
| `sal_gap_min_s` / `sal_gap_max_s` | 360 / 420 | 10..86400 | Random gap between sprints (6 to 7 min) |
| `sal_dur_s` | 30 | 5..300 | Sprint length |
| `sal_ramp_s` | 6 | 1..120 (<= dur/2) | Linear ramp from start rate to goal inside the sprint |
| `sal_tries` | 3 | 1..3 | Sprint chances per window (refilled when a rung is reached) |
| `sal_loss_bp` | 200 | 10..5000 | Failure: loss rose this much over the start level |
| `sal_rtt_pct` | 50 | 10..400 | Failure: queue delay >= this % of base RTT (min `q_tol_min_ms`) |
| `sal_cool_min_s` / `sal_cool_max_s` | 150 / 210 | 1..3600 | After a failed sprint the rate is pinned to the pre-probe rate for this long |
| `sal_reach_pct` | 95 | 50..100 | Delivery >= this % of the goal counts as reached |
| `sal_hyb_gap_s` | 360 | 10..86400 | Hybridization every 6 min |
| `sal_hyb_s` | 20 | 5..300 | Hybridization probe length |
| `sal_hyb_tries` | 3 | 1..3 | Maximum hybridization probes, success or not |
| `sal_log` | 1 | 0/1 | Log every result to the kernel log |

Sprint: only on a clean path (loss below 0.3%, no queue growth), about 30 s, linear ramp plus the max of CTCP, CUBIC, Westwood+ and BBR-style candidates. Severe loss rise, queue delay or overload lasting about 2 RTT = failure: rate returns to the pre-probe rate and stays there for the cool-down. Hybridization (best below 30 Mbit/s): CTCP step x4, no loss-based back-off, CUBIC + Westwood+ + BBR joined, never above 50 Mbit/s; fails only on severe queue delay or overload. The last 3 attempts are kept in a record ring and logged; no other reset path touches them.

## 7. Ready-made recipes

**1 Gbit/s server, default behaviour:**

```
sudo tee /etc/modprobe.d/abrcc.conf <<'EOF'
options tcp_abrcc target_mbps=950 min_mbps=5
EOF
```

**Plain, predictable (no start burst, no probes):**

```
for p in gecko_on sal_on; do echo 0 | sudo tee /sys/module/tcp_abrcc/parameters/$p; done
```

**Exact v3.1 loss behaviour:** `cubic_on=0 gecko_on=0 sal_on=0`

**No loss compensation (never send above the measured need):** `max_comp_bp=0`

**Gentle on shared links:** `gecko_on=0 sal_on=0 fair_share=1 target_mbps=<90% of link>`

**Lossy WiFi / mobile clients:** keep `cubic_on=1`; optionally `chimera=1` to probe with all algorithms under sustained 10% random loss.

**Raise the guaranteed floor to the maximum:** `min_mbps=15` (never higher; hard cap).

**Quiet logs:** `sal_log=0`.

## 8. Monitoring and troubleshooting

```
dmesg | grep abrcc                       # gecko / sprint / hybrid results
ss -ti dst <client-ip>                   # shows cc name (abrcc), pacing_rate, cwnd, rtt
sysctl net.ipv4.tcp_congestion_control
lsmod | grep abrcc
```

Log lines:

- `abrcc: gecko port A->B peak X Mbit/s (goal G reached|not reached) start Y Mbit/s`
- `abrcc: sprint|hybrid ok|fail-loss|fail-rtt|fail-overload|timeout|abort port A->B base .. goal .. peak .. loss .. bp queue .. ms ...`

Log output is rate-limited by the kernel, so not every attempt of a very busy server is printed.

Common problems:

- `modprobe: Key was rejected by service`: Secure Boot, sign the module or disable Secure Boot.
- `abrcc` missing from `tcp_available_congestion_control`: module not loaded, run `sudo modprobe tcp_abrcc`.
- Throughput far below target: `target_mbps` is above the real link, or the client path is the limit; check `ss -ti` for `delivery_rate`.
- Speed jumps at the start of every connection: that is GECKO (first 5 s). Set `gecko_on=0` or shorten `gecko_s`.
- Parameters reset after reboot: write them to `/etc/modprobe.d/abrcc.conf` (sysfs changes are not persistent).

## 9. Compile-time constants (need a rebuild)

| Constant | Value | Meaning |
| --- | --- | --- |
| `ABRCC_MIN_CAP_MBPS` | 15 | Hard cap of the manual `min_mbps` |
| `ABRCC_MIN_RATE` | 62500 B/s | Absolute minimum rate (0.5 Mbit/s), lowered to the equal share if smaller |
| `ABRCC_BW_WIN` | 10 rounds | Bandwidth max-filter window |
| `ABRCC_HEAL_MS` | 5000 | Stable HIGH time before the back-off counter heals |
| `ABRCC_HYB_CAP_MBPS` | 50 | Hybridization never probes above this rate |
| `ABRCC_SAL_OK_MS` | 1000 | Delivery must hold at the goal this long for a sprint success |
| `ABRCC_SAL_RETRY_MS` | 30000 | Retry delay after an aborted probe |
| `ABRCC_SAL_RECS` | 3 | Record ring size = maximum chances |
