# ABRCC 3.3 — Adaptive Bandwidth and RTT Congestion Control

**Adaptive Bandwidth and RTT Congestion Control Algorithm for Linux TCP**

Version: 3.3
Platform: Linux Kernel
Type: TCP Congestion Control
Core Technologies: Brutal, CTCP, CUBIC, Veno, Westwood+, BBR-style bandwidth estimation, and multi-mode control.

---

## 1. Project Introduction

ABRCC (Adaptive Bandwidth and RTT Congestion Control) is an experimental congestion control algorithm designed for Linux TCP.

ABRCC 3.3 employs a multi-mode control architecture. By integrating bandwidth estimation, RTT latency feedback, packet loss classification, dynamic loss compensation, and multi-algorithm candidate probing, it aims to adapt to diverse network environments characterized by varying bandwidth, latency, and packet loss rates.

Unlike traditional congestion control algorithms that rely on a single strategy, ABRCC incorporates multiple control mechanisms into a unified framework, dynamically adjusting the transmission rate based on current network conditions.

Key components include:

- Brutal: Packet loss compensation mechanism
- CTCP: Latency-based window control
- CUBIC: Rate probing
- Veno: Random packet loss identification
- Westwood+: Bandwidth estimation
- BBR-style: Bandwidth filtering
- BBRv3-style: Rate upper-bound management
- Multi-connection fair-share allocation
- GECKO: Startup burst handling
- SALAMANDER: Stepped probing
- HYBRIDIZATION: Low-bandwidth probing
- CHIMERA: Parallel candidate probing

## 2. Three Primary Operating Modes

ABRCC 3.3 operates in three main modes:

1. MODE_CTCP
2. MODE_BAND
3. MODE_HYBRID

Mode selection is primarily based on the smoothed packet loss rate, utilizing a hysteresis mechanism to minimize frequent switching between modes. Default Thresholds:

| Parameter | Default Value | Purpose |
|---|---:|---|
| loss_lo_bp | 30 bp | 0.3%; threshold for entering BAND mode |
| loss_hi_bp | 100 bp | 1%; threshold for entering HYBRID mode |
| ext_loss_bp | 3000 bp | 30%; threshold for extreme congestion/packet loss |

### 2.1 MODE_CTCP

**Trigger Condition: Smoothed packet loss rate is below 0.3%.**

This control mode is based on the CTCP (Compound TCP) delay window.

Key Functions:

- Adjusts the sending rate based on RTT (Round-Trip Time).
- Explores available bandwidth using the CTCP growth mechanism.
- Incorporates the CUBIC curve for supplementary probing.
- Uses the minimum RTT as a delay reference.
- Adjusts the sending rate based on queueing delay.
- Does not enable Brutal packet loss compensation.

Default CTCP Parameters:

- gamma_pkts: 30
- cwnd_gain: 20 (applied as a factor of 2.0, i.e., divided by 10)

A hysteresis mechanism is employed when returning from other modes to prevent frequent switching caused by fluctuations in the packet loss rate around the 0.3% mark.

### 2.2 MODE_BAND

**Trigger Condition: Smoothed packet loss rate reaches 0.3%.**

Key Functions:

- Westwood+ bandwidth estimation.
- Brutal packet loss compensation.
- CUBIC supplementary probing.
- Veno-based random packet loss identification.
- Dynamic sending rate adjustment.
- Congestion backoff.
- Bandwidth upper-limit management.

Standard congestion backoff involves a rate adjustment by a factor of 7/8 (a reduction of approximately 12.5%), typically applied at most once per second.

If Veno identifies the packet loss as random, the algorithm can avoid triggering a congestion backoff that would otherwise occur due to standard random packet loss.

### 2.3 MODE_HYBRID

**Trigger Condition: Smoothed packet loss rate reaches 1%.** **

This is the hybrid control mode for high packet-loss scenarios in ABRCC.

Key features:

- Hybrid probing using CTCP and CUBIC.
- "Brutal" packet-loss compensation.
- Bandwidth estimation via Westwood+.
- BBR-style bandwidth gain.
- Dynamic compensation intensity.
- "HIGH" speed control state.
- Management of the upper limit for transmission rates.

HYBRID incorporates two internal control states:

- ST_CTCP
- ST_HIGH

ST_CTCP handles probing, while ST_HIGH manages high-speed control using the "Brutal" approach.

