# ABRCC 3.3 — Adaptive Bandwidth and RTT Congestion Control

**An Adaptive Multi-Mode TCP Congestion Control Algorithm for Linux**

Version: 3.3  
Platform: Linux Kernel  
Category: TCP Congestion Control  
Architecture: Multi-Mode Adaptive Congestion Control

---

## 1. Introduction

ABRCC (Adaptive Bandwidth and RTT Congestion Control) is an experimental TCP congestion control algorithm designed for the Linux kernel.

Unlike traditional congestion control algorithms that primarily rely on a single control strategy, ABRCC combines multiple congestion control mechanisms and dynamically adjusts its behavior according to network conditions.

Its architecture integrates:

- Brutal-style packet loss compensation
- CTCP delay-based congestion control
- CUBIC bandwidth probing
- Veno-style random loss detection
- Westwood+ bandwidth estimation
- BBR-style bandwidth estimation
- Dynamic congestion compensation
- RTT and jitter management
- Multi-connection bandwidth fairness
- Adaptive rate limiting
- Special bandwidth probing mechanisms

The primary objective of ABRCC is to adapt its transmission behavior to different network conditions, including high packet loss, variable RTT, bandwidth fluctuations, and network congestion.

## 2. Main Operating Modes

ABRCC 3.3 contains three primary operating modes:

1. MODE_CTCP
2. MODE_BAND
3. MODE_HYBRID

Mode selection is primarily determined by the smoothed packet loss rate.

Hysteresis thresholds are used to prevent excessive switching between modes.

### Default Mode Selection Thresholds

| Parameter | Default | Meaning |
|---|---:|---|
| loss_lo_bp | 30 bp | 0.3% packet loss |
| loss_hi_bp | 100 bp | 1% packet loss |
| ext_loss_bp | 3000 bp | 30% extreme loss threshold |

One basis point (bp) equals 0.01%.

### 2.1 MODE_CTCP — Low-Loss Mode

**Activation condition:** Smoothed packet loss below 0.3%, subject to hysteresis when returning from another mode.

MODE_CTCP is the primary delay-based control mode.

Main functions:

- CTCP congestion window control.
- RTT-based transmission rate adjustment.
- CUBIC-assisted bandwidth probing.
- Minimum RTT tracking.
- Queue delay monitoring.
- Adaptive transmission rate growth.
- Delay-based rate reduction.

Default CTCP parameters:

| Parameter | Default |
|---|---:|
| gamma_pkts | 30 |
| cwnd_gain | 20 (10-based scaling) |

Brutal loss compensation is disabled in this mode.

### 2.2 MODE_BAND — Bandwidth-Adaptive Mode

**Activation condition:** Smoothed packet loss reaches 0.3%.

Main functions:

- Westwood+ bandwidth estimation.
- Brutal-style packet loss compensation.
- CUBIC-assisted probing.
- Veno-style random loss detection.
- Dynamic transmission rate adjustment.
- Congestion backoff.
- Bandwidth ceiling management.

The default congestion reduction factor is:

`rate = rate × 7/8`

This reduces the transmission rate by approximately 12.5%.

The default reduction interval is limited to once per second.

When Veno identifies a loss event as likely random rather than congestion-related, ordinary congestion backoff may be avoided.

### 2.3 MODE_HYBRID — High-Loss Hybrid Mode

**Activation condition:** Smoothed packet loss reaches 1%.

MODE_HYBRID combines multiple control strategies for more challenging network conditions.

Main functions:

- CTCP and CUBIC hybrid probing.
- Brutal-style loss compensation.
- Westwood+ bandwidth estimation.
- BBR-style bandwidth gain.
- Dynamic compensation strength.
- HIGH transmission state.
- Transmission rate ceiling management.

MODE_HYBRID contains two internal states:

- ST_CTCP
- ST_HIGH

In ST_CTCP, the algorithm continues bandwidth probing.

In ST_HIGH, the algorithm enters its high-speed control state.

Under HYBRID conditions, the CTCP growth increment can be multiplied by 4.

This multiplier applies to the growth increment, not the entire transmission rate.

---

## 3. Brutal — Target-Rate Control and Packet Loss Compensation

Brutal is one of the important components of ABRCC 3.3.

It introduces a target-rate-based transmission strategy that attempts to compensate for lost packets rather than automatically reducing the target rate after every loss event.

### 3.1 Brutal Operating Principle

Traditional congestion control algorithms commonly reduce their transmission rate when congestion-related packet loss occurs.

