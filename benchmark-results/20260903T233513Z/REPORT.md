# Local latency benchmark with QD monitoring

Run directory: `20260903T233513Z` (UTC).

## Result

QD monitoring confirms that every steady-state profile delivered its nominal record rate without observable
backpressure. The server's Write data rate and the client's Read data rate matched within the reporting-interval
jitter, while `Buffer` and `Dropped` remained zero on both processes. The largest profile sustained about 30,002
data records/s and 0.829 MiB/s with aggregate event p50 of 48.99 ms and p99 of 69.69 ms.

The test still does not expose a saturation knee at 30,000 events/s. `STREAM_FEED` prevents ticker conflation, but it
does not make delivery unbounded: QD's stream agent buffer uses `DROP_OLDEST` by default and increments `Dropped` if
the finite buffer overflows. Zero drops in this run therefore provide useful evidence that the consumer kept up.

## Method

Each profile used separate Release server and client processes on the same machine. Both endpoints ran with
`monitoring.stat=10s`. Each client used a 30-second warm-up, a 180-second measurement, 10-second latency windows,
and a 30-second incomplete-batch timeout. Monitoring aggregates include only the 17 reporting intervals whose full
time range was inside the measurement phase; boundary intervals remain in `monitoring.csv`.

```text
latency_server.exe --address :7400 --monitoring-stat 10s

latency_client.exe --address 127.0.0.1:7400 --task "SUB:Q1000;T1000;S1000" \
  --warmup 30s --duration 3m --window 10s --batch-timeout 30s --monitoring-stat 10s --output <prefix>
```

The other profiles replaced each per-type count with 5,000 and 10,000. The timestamp is taken before the server
updates the reusable event objects, so latency includes batch preparation, `publishEvents`, Graal/QDS serialization,
loopback transport, receive dispatch, and entry into the C++ listener.

## QD throughput and lag

Values are means over the selected monitoring intervals. I/O volume uses MiB/s; lag and RTT use milliseconds.
Subscription contains the market-event records plus one control `TextMessage` record.

| Monitoring metric | Q1k/T1k/S1k | Q5k/T5k/S5k | Q10k/T10k/S10k |
|---|---:|---:|---:|
| Nominal event records/s | 3,000 | 15,000 | 30,000 |
| Server Write data records/s | 3,000.65 | 15,000.18 | 30,002.24 |
| Client Read data records/s | 3,000.65 | 15,000.18 | 30,002.24 |
| Server Write / client Read MiB/s | 0.080 | 0.415 | 0.829 |
| Subscription records | 3,001 | 15,001 | 30,001 |
| Server Write data lag | 0.069 | 0.104 | 0.065 |
| Client Read data lag | 0.823 | 3.934 | 7.781 |
| Client RTT | 0.754 | 3.830 | 7.717 |

Server-side write lag stays below 0.11 ms, while client-side read lag grows approximately with the batch size. QD's
read lag is still much smaller than the end-to-end latency below, which is expected because the test timestamp also
covers server event updates and other stages outside that internal counter.

| Resource/backpressure metric | Q1k/T1k/S1k | Q5k/T5k/S5k | Q10k/T10k/S10k |
|---|---:|---:|---:|
| Server CPU mean / max | 0.014% / 0.030% | 0.100% / 0.120% | 0.154% / 0.170% |
| Client CPU mean / max | 0.021% / 0.040% | 0.114% / 0.140% | 0.164% / 0.200% |
| Server/client maximum Buffer | 0 / 0 | 0 / 0 | 0 / 0 |
| Server/client total Dropped | 0 / 0 | 0 / 0 | 0 / 0 |

QD CPU is process CPU normalized to the total capacity of all 32 logical processors. For scale, one fully occupied
logical processor would be approximately 3.125% on this machine.

## Aggregate latency

All values are milliseconds. `event` combines Quote, Trade, and Summary samples; `batch` observes the last event of
each complete batch.

| Latency metric | Q1k/T1k/S1k | Q5k/T5k/S5k | Q10k/T10k/S10k |
|---|---:|---:|---:|
| Event p50 | 5.05 | 29.66 | 48.99 |
| Event p95 | 6.51 | 42.06 | 65.38 |
| Event p99 | 10.55 | 53.00 | 69.69 |
| Event p99.9 | 14.14 | 77.37 | 80.22 |
| Event maximum | 14.42 | 80.09 | 92.28 |
| Batch p50 | 5.93 | 36.30 | 64.37 |
| Batch p99 | 12.04 | 58.70 | 80.30 |

| Event-type p50 / p99 | Q1k/T1k/S1k | Q5k/T5k/S5k | Q10k/T10k/S10k |
|---|---:|---:|---:|
| Quote | 4.41 / 10.03 | 24.77 / 43.83 | 41.04 / 54.33 |
| Trade | 5.03 / 10.78 | 29.57 / 49.91 | 48.41 / 63.33 |
| Summary | 5.67 / 11.73 | 34.22 / 57.21 | 55.79 / 72.91 |

