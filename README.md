# ABRCC 3.3 Full Technical Specification: Multi-Mode Adaptive TCP Congestion Control Algorithm

**ABRCC — Adaptive Bandwidth and RTT Congestion Control**

Version: 3.3
Platform: Linux Kernel
Type: TCP Congestion Control
Core Design: Multi-mode control, dynamic bandwidth probing, packet loss classification, congestion compensation, and RTT adaptation.

---

## 1. Project Introduction

ABRCC 3.3 is an experimental adaptive congestion control algorithm designed for the Linux TCP stack.

Unlike traditional, monolithic congestion control algorithms, ABRCC employs three primary operating modes. It dynamically adjusts its control strategy based on metrics such as packet loss rate, RTT, bandwidth estimation, and the degree of network congestion.

It integrates concepts from various existing algorithms—including CTCP, CUBIC, Veno, Westwood+, and BBR—while incorporating specialized mechanisms such as GECKO, SALAMANDER, HYBRIDIZATION, and CHIMERA.

The design objectives of ABRCC 3.3 include:

- Adapting to network environments with varying bandwidths.
- Adjusting transmission strategies based on packet loss conditions.
- Distinguishing between random packet loss and congestion-induced packet loss.
- Proactively probing for available bandwidth when network conditions permit.
- Implementing rate reduction and back-off during periods of congestion.
- Limiting total bandwidth usage across multiple connections via a fair-share mechanism.
- Adapting to RTT fluctuations and high-jitter network environments.

## 2. Three Primary Operating Modes

ABRCC 3.3 features three primary operating modes:

1. MODE_CTCP
2. MODE_BAND
3. MODE_HYBRID

These modes are not entirely independent algorithms; rather, they are selected automatically by a unified control framework based on the smoothed packet loss rate. The default packet loss thresholds are as follows:

| Parameter | Default Value | Percentage |
|---|---:|---:|
| loss_lo_bp | 30 bp | 0.3% |
| loss_hi_bp | 100 bp | 1% |
| ext_loss_bp | 3000 bp | 30% |

Here, "bp" stands for "basis points," where 100 bp equals 1%.

### 2.1 MODE_CTCP: Low Packet Loss Control Mode

**Trigger Conditions:**

- The smoothed packet loss rate is below 0.3%.
- If the system was previously in the BAND or HYBRID state, a hysteresis mechanism is applied to prevent an premature switch back to CTCP.

The default reference threshold for returning to CTCP from other modes is a packet loss rate below 0.21%.

Key Functions:

- CTCP delay-based window control.
- Adjustment of the sending rate based on RTT growth.
- Rate increases or decreases based on network latency feedback.
- Allows CUBIC to participate in probing under applicable conditions.
- Does not employ "Brutal"-style packet loss compensation.

CTCP uses the `gamma` parameter to control the delay-based window; the default value for `gamma` is 30 packets.

When latency feedback remains within an acceptable range, the algorithm increases the rate; when the RTT signal is significantly higher than the baseline (minimum RTT), the rate is reduced proportionally to the RTT.

### 2.2 MODE_BAND: Bandwidth-Adaptive Mode

**Trigger Conditions:**

- The smoothed packet loss rate reaches 0.3%.
- The conditions for entering the HYBRID mode have not yet been met.
- If the system was previously in a mode other than CTCP, a hysteresis reference value of 0.21% applies for exiting this mode.

Key Functions:

- Bandwidth estimation and dynamic rate adjustment.
- Westwood+ bandwidth estimation.
- Packet loss compensation.
- Dynamic control of the compensation intensity.
- Coordinated control using both CUBIC and Veno.
- Rate reduction during congestion. Default Back-off Mechanism:

When a standard back-off is required, the sending rate is adjusted as follows:

`rate = rate × 7/8`

This results in a reduction of approximately 12.5% ​​per occurrence, with a default limit of at most one such adjustment per second.

If Veno identifies a packet loss as random (non-congestion-related), no congestion back-off is triggered.

### 2.3 MODE_HYBRID: High-Loss Hybrid Mode

**Trigger Conditions:**

- The smoothed packet loss rate exceeds 1%.
- If already in the HYBRID state, a hysteresis threshold of 0.7% applies.

Key Functions:

