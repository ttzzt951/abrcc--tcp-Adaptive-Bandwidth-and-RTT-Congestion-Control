# ABRCC v3.3 Modification Manual (Alpine Linux)

Install, enable, disable and tune `tcp_abrcc.c` v3.3 on Alpine Linux (3.19 or newer, OpenRC, BusyBox, musl). ABRCC is a **sender-side** congestion control: install it on the server that sends the data. All commands assume you are root (Alpine has no sudo by default; use `doas` if you installed it). I have not compiled it on your Alpine kernel, so test on a non-production machine first.

## 1. Prerequisites

Alpine ships several kernel flavors (`lts`, `virt`, `edge`). The build package must match the flavor and the exact version of the **running** kernel.

```
uname -r                                  # e.g. 6.12.x-0-lts  (last word = flavor)
apk update
apk add build-base linux-lts-dev          # linux-virt-dev / linux-edge-dev for other flavors
apk add kmod                              # optional: full modprobe/depmod tools
ls /lib/modules/$(uname -r)/build         # must exist (otherwise see /usr/src)
```

If the `-dev` package version differs from `uname -r`, run `apk upgrade` and `reboot` first, then install again. If `build` does not exist, look in `/usr/src` and use that directory as `KDIR` below. If `make` complains about a missing libelf, run `apk add elfutils-dev`.

Limits:

- Containers (Docker, LXC) cannot load kernel modules. Install ABRCC on the host kernel.
- Alpine does not enforce module signatures by default. If your setup uses Secure Boot with enforced signing, the module must be signed.

## 2. Build and install

```
mkdir -p /root/abrcc && cd /root/abrcc      # put tcp_abrcc.c here
```

Create `Makefile` (recipe lines start with a TAB):

```
KDIR ?= /lib/modules/$(shell uname -r)/build
obj-m += tcp_abrcc.o
all:
	make -C $(KDIR) M=$(PWD) modules
clean:
	make -C $(KDIR) M=$(PWD) clean
```

```
make
mkdir -p /lib/modules/$(uname -r)/extra
install -m 644 tcp_abrcc.ko /lib/modules/$(uname -r)/extra/
depmod -a
modprobe tcp_abrcc target_mbps=200 min_mbps=5
lsmod | grep abrcc
```

Notes:

- After every kernel upgrade (`apk upgrade` that replaces `linux-lts`) rebuild and reinstall the module for the new kernel before rebooting into it.
- The `cong_control` API change of kernel 6.9 is auto-detected. To force it add `ccflags-y += -DABRCC_NEW_CC_API=1` (or `=0`) to the Makefile.

## 3. Enable and disable ABRCC

ABRCC needs the `fq` qdisc (or kernel internal pacing).

```
modprobe sch_fq
sysctl -w net.core.default_qdisc=fq
sysctl -w net.ipv4.tcp_congestion_control=abrcc
sysctl net.ipv4.tcp_available_congestion_control     # abrcc must be listed
```

**Make it permanent (OpenRC):**

```
printf 'sch_fq\ntcp_abrcc\n' >> /etc/modules
rc-update add modules boot
printf 'net.core.default_qdisc=fq\nnet.ipv4.tcp_congestion_control=abrcc\n' > /etc/sysctl.d/99-abrcc.conf
rc-update add sysctl boot
rc-service sysctl restart
```

After the next reboot verify `sysctl net.ipv4.tcp_congestion_control`. If it still says `cubic` (the sysctl service ran before the module was loaded), apply it from a local script:

```
printf '#!/bin/sh\nsysctl -w net.core.default_qdisc=fq\nsysctl -w net.ipv4.tcp_congestion_control=abrcc\n' > /etc/local.d/abrcc.start
chmod +x /etc/local.d/abrcc.start
rc-update add local default
```

**fq on the NIC** (the default qdisc only applies to interfaces brought up afterwards):

```
apk add iproute2-tc
tc qdisc replace dev eth0 root fq
```

To keep it, add `post-up tc qdisc replace dev eth0 root fq` to the interface section in `/etc/network/interfaces`.

New connections use ABRCC; existing connections keep their old algorithm until they are re-opened. A program can also select it with `setsockopt(fd, IPPROTO_TCP, TCP_CONGESTION, "abrcc", 5)` (non-root programs also need `abrcc` in `net.ipv4.tcp_allowed_congestion_control`).

**Disable ABRCC:**

