# ABRCC 3.3: A Linux TCP Congestion Control Algorithm for Adaptive Bandwidth and RTT Control

**ABRCC — Adaptive Bandwidth and RTT Congestion Control**

Version: 3.3
Project Type: Linux TCP Congestion Control
Focus: Adaptive bandwidth, RTT management, and hybrid multi-algorithm control

## 1. Project Overview

ABRCC (Adaptive Bandwidth and RTT Congestion Control) is an experimental congestion control algorithm designed for Linux TCP.

Its objective is to achieve adaptive control across diverse network environments by leveraging bandwidth estimation, RTT feedback, congestion state identification, and a combination of multiple algorithms.

Unlike traditional approaches that rely on a single congestion control strategy, ABRCC 3.3 employs a multi-mode architecture and incorporates specialized probing mechanisms, allowing the algorithm to apply distinct control strategies based on varying bandwidth conditions and network states.

Rather than being confined to a single method for congestion window growth, ABRCC integrates the characteristics of various congestion control algorithms to establish a more flexible control framework.

## 2. Three Primary Operating Modes

ABRCC 3.3 features three main operating modes.

### 2.1 MODE_CTCP

MODE_CTCP is not merely a simple implementation of CTCP; rather, it functions as a hybrid mode that combines multiple algorithms.

Its design incorporates:

- CTCP
- CUBIC
- Veno
- Westwood+
- BBR-style bandwidth filtering and rate referencing
- A "ceiling" (upper limit) constraint mechanism

This mode aims to balance bandwidth growth, congestion detection, and transmission rate limiting by synthesizing the control characteristics of these various algorithms.

### 2.2 MODE_BAND

MODE_BAND is an operating mode centered on bandwidth estimation and adaptive control.

It focuses primarily on the network's available bandwidth, addressing fluctuations in bandwidth, rate adjustments, and the management of transmission upper limits.

The goal of this mode is to dynamically adjust transmission behavior based on current network conditions, moving away from a reliance on a static, fixed growth strategy. ### 2.3 MODE_HYBRID

MODE_HYBRID is a hybrid operating mode that encompasses two internal control states:

- ST_CTCP
- ST_HIGH

This mode dynamically switches between these control states based on network conditions, allowing the algorithm to balance the requirements for standard control with those for higher transmission rates.

## 3. Four Special Mechanisms

The most distinctive design elements of ABRCC 3.3 are its four special mechanisms:

GECKO, SALAMANDER, HYBRIDIZATION, and CHIMERA.

Rather than serving as additional primary operating modes, these function as specialized probing or control mechanisms tailored for specific network conditions.

### 3.1 GECKO: Rapid-Start Probing

GECKO is designed primarily for bandwidth discovery during the initial phase of a connection.

It incorporates an initial burst phase lasting approximately 5 seconds, intended to rapidly explore available bandwidth as soon as a connection is established.

Unlike approaches that maintain high-speed transmission continuously, GECKO also manages the transition to subsequent control states once the initial phase concludes, preventing excessive, prolonged probing.

### 3.2 SALAMANDER: Stepped-Sprint

SALAMANDER employs a "stepped-sprint" strategy—a method for increasing transmission speed in stages.

Instead of jumping directly to a target transmission rate, it explores network capacity by incrementally raising the rate in distinct steps.

This design is intended to mitigate the risk of congestion that could arise from a sudden, large-scale increase in transmission speed.

### 3.3 HYBRIDIZATION: Low-Bandwidth Hybrid Probing

HYBRIDIZATION is a specialized mechanism designed for low-bandwidth environments.

According to current design specifications, it is triggered when bandwidth falls below 30 Mbps.

It integrates probing strategies from various algorithms to improve both available bandwidth discovery and rate adjustment in low-bandwidth scenarios.

### 3.4 CHIMERA: Parallel Candidate-Algorithm Probing

CHIMERA is one of the most unique special mechanisms in ABRCC 3.3.

It utilizes a parallel approach, where multiple candidate algorithms perform probes simultaneously; the system then compares the results from these candidates to make a selection.

Unlike traditional control schemes that rely on a single, fixed algorithm to generate probing results, CHIMERA leverages multiple candidates. CHIMERA also incorporates a multi-parameter multiplier-based growth design, with settings such as 30x and 4x multipliers discussed.

These multipliers are part of the internal parameter design and should not be interpreted as a guarantee that the final TCP transmission rate will increase by the exact same factor.

## 4. Bandwidth and RTT Management

In addition to its three primary operating modes and four specialized mechanisms, ABRCC inherits several control designs established in previous versions.

These include:

- Global fair-share management
- Bandwidth maximum filtering
- Minimum RTT refresh
- PROBE_RTT-lite
- High-jitter handling
- Time-based backoff
- Dynamic compensation scaling
- Congestion capacity learning
- Dynamic minimum rate

These mechanisms address bandwidth estimation, RTT management, congestion recovery, and transmission rate control.

They are designed to mitigate the destabilizing effects that fluctuations in network conditions can have on transmission behavior.

## 5. Design Significance of the Multi-Mode Architecture

The core concept behind ABRCC 3.3 is to organize various control strategies within a unified algorithmic framework.

The three primary operating modes handle the main control responsibilities, while the four specialized mechanisms provide additional probing and control capabilities tailored to specific conditions.

This architecture is characterized by the following:

1. Multi-mode: It is not restricted to a single operating mode.
2. Multi-algorithm: It seeks to integrate the strengths of different congestion control algorithms.
3. Adaptive probing: It employs different probing strategies based on varying network conditions.
4. Parallel candidates: CHIMERA introduces a framework for comparing multiple algorithmic candidates in parallel.
5. Dynamic control: It adjusts transmission behavior by incorporating bandwidth, RTT, and congestion feedback.

## 6. Future Research Directions

The ongoing development of ABRCC involves not only the addition of new algorithmic mechanisms but also more rigorous performance validation.

Areas worthy of further research include:

- Fairness comparisons with Linux CUBIC, BBR, and Reno.
- Throughput performance under varying RTT conditions.
- Stability in environments with random or bursty packet loss.
- Convergence speed during dynamic bandwidth fluctuations.
- Fairness in scenarios involving competition among multiple connections. - Performance under various queue management mechanisms.
- The balance between probing for capacity and slowing down during congestion.

## 7. Project Summary

ABRCC 3.3 is an experimental Linux TCP congestion control project that employs a multi-mode approach, a combination of multiple algorithms, and specialized probing mechanisms.

Its core design consists of three primary operating modes and four specialized mechanisms:

**Primary Operating Modes:**
- MODE_CTCP
- MODE_BAND
- MODE_HYBRID

**Specialized Mechanisms:**
- GECKO
- SALAMANDER
- HYBRIDIZATION
- CHIMERA

ABRCC aims to explore a more flexible TCP congestion control architecture, allowing different control strategies to be applied effectively based on the specific network environment.

The project requires further validation—through the release of source code, reproducible testing, and comparative experiments across diverse network environments—to confirm the efficacy of its design.

**ABRCC — Adaptive Bandwidth and RTT Congestion Control.**