The Quote/Trade/Summary ordering remains visible and its gap grows with the number of instruments, consistent with
ordered work in the publication and serialization path.

## Comparison with the previous run

The previous run did not explicitly enable periodic QD statistics. A single comparison run cannot isolate logging
overhead from normal scheduler and OS variation.

| Comparison metric | Q1k/T1k/S1k | Q5k/T5k/S5k | Q10k/T10k/S10k |
|---|---:|---:|---:|
| Previous / current p50 | 5.80 / 5.05 | 24.62 / 29.66 | 50.66 / 48.99 |
| p50 change | -12.9% | +20.5% | -3.3% |
| Previous / current p99 | 13.36 / 10.55 | 37.25 / 53.00 | 70.47 / 69.69 |
| p99 change | -21.1% | +42.3% | -1.1% |

The absence of a consistent regression across profiles argues against attributing the middle-profile increase to
monitoring alone. Multiple randomized repetitions would be required to estimate monitoring overhead.

## Window stability and integrity

There were 18 latency windows per profile. Window ranges below are aggregate event latencies in milliseconds.

| Latency metric | Q1k/T1k/S1k | Q5k/T5k/S5k | Q10k/T10k/S10k |
|---|---:|---:|---:|
| Window p50 minimum | 4.86 | 26.60 | 46.78 |
| Window p50 median | 5.04 | 29.84 | 48.91 |
| Window p50 maximum | 5.44 | 33.08 | 51.44 |
| Window p99 minimum | 5.97 | 36.58 | 65.83 |
| Window p99 median | 7.78 | 46.52 | 67.64 |
| Window p99 maximum | 14.14 | 78.69 | 89.44 |

| Integrity metric | Q1k/T1k/S1k | Q5k/T5k/S5k | Q10k/T10k/S10k |
|---|---:|---:|---:|
| Complete batches | 180 | 180 | 180 |
| Event samples | 540,000 | 2,700,000 | 5,400,000 |
| Callbacks | 2,161 | 7,444 | 13,869 |
| Clock anomalies | 0 | 0 | 0 |
| Diagnosed incomplete batches | 0 | 0 | 1 |
| Pending batches at shutdown | 0 | 0 | 0 |

The largest profile diagnosed one partial batch (`sequence=31`, `11023/30000` events, marker present). Sequence 31
and the delayed timeout report place it at the warm-up/measurement transition, where fragments observed before
measurement are deliberately discarded. All 180 subsequent complete batches contributed exactly 30,000 samples,
so the fragment did not reduce the measured sample total.

## Monitoring field interpretation

- `Subscription` counts unique subscribed record IDs, not C++ subscription objects.
- `Storage` and `Buffer` are current record counts in storage and outgoing agent buffers.
- `Dropped` is the number of records dropped during the reporting interval.
- Read/Write are byte rates and, when available, subscription/data record rates.
- Data lag and RTT are record-weighted averages reported by QD; they are not identical to this test's latency.

The interpretation follows the QD implementations of
[`MonitoringEndpoint`](https://github.com/devexperts/QD/blob/master/qds-monitoring/src/main/java/com/devexperts/qd/monitoring/MonitoringEndpoint.java),
[`ConnectorsMonitoringTask`](https://github.com/devexperts/QD/blob/master/qds-monitoring/src/main/java/com/devexperts/qd/monitoring/ConnectorsMonitoringTask.java),
[`IOCounters`](https://github.com/devexperts/QD/blob/master/qds-monitoring/src/main/java/com/devexperts/qd/monitoring/IOCounters.java),
[`Stream`](https://github.com/devexperts/QD/blob/master/qd-core/src/main/java/com/devexperts/qd/impl/matrix/Stream.java), and
[`AgentBuffer`](https://github.com/devexperts/QD/blob/master/qd-core/src/main/java/com/devexperts/qd/impl/matrix/AgentBuffer.java).

## Environment and limitations

- CPU: AMD Ryzen 9 5950X, 16 cores / 32 logical processors.
- RAM: 128 GiB.
- OS: Windows 11 Pro Insider Preview, version 10.0.26340, build 26340.
- Build: C++23 Release, MSVC 19.51.36256.0, CMake 4.4.2, Visual Studio 18 2026 generator.
- dxFeed Graal C++ API: v7.0.0; runtime protocol log reports QDS 3.342.
- Source base: Git commit `80583e0175a8dc9eb9436778bccbe1569fcf06f0`, with monitoring and reporting changes
  in the working tree.

This remains one run per profile on loopback without CPU affinity or process-priority control. It establishes the
behavior of this build and machine, not a production SLA.