```
sysctl -w net.ipv4.tcp_congestion_control=cubic     # or bbr
rm /etc/sysctl.d/99-abrcc.conf /etc/local.d/abrcc.start 2>/dev/null
sed -i '/^tcp_abrcc/d' /etc/modules
rmmod tcp_abrcc               # only works when no connection uses it any more
```

## 4. How to change a parameter

All parameters are module parameters (mode 0644, root can write). Values are re-read and range-checked on every ACK, so out-of-range values are clamped, never dangerous. Switches take `1` (on) or `0` (off).

**At runtime (instant, all connections, lost at reboot or module reload):**

```
cat /sys/module/tcp_abrcc/parameters/target_mbps
echo 500 > /sys/module/tcp_abrcc/parameters/target_mbps
grep -H . /sys/module/tcp_abrcc/parameters/*            # list everything
```

**Permanent (either way works):**

```
# A) options file, applied whenever the module loads
printf 'options tcp_abrcc target_mbps=500 min_mbps=5 gecko_on=1 sal_on=1\n' > /etc/modprobe.d/abrcc.conf

# B) inline in /etc/modules
tcp_abrcc target_mbps=500 min_mbps=5
```

Units: `bp` = basis points (100 bp = 1%). `Mbit/s` values are wire rate (bits per second).

### Diskless or data mode (RAM-based Alpine)

`/lib/modules` is rebuilt from the boot media, so a module copied there is lost at reboot. Keep the `.ko` in a path covered by `lbu` and load it with `insmod`:

```
mkdir -p /etc/abrcc && cp tcp_abrcc.ko /etc/abrcc/
printf '#!/bin/sh\nmodprobe sch_fq\ninsmod /etc/abrcc/tcp_abrcc.ko target_mbps=500 min_mbps=5\nsysctl -w net.core.default_qdisc=fq\nsysctl -w net.ipv4.tcp_congestion_control=abrcc\n' > /etc/local.d/abrcc.start
chmod +x /etc/local.d/abrcc.start
rc-update add local default
lbu add /etc/abrcc
lbu commit
```

The `.ko` must be rebuilt after every kernel update (`insmod` refuses a module built for another kernel).

## 5. Maximum and minimum bandwidth

### 5.1 Maximum: `target_mbps`

Total wire-rate budget of the whole server, default 200, range 1 to 100000 Mbit/s. With `fair_share=1` it is shared by the active flows, and the sum of all flows (loss compensation included) is hard-capped at it. Use 90 to 95% of the real bottleneck: 100 Mbit/s link = 90 to 95, 1 Gbit/s = 900 to 950, 10 Gbit/s = 9000 to 9500.

```
echo 950 > /sys/module/tcp_abrcc/parameters/target_mbps
```

Related: `hold_drop_mbps` (HIGH mode entry = target minus this value, scaled by share). `sal_goal_mbps` and `gecko_min_mbps` are clamped to `target_mbps`.

### 5.2 Minimum: `min_mbps`

- `0` (default): automatic ladder, split among active flows. Start floor 10 Mbit/s (target >= 200), 7 (>= 100), else 5; rising over 20 to 60 s to `min(max(target/4, 20/15/10), 50)` Mbit/s. It never stays above 75% of a proven capacity.
- `1` to `15`: manual total minimum, always effective (also at 0% loss and in PROBE\_RTT). Hard-limited to **15 Mbit/s total** (compile-time `ABRCC_MIN_CAP_MBPS`), divided among active flows, never above `target_mbps`.
- `min_relax=1` lets the floor relax 100% to 50% to 25% under persistent overload (default 0).
- v3.3 budget rule: the floor is capped at `target / number_of_flows`, so many flows can never exceed the server budget. The absolute floor is 0.5 Mbit/s, also capped by that share.

```
echo 10 > /sys/module/tcp_abrcc/parameters/min_mbps
```

### 5.3 Start rate: `gecko_min_mbps`

After the start burst the rate is cut to `max(peak/2, gecko_min_mbps)`, default 8 Mbit/s.

## 6. Feature switches

| Feature | Parameter | Off | On | Default |
| --- | --- | --- | --- | --- |
| GECKO start burst | `gecko_on` | 0 | 1 | 1 |
| SALAMANDER ladder probe + HYBRIDIZATION | `sal_on` | 0 | 1 | 1 |
| CUBIC + Veno + Westwood+ cut (0 = exact v3.1 loss behaviour) | `cubic_on` | 0 | 1 | 1 |
| CHIMERA all-protocol probe | `chimera` | 0 | 1 | 0 |
| Fair share among flows | `fair_share` | 0 | 1 | 1 |
| Brutal loss compensation | `max_comp_bp` | 0 | 5000 | 5000 |
| PROBE\_RTT-lite | `prtt_s` | 0 | seconds | 10 |
| Periodic CTCP re-probe of HIGH flows | `reprobe_s` | 0 | seconds | 20 |
| Floor relaxation | `min_relax` | 0 | 1 | 0 |
| Result logging to the kernel log | `sal_log` | 0 | 1 | 1 |

