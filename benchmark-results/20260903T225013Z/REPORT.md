# Local latency benchmark report

Run directory: `20260903T225013Z` (UTC).

## Result

The implementation sustained all three requested profiles without a growing latency trend. End-to-end latency grew
roughly with the number of events in a publication batch: the aggregate median was 5.80 ms at 3,000 events/s,
24.62 ms at 15,000 events/s, and 50.66 ms at 30,000 events/s. At the largest profile, aggregate p99 was 70.47 ms
and the maximum observed latency was 83.29 ms.

These numbers are not network-only latency. The timestamp is taken before the server updates the reusable event
objects, so the measurement includes server-side batch preparation, `publishEvents`, Graal/QDS serialization,
loopback transport, receive dispatch, and entry into the C++ listener.

## Method

One server and one client ran as separate Release processes on the same machine. Each profile used a 30-second
warm-up, a 180-second measurement, 10-second reporting windows, and a 30-second incomplete-batch timeout. The server
published one batch per second.

```text
latency_server.exe --address :7400

latency_client.exe --address 127.0.0.1:7400 \
  --task "SUB:Q1000;T1000;S1000" --warmup 30s --duration 3m --window 10s --batch-timeout 30s \
  --output benchmark-results/20260903T225013Z/q1k-t1k-s1k

latency_client.exe --address 127.0.0.1:7400 \
  --task "SUB:Q5000;T5000;S5000" --warmup 30s --duration 3m --window 10s --batch-timeout 30s \
  --output benchmark-results/20260903T225013Z/q5k-t5k-s5k

latency_client.exe --address 127.0.0.1:7400 \
  --task "SUB:Q10000;T10000;S10000" --warmup 30s --duration 3m --window 10s --batch-timeout 30s \
  --output benchmark-results/20260903T225013Z/q10k-t10k-s10k
```

## Aggregate latency

All values are milliseconds. `event` is the combined distribution of Quote, Trade, and Summary samples. `batch`
measures the observation time of the last event belonging to a complete batch.

| Profile | Events/s | Samples | p50 | p95 | p99 | p99.9 | Maximum | Batch p50 | Batch p99 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Q1k/T1k/S1k | 3,000 | 540,000 | 5.80 | 8.03 | 13.36 | 15.84 | 16.15 | 6.81 | 13.53 |
| Q5k/T5k/S5k | 15,000 | 2,700,000 | 24.62 | 33.19 | 37.25 | 49.46 | 51.19 | 29.63 | 50.15 |
| Q10k/T10k/S10k | 30,000 | 5,430,000 | 50.66 | 66.20 | 70.47 | 80.33 | 83.29 | 65.23 | 76.65 |

Relative to the 3,000-events/s profile, aggregate p50 increased by 4.25x at 15,000 events/s and by 8.74x at
30,000 events/s. Aggregate p95 increased by 4.13x and 8.24x respectively. This is close to linear scaling over the
tested range; there is no abrupt knee indicating saturation.

## Latency by event type

The table shows p50 / p99 in milliseconds.

| Profile | Quote | Trade | Summary |
|---|---:|---:|---:|
| Q1k/T1k/S1k | 4.87 / 12.75 | 5.75 / 14.02 | 6.55 / 14.76 |
| Q5k/T5k/S5k | 20.61 / 30.20 | 24.26 / 34.13 | 27.61 / 41.52 |
| Q10k/T10k/S10k | 42.40 / 56.94 | 49.96 / 65.44 | 58.21 / 74.57 |

Quote consistently arrives first, Trade next, and Summary last. The gap grows with each per-type population: at
10,000 instruments per type, the median gaps are about 7.6 ms from Quote to Trade and 8.2 ms from Trade to Summary.
This is consistent with ordered work inside a large publication/serialization pipeline, not a symbol-specific
latency effect.

## Window stability and integrity

Window ranges below are aggregate event latencies in milliseconds. There were 18 windows per profile.

| Latency metric | Q1k/T1k/S1k | Q5k/T5k/S5k | Q10k/T10k/S10k |
|---|---:|---:|---:|
| Window p50 minimum | 5.06 | 23.61 | 48.97 |
| Window p50 median | 5.83 | 24.57 | 50.89 |
| Window p50 maximum | 6.62 | 26.98 | 54.27 |
| Window p99 minimum | 6.56 | 29.62 | 66.61 |
| Window p99 median | 8.41 | 36.88 | 69.39 |
| Window p99 maximum | 16.15 | 49.25 | 80.17 |

| Integrity metric | Q1k/T1k/S1k | Q5k/T5k/S5k | Q10k/T10k/S10k |
|---|---:|---:|---:|
| Complete batches | 180 | 180 | 181 |
| Callbacks | 2,229 | 7,578 | 14,617 |
| Clock anomalies | 0 | 0 | 0 |
| Incomplete batches | 0 | 1 | 0 |

Every complete batch contributed exactly the configured number of samples: 540,000, 2,700,000, and 5,430,000
events respectively. The largest profile recorded 181 rather than 180 batches because the first/last one-second tick
and the 180-second client deadline fell on opposite sides of a boundary; its 10-second windows consequently contain
9 to 11 complete batches.

The middle profile diagnosed one partial batch (`sequence=31`, `5162/15000` events, marker present) while still
recording 180 complete batches. Its timing and sequence are consistent with a delivery split across the transition
from warm-up to measurement, where pre-measurement fragments are deliberately discarded. It did not reduce the
expected complete-batch sample total, but another run would be needed to prove that this was only a boundary effect.

There is no evidence of a growing backlog at 30,000 events/s: window medians remain within 48.97-54.27 ms and the
run ends with zero pending batches. The higher p99/max values appear as occasional whole callback or batch delays,
rather than isolated slow symbols.

## Outliers

Outliers use `Q3 + 1.5 * IQR` independently for each 10-second window and event type. The outlier CSV files therefore
contain type-specific rows (`event-quote`, `event-trade`, and `event-summary`) plus batch rows. They are diagnostic
samples, not failures, and counts are not directly comparable between profiles because each window has its own
threshold. The aggregate `event` distribution is retained in the summary only so that the same event is not exported
twice.

## Environment and limitations

- CPU: AMD Ryzen 9 5950X, 16 cores / 32 logical processors.
- RAM: 128 GiB.
- OS: Windows 11 Pro Insider Preview, version 10.0.26340, build 26340.
- Build: C++23 Release, MSVC 19.51.36256.0, CMake 4.4.2, Visual Studio 18 2026 generator.
- dxFeed Graal C++ API: v7.0.0; runtime protocol log reports QDS 3.342.
- Source base: Git commit `80583e0175a8dc9eb9436778bccbe1569fcf06f0`, with the reporting changes in the
  working tree.

This is a single run per profile on loopback, without CPU affinity, process-priority control, or isolation from
background OS activity. It establishes the behavior of this machine and build, not a production SLA. For stronger
capacity conclusions, repeat each profile several times, randomize their order, record CPU utilization, and add
profiles above 30,000 events/s until latency or pending-batch count shows a clear saturation knee.