Brutal uses a different approach.

When packet loss occurs, it can increase the actual transmission rate to compensate for the lost data and attempt to maintain the intended effective delivery rate.

For example:

Assume the target delivery rate is 100 Mbps and the packet loss rate is 10%.

The theoretical transmission rate required to compensate for the losses is:

`send_rate = 100 / (1 - 0.10)`

`send_rate = 111.11 Mbps`

This means the sender attempts to transmit approximately 111.11 Mbps to achieve an effective delivery rate of 100 Mbps, assuming the loss model is representative.

### 3.2 ABRCC Brutal Compensation

ABRCC uses the following parameters:

| Parameter | Default | Purpose |
|---|---:|---|
| max_comp_bp | 5000 | Maximum compensation limit: 50% |
| comp_pct | 0–100 | Dynamic compensation strength |
| loss_s | Dynamic | Smoothed packet loss rate |

The compensation calculation is:

`comp = min(loss_s, max_comp_bp) × comp_pct / 100`

The theoretical compensated sending rate is:

`send_rate = base_rate / (1 - comp)`

The final rate is still restricted by the server's fair-share bandwidth budget.

### 3.3 Dynamic Compensation

ABRCC does not permanently operate at maximum compensation.

When congestion evidence is detected:

- Compensation strength is reduced by half.
- Additional transmission is restricted.
- Queue buildup and transport overload receive priority.

When congestion evidence disappears:

- Compensation strength increases by 10 per control round.
- Maximum compensation strength is 100%.

### 3.4 Brutal Activation and Deactivation

| Operating Condition | Brutal Compensation |
|---|---|
| MODE_CTCP | Disabled |
| MODE_BAND | Enabled |
| MODE_HYBRID | Enabled |
| Extreme congestion | Disabled |
| PROBE_RTT-lite | Disabled |
| GECKO startup phase | Disabled |

Brutal does not mean unrestricted transmission.

ABRCC retains RTT monitoring, overload detection, transmission ceilings, fair-share limits, and extreme congestion protection.

---

## 4. Internal Control States

ABRCC has two internal control states.

### 4.1 ST_CTCP — Standard Probing State

This is the primary bandwidth exploration state.

Responsibilities:

- CTCP delay-based window control.
- CUBIC-assisted growth.
- Available bandwidth probing.
- RTT monitoring.
- Determining whether the connection can enter HIGH.

### 4.2 ST_HIGH — High-Speed Control State

ST_HIGH is the high-speed control state associated with Brutal-style target-rate management.

Activation conditions:

1. The primary mode is MODE_HYBRID.
2. The current rate reaches the H threshold.
3. The backoff waiting period has expired.
4. No extreme congestion condition is active.

The default H threshold is calculated using the fair-share bandwidth budget B and the hold_drop_mbps parameter.

The default hold_drop_mbps value is 50 Mbps.

Main functions:

- Move the transmission rate toward the compensated fair-share target.
- Maintain Brutal-style target-rate control.
- Apply learned transmission ceilings.
- Monitor transport overload.
- Return to ST_CTCP when necessary.

If three consecutive control rounds are classified as abnormal, the algorithm reduces the rate and exits HIGH.

---

## 5. Four Special Mechanisms

ABRCC 3.3 includes four distinctive special mechanisms:

1. GECKO
2. SALAMANDER
3. HYBRIDIZATION
4. CHIMERA

These are special probing and control mechanisms, not additional primary operating modes.

### 5.1 GECKO — Startup Burst Probing

Default: Enabled.

Activation:

- During the initial TCP connection startup phase.

Default duration:

- 5 seconds.

Operating procedure:

1. Use the available fair-share bandwidth budget during startup.
2. Temporarily ignore ordinary packet loss feedback.
3. Record the highest delivery rate observed during startup.
4. Adjust the transmission rate after the startup phase.
5. Return to normal CTCP control.

The default post-startup rate is:

`rate = max(peak / 2, 8 Mbps)`

The actual rate is subject to the fair-share limit and the condition that it must not be reduced below the best delivery rate already achieved when the startup peak is below 8 Mbps.

GECKO also clears startup packet loss and queue history to prevent startup behavior from contaminating subsequent control decisions.

### 5.2 SALAMANDER — Staircase Bandwidth Acceleration

Default: Enabled through sal_on=1.

SALAMANDER uses the historical maximum delivery rate to determine the next bandwidth probing target.

Default bandwidth stages:

| Historical Best Delivery Rate | Next Target |
|---|---:|
| Below 30 Mbps | Use HYBRIDIZATION |
| 30–50 Mbps | 50 Mbps |
| 50–100 Mbps | 100 Mbps |
| 100–150 Mbps | 150 Mbps |
| 150–200 Mbps | 200 Mbps |
| 200 Mbps reached | Complete default staircase |

#### Activation Conditions

- Current internal state is ST_CTCP.
- Current primary mode is MODE_CTCP.
- RTT is within the permitted range.
- No extreme congestion is present.
- No transport overload is detected.
- Sufficient bandwidth growth headroom exists.
- The current target has not been reached for at least 10 minutes.
- The previous probing interval has expired.

#### Default Parameters

| Parameter | Default |
|---|---:|
| sal_idle_s | 600 seconds |
| sal_gap_min_s | 360 seconds |
| sal_gap_max_s | 420 seconds |
| sal_dur_s | 30 seconds |
| sal_ramp_s | 6 seconds |
| sal_tries | 3 |
| sal_reach_pct | 95% |

#### Probing Procedure

- Increase the transmission rate linearly toward the target.
- Combine CTCP, CUBIC, Westwood+, and BBR-style candidate rates.
- Limit candidate growth to a maximum of 2 times per control round.
- Respect the target bandwidth and fair-share limits.

#### Failure Conditions

1. Smoothed packet loss increases by at least 2 percentage points compared with the probing baseline.
2. Extreme congestion is detected.
3. The connection enters HIGH.
4. Queue delay reaches the minimum of 10 ms or 50% of the base RTT.
5. Transport overload is detected.

The abnormal condition must persist for approximately two RTTs and at least 200 ms before probing is considered unsuccessful.

#### Failure Recovery

- Restore the pre-probing transmission rate.
- Apply a transmission ceiling.
- Enter a randomized cooldown period of 150–210 seconds.
- Record the probing result.

#### Success Condition

The delivery rate reaches at least 95% of the target and remains there for at least one second.

SALAMANDER retains up to three probing records, including:

- Start time.
- Duration.
- Initial rate.
- Target rate.
- Peak rate.
- Packet loss.
- Queue delay.

### 5.3 HYBRIDIZATION — Low-Bandwidth Hybrid Probing

Default: Controlled by sal_on.

HYBRIDIZATION is designed for connections whose historical best delivery rate is below 30 Mbps.

#### Activation Conditions

- Historical best delivery rate is below 30 Mbps.
- Internal state is ST_CTCP.
- The connection is actively transmitting.
- PROBE_RTT is not active.
- Extreme congestion is absent.
- Transport overload is absent.
- Sufficient growth headroom exists.
- The minimum probing interval has elapsed.

#### Default Parameters

| Parameter | Default |
|---|---:|
| sal_off_mbps | 30 Mbps |
| sal_hyb_gap_s | 360 seconds |
| sal_hyb_s | 20 seconds |
| sal_hyb_tries | 3 |
| Maximum probing target | 50 Mbps |

#### Operating Procedure

1. Multiply the CTCP growth increment by 4.
2. Temporarily suspend ordinary packet loss backoff.
3. Include CUBIC as a candidate.
4. Include Westwood+ bandwidth estimation.
5. Include BBR-style bandwidth gain candidates.
6. Select the highest eligible candidate rate.

Unlike SALAMANDER, HYBRIDIZATION does not use the same requirement that the primary mode must be MODE_CTCP.

It is designed to explore additional bandwidth under low-bandwidth conditions.

#### Failure Conditions

- Severe queue delay.
- Transport overload.
- Extreme congestion.

After failure, the transmission rate returns to its pre-probing value.

Unlike SALAMANDER, a simple increase in packet loss does not automatically cause failure.

Each probing attempt lasts up to 20 seconds. A maximum of three attempts is permitted.

### 5.4 CHIMERA — Multi-Algorithm Parallel Candidate Mechanism

Default:

`chimera=0`

CHIMERA is disabled by default and must be explicitly enabled.

Enable:

`chimera=1`

#### Activation Conditions

- Peak packet loss reaches 10%.
- Veno random-loss classification reaches 60%.
- No congestion evidence is present.
- No extreme congestion condition is active.
- Conditions persist for at least 16 control rounds.

#### Participating Candidates

- CTCP
- CUBIC
- Westwood+
- BBR-style bandwidth gain

CHIMERA calculates multiple candidate transmission rates and selects the highest eligible candidate.

#### Eight-Round Cycle

