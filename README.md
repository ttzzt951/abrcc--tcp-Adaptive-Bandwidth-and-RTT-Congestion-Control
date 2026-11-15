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

When packet loss occurs—and is deemed eligible for compensation—the actual transmissio