In the HYBRID ST_CTCP state, the CTCP growth step can be multiplied by 4.

Note that this refers to the multiplier for the growth increment, not a direct fourfold increase in the overall transmission rate.

---

## 3. Brutal: The Core Packet-Loss Compensation Mechanism

"Brutal" is a vital core component of ABRCC 3.3 and a key design element that distinguishes this algorithm from traditional congestion-window growth strategies.

It employs a target transmission rate and packet-loss compensation logic, attempting to increase the transmission volume when data loss occurs in order to maintain the expected data delivery rate.

### 3.1 How Brutal Works

Standard TCP congestion control typically reduces the transmission rate following congestion-induced packet loss.

Brutal, by contrast, takes a different approach:

When packet loss occurs—and is deemed eligible for compensation—the actual transmission rate is increased based on the packet-loss rate to compensate for the potentially lost data.

For example:

Suppose the base transmission rate is 100 Mbps, with a compensation rate of 10%.

The theoretical transmission rate would then be:

100 / (1 - 0.10) = 111.11 Mbps

This allows the system to maintain an effective delivery target of approximately 100 Mbps, even in the presence of a certain degree of packet loss. ### 3.2 ABRCC Brutal Compensation Calculation

ABRCC uses the following parameters:

| Parameter | Default Value | Function |
|---|---:|---|
| max_comp_bp | 5000 | Maximum compensation cap (50%) |
| comp_pct | 0–100 | Dynamic compensation intensity |
| loss_s | Dynamically calculated | Smoothed packet loss rate |

Compensation rate calculation:

`comp = min(loss_s, max_comp_bp) × comp_pct / 100`

Actual transmission rate (on the wire):

`send_rate = base_rate / (1 - comp)`

This is ultimately subject to a hard limit defined by the server's fair-share bandwidth budget, $B$.

### 3.3 Dynamic Compensation Mechanism

ABRCC does not always apply compensation at maximum intensity.

When evidence of congestion is detected:

- Compensation intensity is halved.
- The volume of additional data sent is reduced.
- Priority is given to controlling queue buildup and transmission overload.

When there is no evidence of congestion:

- Compensation intensity increases by 10 units per control cycle.
- It can recover up to a maximum of 100%.

### 3.4 Conditions for Enabling/Disabling Brutal

| Operating Condition | Brutal Compensation |
|---|---|
| MODE_CTCP | Disabled |
| MODE_BAND | Enabled |
| MODE_HYBRID | Enabled |
| Extreme Congestion | Disabled |
| PROBE_RTT-lite | Disabled |
| GECKO Startup Phase | Disabled |

Brutal is not a mechanism that ignores congestion unconditionally.

It remains constrained by RTT, delivery rate, the ceiling, the server's bandwidth budget, and safeguards against extreme congestion.

---

## 4. Internal Control States

### 4.1 ST_CTCP

The base probing state.

Responsible for:

- CTCP delay-based window control.
- CUBIC-style auxiliary growth.
- Network bandwidth probing.
- Monitoring RTT variations. - Determine whether the conditions for entering the HIGH state are met.

### 4.2 ST_HIGH

A high-speed control state following the "Brutal" approach.

Entry conditions:

1. The current primary mode is MODE_HYBRID.
2. The CTCP detection rate has reached the H threshold.
3. The backoff wait period has elapsed.
4. There is no extreme congestion.

The default H threshold is calculated based on the fair-share bandwidth ($B$) and the `hold_drop_mbps` parameter.

The default value for `hold_drop_mbps` is 50 Mbps.

Primary functions of ST_HIGH:

- Drive the sending rate toward the compensated fair-share target.
- Maintain rate control based on the "Brutal" approach.
- Limit the rate using a "learned ceiling."
- Monitor for transmission overload.
- Fall back to the ST_CTCP state based on network conditions.

If three consecutive anomalous control rounds occur while in the HIGH state, the rate is reduced, and the system reverts to the ST_CTCP state.

---

## 5. Four Special Mechanisms

### 5.1 GECKO: Startup Burst Probing

Enabled by default.

Trigger condition: The startup phase following the completion of the TCP handshake.

Default duration: 5 seconds.

Mechanism:

- Utilize the full fair-share budget during the startup phase.
- Temporarily ignore packet-loss feedback.
- Record the best delivery rate achieved during the startup phase.
- Upon conclusion of the startup phase, adjust the rate to half of the best delivery rate recorded.

The default minimum rate following the startup phase is 8 Mbps; however, this value cannot be lower than the best delivery rate actually achieved, and it remains subject to the fair-share limit.

Subsequently, the packet-loss and queue history accumulated during the startup phase are cleared.

### 5.2 SALAMANDER: Stepped Bandwidth Sprint

Enabled by default (requires `sal_on=1`). Select the target for the next phase based on the best historical delivery rate:

| Best Historical Delivery Rate | Next Target |
|---|---:|
| Below 30 Mbps | Handled via HYBRIDIZATION |
| 30–50 Mbps | 50 Mbps |
| 50–100 Mbps | 100 Mbps |
| 100–150 Mbps | 150 Mbps |
| 150–200 Mbps | 200 Mbps |
| Reached 200 Mbps | Default progression completed |

Probing Conditions:

- Currently in the ST_CTCP state.
- Current primary mode is MODE_CTCP.
- RTT meets the requirements.
- No extreme congestion.
- No transmission overload.
- Current target has not been reached for at least 10 minutes.
- An interval of 6–7 minutes has elapsed since the last probe.

The default probe duration is 30 seconds, with a 6-second linear growth phase.

Up to three probe attempts are permitted for each target.

Upon failure:

- Revert to the rate prior to the probe.
- Impose a ceiling limit.
- Enter a cooldown period (randomized between 150–210 seconds).
- Log the probe result.

Success is defined as achieving a delivery rate of at least 95% of the target, sustained for a minimum of 1 second.

### 5.3 HYBRIDIZATION: Low-Bandwidth Hybrid Probing

Controlled by `sal_on` by default.

Trigger Conditions:

- Best historical delivery rate is below 30 Mbps.
- Currently in the ST_CTCP state.
- No extreme congestion.
- No transmission overload.
- Probe interval requirement is met.

Default Parameters:

- Probe interval: 360 seconds.
- Probe duration: 20 seconds.
- Maximum number of probe attempts: 3.
- Maximum probe target: 50 Mbps.

Operational Mechanism:

- Multiply the CTCP growth step by 4.
- Suspend standard packet-loss-based backoff.
- Simultaneously incorporate CUBIC as a candidate. - Added Westwood+ bandwidth estimation.
- Added a BBR-style bandwidth gain candidate.
- Selects the maximum candidate rate.

Failure conditions are primarily defined by severe queue latency or transmission overload, rather than just an increase in packet loss.

### 5.4 CHIMERA: Multi-Algorithm Parallel Candidate Mechanism

Disabled by default:

`chimera=0`

Enabled:

`chimera=1`

Trigger conditions:

- Peak packet loss rate reaches the default threshold of 10%.
- Veno identifies the loss as random (with `rand_pct` reaching 60).
- No evidence of congestion exists.
- The system is not in a state of extreme congestion.
- These conditions persist for at least 16 control rounds.

Participating candidate mechanisms:

- CTCP
- CUBIC
- Westwood+
- BBR-style bandwidth gain

The algorithm calculates multiple candidate rates simultaneously and selects the highest value.

CHIMERA operates on an eight-round cycle:

- Round 0: BBR-style probing, with a gain of 1.25.
- Round 1: DRAIN phase, with the rate limited to 0.75x the estimated bandwidth.
- Other rounds: Standard candidate cruising.

Candidate growth remains subject to per-round growth limits, a ceiling, overload detection, and server budget constraints.

3.3 Fixed an issue where the DRAIN phase failed to effectively reduce the rate, ensuring the 0.75x bandwidth limit is actually applied.

---

## 6. Packet Loss Identification and the Veno Mechanism

ABRCC does not automatically classify all packet loss as congestion.

It attempts to distinguish between:

- Random packet loss.
- Congestion-related packet loss.
- Extreme congestion.

Packet loss statistics require a minimum of 100 samples by default.

The smoothed packet loss rate is calculated as:

`loss_s = (old_loss_s × 3 + instant_loss) / 4`

Loss of an entire window during an RTO (Retransmission Timeout) event is not factored into standard packet loss statistics. ### Veno Style Determination

`rand_pct` represents the tendency for random packet loss:

| rand_pct | Determination |
|---|---|
| 60–1 | |