| Cycle Stage | Behavior |
|---|---|
| Round 0 | BBR-style probing with a gain of 1.25 |
| Round 1 | BBR-style DRAIN, limited to 0.75 times estimated bandwidth |
| Remaining rounds | Normal candidate cruising with gain 1.0 |

Candidate growth remains subject to:

- Maximum per-round growth.
- Learned transmission ceilings.
- Overload detection.
- Server bandwidth budgets.

#### Version 3.3 Improvement

ABRCC 3.3 fixes an important CHIMERA DRAIN implementation issue.

Previously, the DRAIN stage used maximum-value comparison, preventing the 0.75 bandwidth limit from actually reducing the transmission rate.

Version 3.3 changes this behavior so that the DRAIN stage can genuinely reduce the rate.

---

## 6. Packet Loss Detection and Classification

ABRCC does not automatically treat every packet loss event as network congestion.

It attempts to distinguish:

- Random packet loss.
- Congestion-related packet loss.
- Extreme congestion.

### 6.1 Packet Loss Statistics

The algorithm calculates packet loss using ACK and lost-packet statistics.

A minimum of 100 ACK/loss samples is required before a valid instantaneous loss estimate is generated.

Full-window losses during RTO are excluded from ordinary loss statistics.

The smoothed loss rate is calculated as:

`loss_s = (old_loss_s × 3 + instant_loss) / 4`

ABRCC also maintains a decaying peak-loss measurement called loss_pk.

### 6.2 Veno-Style Random Loss Classification

Veno classification is updated when CUBIC participates.

It considers:

- RTT queue delay.
- Queue backlog in packets.
- Actual delivery rate.
- Expected delivery rate.
- RTT jitter.

Default parameters:

| Parameter | Default |
|---|---:|
| veno_beta | 3 packets |
| veno_pct | 8% of base RTT |
| Minimum queue tolerance | 2 ms |
| Low delivery threshold | 85% of expected rate |

The rand_pct classification value is interpreted as follows:

| rand_pct | Interpretation |
|---|---|
| 60–100 | More likely random loss |
| 40–60 | Uncertain classification |
| 0–40 | More likely congestion-related loss |

Random loss can avoid ordinary congestion backoff, whereas congestion-related loss triggers rate reduction.

### 6.3 Extreme Congestion Detection

Default threshold:

`ext_loss_bp = 3000`

This corresponds to 30% packet loss.

However, reaching 30% loss alone does not immediately activate extreme congestion protection.

The following conditions must also be satisfied:

- Smoothed packet loss reaches 30%.
- RTT queue congestion or transport overload evidence exists.
- The extreme condition persists for at least two control rounds.

When activated:

- Brutal compensation is disabled.
- Transmission rate is restricted.
- Probing mechanisms are limited.
- HIGH state is exited.
- Normal congestion control takes priority.

---

## 7. CUBIC and Westwood+ Integration

CUBIC is enabled by default.

Default parameters:

| Parameter | Default |
|---|---:|
| cubic_on | 1 |
| cub_loss_bp | 20 bp |
| cub_beta | 70% |

CUBIC can participate in CTCP probing when smoothed packet loss reaches 0.2%.

Its growth curve is used to generate candidate transmission rates.

### Westwood+ Bandwidth Estimation

Westwood+ estimates available bandwidth based on the amount of data delivered during each control round.

A 7/8 low-pass filter is used.

When congestion-related loss occurs:

1. Record the actual transmission rate.
2. Update the transmission ceiling.
3. Estimate available bandwidth using Westwood+.
4. Restrict the new rate to approximately 70–87.5% of the previous rate.
5. Restart the CUBIC epoch.
6. Prevent immediate ceiling recovery during the same control round.

This ordering is an important part of the 3.3 implementation.

---

## 8. BBR-Style Bandwidth Estimation and RTT Management

ABRCC maintains multiple bandwidth references.

These include:

- Maximum delivery rate over the most recent 10 control rounds.
- Delivery Rate EWMA.
- Westwood+ bandwidth estimation.
- BBR-style bandwidth gain candidates.

### 8.1 BBR-Style Bandwidth Filter

The algorithm uses the maximum observed delivery rate over a rolling window of 10 control rounds as a bandwidth reference.

### 8.2 Delivery Rate EWMA

Delivery-rate samples are smoothed using an EWMA with a 1/8 update weight.

### 8.3 PROBE_RTT-lite

Default minimum RTT refresh interval:

10 seconds.

Default probing duration:

200 ms.

Functions:

- Temporarily reduce transmission rate.
- Drain the queue.
- Update the minimum RTT reference.
- Restore normal control.

If SALAMANDER or HYBRIDIZATION is active, the probing mechanism gives priority to the ongoing special probing process.

### 8.4 High Jitter Handling

Default jitter threshold:

`max(base_rtt × 30%, 10 ms)`

When high jitter persists for three control rounds, the algorithm uses the minimum RTT sample from the current round instead of ordinary SRTT as its delay signal.

This helps reduce the influence of temporary RTT fluctuations.

---

## 9. Congestion Reduction and Recovery Mechanisms

ABRCC includes several independent rate reduction mechanisms.

| Mechanism | Default Behavior |
|---|---|
| CTCP RTT reduction | Reduces rate according to queue delay |
| BAND backoff | Multiplies rate by 7/8 |
| Westwood+ reduction | Adjusts rate using estimated bandwidth |
| CUBIC response | Updates epoch and reduces candidate rate |
| HIGH fallback | Exits HIGH after three abnormal rounds |
| Exponential backoff | Increases waiting time after failures |
| Extreme congestion protection | Disables compensation and restricts rate |
| Ceiling management | Maintains a learned safe rate limit |
| Automatic minimum rate | Dynamically adjusts the minimum rate |

### 9.1 Time-Based Backoff

Default parameters:

| Parameter | Default |
|---|---:|
| hold_rounds | 8 |
| hold_min_ms | 200 ms |
| hold_max_s | 30 seconds |
| reprobe_s | 20 seconds |
| reprobe_rounds | 8 |

As the number of failures increases, the waiting period becomes longer.

This prevents excessive repeated probing.

After at least 64 healthy control rounds and a minimum of five seconds of stable operation, the failure counter can gradually recover.

### 9.2 Congestion Capacity Learning

If three consecutive control rounds detect transport overload, the transmission rate is restricted to approximately 125% of the measured delivery rate.

If six consecutive rounds show that delivery rate is below 90% of sending rate, the algorithm may classify the path as having reached its practical capacity.

It then establishes a rate ceiling using the rolling maximum bandwidth estimate.

The ceiling recovers gradually under normal conditions.

It does not immediately increase during the same round in which congestion-related rate reduction occurs.

---

## 10. Global Fair-Share Bandwidth Management

ABRCC supports module-level bandwidth sharing across multiple TCP connections.

Default parameters:

| Parameter | Default |
|---|---:|
| target_mbps | 200 |
| fair_share | 1 |
| fair_idle_ms | 2000 |

Main functions:

1. Track active connections.
2. Allocate the server's total bandwidth budget.
3. Identify connections limited by their own network paths.
4. Reallocate unused bandwidth to other connections.
5. Prevent the sum of minimum connection rates from exceeding the server budget.

Version 3.3 also fixes a minimum-rate allocation issue that could allow the combined minimum rates to exceed the total server budget.

---

## 11. Dynamic Minimum Transmission Rate

ABRCC supports both automatic and manually configured minimum transmission rates.

Default parameters:

| Parameter | Default |
|---|---:|
| min_mbps | 0 |
| min_relax | 0 |
| Internal minimum rate | 0.5 Mbps |
| Manual minimum-rate aggregate limit | 15 Mbps |

The minimum transmission rate is restricted by the fair-share budget.

During persistent overload, the automatic minimum rate can gradually decrease.

When the network recovers, it can gradually increase again.

---

## 12. Complete Default Parameter Reference

| Parameter | Default | Function |
|---|---:|---|
| target_mbps | 200 | Server bandwidth budget |
| loss_lo_bp | 30 | MODE_BAND threshold |
| loss_hi_bp | 100 | MODE_HYBRID threshold |
| ext_loss_bp | 3000 | Extreme loss threshold |
| cub_loss_bp | 20 | CUBIC participation threshold |
| max_comp_bp | 5000 | Maximum compensation |
| fair_share | 1 | Enable fair-share allocation |
| gecko_on | 1 | Enable GECKO |
| gecko_s | 5 | GECKO startup duration |
| gecko_min_mbps | 8 | GECKO minimum startup target |
| sal_on | 1 | Enable SALAMANDER and HYBRIDIZATION |
| sal_goal_mbps | 200 | Maximum staircase target |
| sal_off_mbps | 30 | Low-bandwidth threshold |
| sal_dur_s | 30 | SALAMANDER duration |
| sal_ramp_s | 6 | SALAMANDER ramp duration |
| sal_tries | 3 | Maximum attempts per stage |
| sal_reach_pct | 95% | Successful target threshold |
| sal_loss_bp | 200 | Loss increase failure threshold |
| sal_cool_min_s | 150 | Minimum failure cooldown |
| sal_cool_max_s | 210 | Maximum failure cooldown |
| sal_hyb_gap_s | 360 | HYBRIDIZATION interval |
| sal_hyb_s | 20 | HYBRIDIZATION duration |
| sal_hyb_tries | 3 | Maximum HYBRIDIZATION attempts |
| chimera | 0 | CHIMERA disabled by default |
| chimera_loss_bp | 1000 | CHIMERA peak-loss threshold |
| chimera_rounds | 16 | CHIMERA activation persistence |