Brutal HIGH mode cannot be switched off completely. It is entered only above `loss_hi_bp` (default 1%); `loss_hi_bp=5000` (maximum) makes it practically unreachable.

## 7. All parameters

| Parameter | Default | Range | Meaning |
| --- | --- | --- | --- |
| `target_mbps` | 200 | 1..100000 | Server total maximum, Mbit/s |
| `min_mbps` | 0 | 0..15 | Manual total minimum, 0 = automatic ladder |
| `min_relax` | 0 | 0/1 | Floor may relax under overload |
| `hold_drop_mbps` | 50 | any | HIGH entry = target minus this |
| `loss_lo_bp` | 30 | 1..loss\_hi | Below = pure CTCP (0.3%) |
| `loss_hi_bp` | 100 | 10..5000 | Above = HYBRID/Brutal mode (1%) |
| `gamma_pkts` | 30 | 1..10000 | CTCP gamma |
| `cwnd_gain` | 20 | 10..100 | cwnd gain, /10 |
| `q_tol_pct` | 25 | 5..400 | Queue tolerance, % of base RTT |
| `q_tol_min_ms` | 10 | 1..1000 | Minimum queue tolerance |
| `jit_pct` | 30 | 5..400 | Jitter threshold, % of base RTT |
| `jit_min_ms` | 10 | 1..1000 | Minimum jitter threshold |
| `overload_pct` | 60 | 10..95 | Delivery below this % of sending = overload |
| `max_comp_bp` | 5000 | 0..7500 | Loss compensation cap, 0 = none |
| `fair_share` | 1 | 0/1 | Share target among active flows |
| `fair_idle_ms` | 2000 | 200..60000 | Idle time before a flow leaves the pool |
| `reprobe_s` | 20 | 0..3600 | CTCP re-probe of HIGH flows, 0 = off |
| `reprobe_rounds` | 8 | 1..64 | Rounds HIGH stays blocked after re-probe |
| `hold_rounds` | 8 | 1..64 | Base rounds CTCP runs before HIGH again |
| `hold_min_ms` | 200 | 10..10000 | Time floor of one hold unit |
| `hold_max_s` | 30 | 1..600 | Upper bound of the exponential hold |
| `prtt_s` | 10 | 0..3600 | Min RTT refresh interval, 0 = off |
| `prtt_ms` | 200 | 50..2000 | PROBE\_RTT-lite duration |
| `ext_loss_bp` | 3000 | loss\_hi..9000 | Extreme congestion loss (30%) |
| `cubic_on` | 1 | 0/1 | CUBIC + Veno + Westwood+ cut |
| `cub_loss_bp` | 20 | 1..loss\_hi | CUBIC joins CTCP from this loss (0.2%) |
| `cub_beta` | 70 | 50..87 | Cut floor, % of rate |
| `veno_beta` | 3 | 1..1000 | Veno backlog threshold, packets |
| `veno_pct` | 8 | 1..100 | Stable-path queue tolerance, % of base RTT |
| `chimera` | 0 | 0/1 | CHIMERA mode |
| `chimera_loss_bp` | 1000 | loss\_hi..9000 | Peak loss that arms CHIMERA (10%) |
| `chimera_rounds` | 16 | 4..64 | Rounds of random loss before CHIMERA |
| `gecko_on` | 1 | 0/1 | Start burst |
| `gecko_s` | 5 | 1..30 | Burst length, seconds |
| `gecko_min_mbps` | 8 | 1..target | Start rate = max(peak/2, this) |
| `sal_on` | 1 | 0/1 | Salamander + hybridization |
| `sal_goal_mbps` | 200 | 2..target | Top rung of the ladder |
| `sal_off_mbps` | 30 | 1..goal | Below this best rate: hybridization only |
| `sal_idle_s` | 600 | 10..86400 | Goal not reached this long before a sprint |
| `sal_gap_min_s` | 360 | 10..86400 | Sprint gap minimum (6 min) |
| `sal_gap_max_s` | 420 | 10..86400 | Sprint gap maximum (7 min) |
| `sal_dur_s` | 30 | 5..300 | Sprint length |
| `sal_ramp_s` | 6 | 1..120 | Linear ramp inside the sprint (<= dur/2) |
| `sal_tries` | 3 | 1..3 | Sprint chances per window |
| `sal_loss_bp` | 200 | 10..5000 | Failure: loss rise over the start level |
| `sal_rtt_pct` | 50 | 10..400 | Failure: queue delay, % of base RTT |
| `sal_cool_min_s` | 150 | 1..3600 | Pinned to pre-probe rate after failure, min |
| `sal_cool_max_s` | 210 | 1..3600 | Same, max |
| `sal_reach_pct` | 95 | 50..100 | Delivery % of goal that counts as reached |
| `sal_hyb_gap_s` | 360 | 10..86400 | Hybridization every 6 min |
| `sal_hyb_s` | 20 | 5..300 | Hybridization probe length |
| `sal_hyb_tries` | 3 | 1..3 | Max hybridization probes, success or not |
| `sal_log` | 1 | 0/1 | Log results to the kernel log |