- Enhanced CTCP growth step.
- Multi-algorithm participation in control.
- "Brutal"-style rate control.
- Dynamic packet loss compensation.
- Management of the upper limit for the sending rate.
- High-speed state switching.

When in the internal CTCP state, the HYBRID mode multiplies the CTCP growth step by 4.

Note that this does not mean the final sending rate is multiplied by 4; rather, it is the growth increment that is scaled.

When the criteria for entering the HIGH state are met, the algorithm switches to `ST_HIGH`.

---

## 3. Internal Control States

ABRCC utilizes two additional internal control states.

### 3.1 ST_CTCP

This is the fundamental probing state.

Primary responsibilities include:

- Probing for available bandwidth.
- Adjusting the sending rate based on RTT.
- Executing CTCP growth.
- Incorporating CUBIC when applicable.
- Evaluating whether the conditions for entering the HIGH state are met.

### 3.2 ST_HIGH

This is a "Brutal"-style high-speed control state.

Entry requires that all of the following conditions are met:

1. The current primary mode is `MODE_HYBRID`.
2. The current rate has reached the `H` threshold.
3. The back-off wait period has elapsed.
4. The system is not in a state of extreme congestion.

The default calculation for `H` is:

`H = B × (target - 50) / target`

Where `B` represents the upper limit of the connection's fair share of bandwidth. For example, when the server's target bandwidth is 200 Mbps, the "H" (HIGH) state defaults to 75% of the current fair share.

Key functions of ST_HIGH:

- Adjusts the sending rate toward the fair-share target.
- Limits the rate using a learned "ceiling."
- Performs dynamic packet-loss compensation.
- Monitors RTT and transmission overload.
- Falls back to ST_CTCP based on network conditions.

If three consecutive control rounds are deemed anomalous, the rate is reduced to 7/8 of its current value, and the algorithm reverts to CTCP.

If a failure occurs while in the HIGH state, the backoff wait time increases incrementally, up to a default maximum of 30 seconds.

---

## 4. Four Special Mechanisms

The most distinctive design elements of ABRCC 3.3 are its four special mechanisms:

- GECKO
- SALAMANDER
- HYBRIDIZATION
- CHIMERA

These are not separate primary operating modes, but rather specialized probing or control mechanisms.

### 4.1 GECKO: Startup Burst Probing Mechanism

Default state: Enabled.

Trigger conditions:

- Completion of TCP connection initialization.
- Entry into the startup phase following connection establishment.

Default duration: 5 seconds.

Operational logic:

1. Utilizes the full fair-share budget allocated to the connection during the startup phase.
2. Temporarily disregards packet loss and standard congestion control feedback.
3. Records the peak delivery rate observed during the startup phase.
4. Transitions back to standard CTCP control once the startup phase concludes.

Rate after the startup phase:

`rate = max(peak / 2, 8 Mbps)`

Subsequently, the rate is subject to minimum rate constraints and the fair-share upper limit.

Special case: If the startup phase fails to reach 8 Mbps, the rate will not be reduced below the peak delivery rate actually achieved.

GECKO also clears the packet-loss and queue history accumulated during the startup phase, preventing the initial burst from skewing subsequent control states.

### 4.2 SALAMANDER: Stepped Bandwidth-Surge Mechanism

Default state: Enabled (requires `sal_on=1`). SALAMANDER determines the target for the next probing phase based on the best delivery rate observed during the connection's history.

Default Steps:

| Existing Effective Bandwidth | Next Target |
|---|---:|
| Below 30 Mbps | Disable step-based probing; use HYBRIDIZATION |
| 30–50 Mbps | 50 Mbps |
| 50–100 Mbps | 100 Mbps |
| 100–150 Mbps | 150 Mbps |
| 150–200 Mbps | 200 Mbps |
| Target Reached | Step completed |

Conditions for Initiating a Probe:

- Currently in the `ST_CTCP` state.
- Current primary mode is `MODE_CTCP`.
- RTT is within an acceptable range.
- The connection is active.
- Not currently in `PROBE_RTT`.
- No extreme congestion is present.
- There is still sufficient room for rate growth.
- The current target has not been reached for at least 10 minutes.
- An interval of 6–7 minutes has elapsed since the last probe.

Default Probing Parameters:

| Parameter | Default Value |
|---|---:|
| `sal_idle_s` | 600 seconds |
| `sal_gap_min_s` | 360 seconds |
| `sal_gap_max_s` | 420 seconds |
| `sal_dur_s` | 30 seconds |
| `sal_ramp_s` | 6 seconds |
| `sal_tries` | 3 attempts |
| `sal_reach_pct` | 95% |

Operational Mechanism:

- Employs a linear growth approach to transition from the pre-probe rate toward the target rate.
- Incorporates candidate rates derived from CTCP, CUBIC, Westwood+, and BBR methodologies.
- Candidate growth is constrained by a maximum 2x increase per round and the target bandwidth.

Failure Conditions:

1. The smoothed packet loss rate increases by at least 2 percentage points compared to the rate at the start of the probe. 2. Extreme congestion occurs.
3. The state transitions to HIGH.
4. Queue delay reaches a minimum of 10 ms or 50% of the base RTT.
5. Transmission overload occurs.

These anomalies must persist for approximately two RTTs—and at least 200 ms—before a probe is deemed a failure.

Failure Handling:

- Revert to the rate used prior to the start of the probe.
- Limit the rate to the vicinity of the previous probe rate.
- Initiate a random cooldown period of 150–210 seconds.
- Resume normal control.

Success Condition:

The delivery rate reaches 95% of the target and remains at that level for at least 1 second.

SALAMANDER retains up to three probe records. Each record includes the start time, duration, base rate, target rate, peak rate, packet loss rate, and queue delay.

### 4.3 HYBRIDIZATION: Low-Bandwidth Hybrid Probing Mechanism

Default State: Enabled via `sal_on`.

Trigger Conditions:

- The best historical delivery rate is below 30 Mbps.
- Currently in the `ST_CTCP` state.
- The connection is actively transmitting data.
- Not currently in `PROBE_RTT`.
- No extreme congestion is present.
- Sufficient headroom exists for bandwidth growth.
- At least 6 minutes have elapsed since the last probe.

Default Parameters:

| Parameter | Default Value |
|---|---:|
| `sal_off_mbps` | 30 Mbps |
| `sal_hyb_gap_s` | 360 seconds |
| `sal_hyb_s` | 20 seconds |
| `sal_hyb_tries` | 3 attempts |
| Maximum Probe Target | 50 Mbps |

Operational Logic:

1. Use a growth step four times larger than that of CTCP.
2. Suspend standard packet-loss-based backoff.
3. Incorporate a CUBIC-based candidate.
4. Incorporate a Westwood+-based bandwidth estimation candidate.
5. Incorporate a BBR-style bandwidth gain candidate.
6. Compare the candidate rates and select the highest value. Unlike SALAMANDER, HYBRIDIZATION does not require the current primary mode to be MODE_CTCP, nor does it mandate that the RTT be within a normal range.

It is primarily designed for hybrid probing under low-bandwidth conditions.

Failure conditions:

- Severe queuing delay.
- Transmission overload.
- Extreme congestion.

Upon failure, the system reverts to the rate used prior to the probe. Unlike SALAMANDER, it does not trigger a failure solely due to an increase in packet loss, nor does it employ the same 150–210 second "cooldown" period following a failure.

Each probe lasts a maximum of approximately 20 seconds, with up to three attempts permitted; each attempt—regardless of whether it succeeds or fails—counts toward this limit.

### 4.4 CHIMERA: Multi-Algorithm Parallel Candidate Mechanism

CHIMERA is an independent, optional mechanism within ABRCC.

**Disabled by default: `chimera=0`.**

To enable:

`chimera=1`

Trigger conditions:

- Peak packet loss rate reaches 10%.
- The Veno random packet loss indicator reaches 60%.
- No evidence of congestion.
- No state of extreme congestion.
- These conditions persist for a default duration of 16 control rounds.

It incorporates four candidates:

- CTCP
- CUBIC
- Westwood+
- BBR-style bandwidth gain

The algorithm selects the highest rate among these candidates.

A specific eight-round cycle is applied:

| Round | Action |
|---|---|
| Round 0 | BBR-style probe, gain = 1.25 |
| Round 1 | BBR DRAIN, capped at 0.75x bandwidth |
| Other Rounds | Standard candidate "cruising," gain = 1.0 |

During the probing phase, the growth of any candidate is further limited to a maximum of 2x per round.

If an occurrence...