---

## 13. Version 3.3 — Changes and Improvements

ABRCC 3.3 introduces additional probing mechanisms and fixes several important implementation issues.

### New Mechanisms

1. **GECKO:** Five-second startup burst probing.
2. **SALAMANDER:** Multi-stage staircase bandwidth acceleration.
3. **HYBRIDIZATION:** Hybrid probing for connections below 30 Mbps.
4. **Probing history:** Records probing duration, targets, peak rates, packet loss, and queue delay.
5. **Failure recovery:** Probing rollback and cooldown mechanisms.

### Important Fixes and Optimizations

1. CHIMERA BBR-style DRAIN now genuinely reduces the transmission rate.
2. CUBIC congestion event calculation order has been corrected.
3. Ceiling, Westwood+ reduction, and CUBIC epoch calculations use the actual transmission rate.
4. Multi-connection minimum-rate allocation no longer exceeds the server bandwidth budget.
5. Fixed u32 overflow issues under high bandwidth conditions.
6. Important rate variables use u64 to improve large-rate calculations.
7. Improved CWND calculations for high-bandwidth and high-RTT environments.

---

## 14. Deployment Requirements

ABRCC 3.3 is a Linux kernel TCP congestion control module.

Requirements:

- Installation on the TCP sending endpoint.
- A Linux kernel supporting the required TCP congestion control interfaces.
- fq qdisc or an environment with appropriate internal pacing support.
- Correct server bandwidth budget configuration.

Example configuration:

```bash
modprobe tcp_abrcc target_mbps=200 min_mbps=5

sysctl -w net.core.default_qdisc=fq

sysctl -w net.ipv4.tcp_congestion_control=abrcc
```

Module parameters can be inspected through:

```bash
/sys/module/tcp_abrcc/parameters/
```

---

## 15. Testing and Research Directions

ABRCC 3.3 contains multiple interacting mechanisms. Its performance and stability should be evaluated through reproducible experiments.

Recommended testing areas:

- Throughput under different RTT conditions.
- Packet loss from 0.1% to 30%.
- Random-loss versus congestion-loss classification accuracy.
- Multi-connection fairness.
- Stability under high jitter.
- Brutal compensation effectiveness.
- GECKO startup burst behavior.
- SALAMANDER probing success rate.
- HYBRIDIZATION low-bandwidth performance.
- CHIMERA behavior under random packet loss.
- Comparisons against Linux CUBIC, BBR, and Reno.
- Long-duration stability and congestion recovery.

Performance claims should be supported by measured experimental results rather than inferred from algorithm design.

---

## 16. Architecture Summary

ABRCC 3.3 consists of three primary operating modes, two internal control states, multiple integrated congestion control mechanisms, and four special probing mechanisms.

### Three Primary Operating Modes

- MODE_CTCP
- MODE_BAND
- MODE_HYBRID

### Two Internal Control States

- ST_CTCP
- ST_HIGH

### Core Algorithms and Control Mechanisms

- Brutal
- CTCP
- CUBIC
- Veno
- Westwood+
- BBR-style bandwidth estimation
- BBRv3-style ceiling management

### Four Special Mechanisms

- GECKO
- SALAMANDER
- HYBRIDIZATION
- CHIMERA

### Auxiliary Systems

- Dynamic packet loss compensation
- RTT management
- Extreme congestion protection
- Time-based backoff
- Congestion capacity learning
- Fair-share bandwidth allocation
- Dynamic minimum transmission rate
- Transmission ceiling management

ABRCC's central design philosophy is to avoid treating every network condition identically.

Instead, it dynamically adjusts transmission behavior according to bandwidth availability, RTT, packet loss, queue conditions, and congestion evidence.

**ABRCC — Adaptive Bandwidth and RTT Congestion Control.**