How the new modes behave:

- **GECKO**: for `gecko_s` seconds after the handshake the flow runs at its full budget and ignores loss; the best delivery rate is recorded; then the rate is cut to `max(peak/2, gecko_min_mbps)`.
- **SALAMANDER ladder**: from the best delivery rate ever seen. Below 30 Mbit/s salamander is off and hybridization runs; 30 to 50 targets 50, 50 to 100 targets 100, 100 to 150 targets 150, 150 to 200 targets 200. The rungs are fixed in the code; `sal_goal_mbps` only caps them. A sprint (about 30 s) starts only when the goal has not been reached for 10 minutes, then every 6 to 7 minutes, at most 3 chances. Severe loss rise, queue delay or overload lasting about 2 RTT = failure: the rate returns to the pre-probe rate and stays there for 2:30 to 3:30.
- **HYBRIDIZATION** (best below 30 Mbit/s): every 6 minutes, at most 3 times, about 20 s, CTCP step x4, no loss-based back-off, CUBIC + Westwood+ + BBR joined, never above 50 Mbit/s; fails only on severe queue delay or overload.

## 8. Ready-made recipes

```
# 1 Gbit/s server, defaults otherwise
printf 'options tcp_abrcc target_mbps=950 min_mbps=5\n' > /etc/modprobe.d/abrcc.conf

# Plain and predictable: no start burst, no probes
echo 0 > /sys/module/tcp_abrcc/parameters/gecko_on
echo 0 > /sys/module/tcp_abrcc/parameters/sal_on
```

- Exact v3.1 loss behaviour: `cubic_on=0 gecko_on=0 sal_on=0`
- No loss compensation: `max_comp_bp=0`
- Gentle on shared links: `gecko_on=0 sal_on=0 fair_share=1` and `target_mbps` at 90% of the link
- Lossy WiFi or mobile clients: keep `cubic_on=1`, optionally `chimera=1`
- Highest guaranteed floor: `min_mbps=15` (hard cap)
- Quiet logs: `sal_log=0`

## 9. Monitoring and troubleshooting

```
apk add iproute2-ss                      # provides ss
ss -ti dst <client-ip>                   # cc name (abrcc), pacing_rate, cwnd, rtt, delivery_rate
dmesg | grep abrcc                       # gecko / sprint / hybrid results
lsmod | grep abrcc
sysctl net.ipv4.tcp_congestion_control
```

Log lines: `abrcc: gecko port A->B peak X Mbit/s (goal G reached|not reached) start Y Mbit/s` and `abrcc: sprint|hybrid ok|fail-loss|fail-rtt|fail-overload|timeout|abort ...`. Kernel log output is rate-limited, so not every attempt of a very busy server is printed.

Common problems:

- `modprobe: ... Exec format error` or `Invalid module format`: the module was built for a different kernel. Rebuild against the running kernel (`uname -r`).
- `make` cannot find the build directory: the `-dev` package does not match the running kernel, run `apk upgrade`, reboot, reinstall `linux-lts-dev`.
- `abrcc` missing from `tcp_available_congestion_control`: the module is not loaded, run `modprobe tcp_abrcc`.
- Congestion control still `cubic` after reboot: use the `/etc/local.d/abrcc.start` method from section 3.
- Parameters reset after reboot: write them to `/etc/modprobe.d/abrcc.conf` or inline in `/etc/modules`.
- Speed jumps at the start of every connection: that is GECKO. Set `gecko_on=0` or shorten `gecko_s`.

## 10. Compile-time constants (rebuild required)

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
