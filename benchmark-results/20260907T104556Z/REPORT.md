# Matched ticker recovery after TimeAndSale HISTORY delivery

## Experiment definition

- **Objective:** Determine whether ticker latency returns to its pre-snapshot baseline after a transient TimeAndSale HISTORY subscription is removed.
- **Changed variable:** Retained TimeAndSale history depth delivered and removed during an already-running ticker measurement.
- **Controls:** CXX API v8 stack, 375 subscribed and active symbols, fixed Q/T/E/S load of 150000 events/s before and after the snapshot, 10 ms cadence, FEED role, zero aggregation, loopback transport, warm-up, measurement phases, and publication order seed.
- **Evaluation criteria:** Complete every snapshot with QD Dropped zero; verify that marker-correlated ticker latency and resource use rise during delivery and recover toward the matched Q/T/E/S pre-snapshot baseline after TimeAndSale removal.
- **Limitations:** The synthetic in-process server and loopback transport do not reproduce production multiplexer retention or network conditions; subscription removal is asynchronous; the snapshot phase is short and resource sampling can contain few observations; phase correlation counters can straddle exact boundaries.

## TimeAndSale snapshot and live cutover

The client adds a separate HISTORY subscription for the configured retained interval. Snapshot and live values are
medians across repetitions; the event range is the minimum and maximum complete snapshot size.

| Scenario | Runs | After snapshot | Completed symbols | Snapshot events median (range) | Snapshot callbacks | Snapshot duration | SNIP | First live vs global completion | Live p99 | CPU, one-core basis | RSS mean / maximum | Integrity |
|---|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| tns-recovery-h100 | 3 | removed | 375 | 37893 (37732–37964) | 44 | 417.873 ms | 375 | -18.184 ms | 12480.900 us | 37.046% | 215.267 MiB / 379.051 MiB | OK |
| tns-recovery-h1000 | 3 | removed | 375 | 374982 (374980–374984) | 422 | 1190.410 ms | 375 | -190.237 ms | 19418.600 us | 27.928% | 232.942 MiB / 386.914 MiB | OK |
| tns-recovery-h200 | 3 | removed | 375 | 75717 (75580–76058) | 95 | 573.592 ms | 375 | -58.194 ms | 28225.600 us | 31.098% | 216.291 MiB / 492.969 MiB | OK |


Integrity requires every requested symbol to complete with `SNAPSHOT_END` or `SNAPSHOT_SNIP`, no duplicate indices,
no live events before that symbol's snapshot completion, and no clock anomalies. A run that retains its TimeAndSale
subscription must also measure at least one live TimeAndSale event; a matched-recovery run may remove all symbols at
global snapshot completion.
`First live vs global completion` is negative when symbols that completed early start receiving live updates while
snapshots for other symbols are still in progress; this is valid per-symbol snapshot-to-live overlap. `SNAPSHOT_SNIP`
is reported separately because it is an expected bounded-history condition, not an integrity failure.
In a matched-recovery run, `Live p99` covers only per-symbol live-cutover events received before global snapshot
completion and removal; it is not a post-snapshot steady-state measurement.
CPU and RSS are sampled in the Graal client during the configured measurement interval. Delayed-subscription runs
include the snapshot, while subscriptions completed before measurement sample only the post-snapshot interval.

## Ticker latency around TimeAndSale snapshot delivery

The client starts ticker measurement first, adds the TimeAndSale HISTORY subscription after the configured delay,
and partitions ticker observations into `before`, `during`, and `after` phases. Phase boundaries are detected by the
client at subscription and global snapshot completion. Values are medians across repetitions.

| Scenario | Phase | Runs | Duration | Listener coverage | Event p50 | Event p99 (range) | Event p99.9 | Event maximum | CPU, one-core basis | RSS mean / maximum |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| tns-recovery-h100 | after | 3 | 29562.500 ms | 99.176% | 3139.800 us | 10291.300 (10216.900–12184.300) us | 16957.500 us | 22368.100 us | 34.481% | 240.481 / 379.051 MiB |
| tns-recovery-h100 | before | 3 | 10091.900 ms | 99.702% | 2612.000 us | 6078.800 (4767.800–6883.500) us | 12040.500 us | 12641.800 us | 27.858% | 145.175 / 193.508 MiB |
| tns-recovery-h100 | during | 3 | 417.873 ms | 100.000% | 3246.700 us | 10814.300 (8419.900–13068.600) us | 11516.900 us | 11737.800 us | 19.640% | 105.337 / 109.031 MiB |
| tns-recovery-h1000 | after | 3 | 28778.900 ms | 99.289% | 2496.600 us | 6044.600 (5339.400–6865.200) us | 14432.900 us | 17500.800 us | 24.535% | 268.188 / 386.914 MiB |
| tns-recovery-h1000 | before | 3 | 10083.300 ms | 99.801% | 2814.600 us | 5892.700 (4545.500–7713.500) us | 13815.000 us | 16905.200 us | 28.673% | 144.969 / 193.855 MiB |
| tns-recovery-h1000 | during | 3 | 1190.410 ms | 94.118% | 3367.500 us | 17223.500 (16823.400–22377.500) us | 18764.400 us | 18764.400 us | 46.658% | 114.045 / 157.020 MiB |
| tns-recovery-h200 | after | 3 | 29408.900 ms | 99.220% | 2682.400 us | 9014.000 (8959.800–10278.600) us | 15073.400 us | 19867.300 us | 27.044% | 242.685 / 493.031 MiB |
| tns-recovery-h200 | before | 3 | 10068.600 ms | 99.703% | 2897.300 us | 6386.800 (5316.000–6743.300) us | 12470.500 us | 14217.300 us | 29.478% | 143.057 / 191.789 MiB |
| tns-recovery-h200 | during | 3 | 573.592 ms | 97.727% | 4010.300 us | 18396.100 (11128.000–26869.100) us | 18815.700 us | 18815.700 us | 29.494% | 105.490 / 115.426 MiB |


Phase latency is based on marker-correlated Quote/Trade/TradeETH/Summary listener observations. Resource sampling is
performed separately in each phase; 100% CPU means one fully occupied logical core. A short `during` phase can have
few samples, so its range and the raw per-run CSV must be considered alongside the median. Phase delivery counters
can straddle a boundary when market events and their timestamp marker complete on opposite sides of it; whole-run
integrity and QD `Dropped` remain the authoritative loss checks.

## Results

Run-level values are aggregated using the median; the range shows the minimum and maximum across independent repetitions. Latencies are in milliseconds.

| Scenario | Role | Batch limit | Aggregation | Runs | Listener coverage median | Listener deficit median | Event p50 median | Event p99 median (range) | Event p99.9 median | Batch p99 median | Integrity |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| tns-recovery-h100 | feed | optimal | 0.000 ms | 3 | 99.550% | 28198.000 | 3.132 | 10.048 (9.802–10.813) | 17.283 | 11.782 | OK |
| tns-recovery-h1000 | feed | optimal | 0.000 ms | 3 | 99.547% | 27782.000 | 2.408 | 8.140 (7.436–10.283) | 16.075 | 9.339 | OK |
| tns-recovery-h200 | feed | optimal | 0.000 ms | 3 | 99.588% | 25705.000 | 2.755 | 8.960 (8.904–10.279) | 14.577 | 9.722 | OK |


`Listener coverage median` is recurring events observed by the C++ listener divided by events expected for the
correlated publications. `Listener deficit` is the corresponding observation gap. It is not a transport-loss or
QD-drop counter. In `FEED` mode, the gap may contain TICKER states superseded before listener delivery; endpoint
buffering, `Dropped` records, incomplete publication correlation, and measurement boundaries must also be checked.
Events without a delivered timestamp marker are reported separately as `uncorrelated_events` and excluded from the
listener coverage. Because FEED does not preserve publication boundaries, listener deficit and per-marker excess
are observations rather than integrity failures; STREAM_FEED still requires exact correlated delivery.

Generated files: `latency-runs.csv`, `latency-comparison.csv`, `time-series-runs.csv`, `time-series-comparison.csv`, `snapshot-overlap-runs.csv`, `snapshot-overlap-comparison.csv`, `delivery-runs.csv`, `delivery-comparison.csv`, `monitoring.csv`, `monitoring-summary.csv`, and `monitoring-comparison.csv`.

## Client monitoring

The table shows medians across repetitions. Lag is in milliseconds; dropped is the largest per-run sum and buffer is the largest per-run high-water mark.

| Scenario | Read records/s | Read lag | CPU | Maximum buffer | Maximum dropped |
|---|---:|---:|---:|---:|---:|
| tns-recovery-h100 | 150065.714 | 0.500 | 1.033% | 0.000 | 0.000 |
| tns-recovery-h1000 | 157732.714 | 0.624 | 0.787% | 0.000 | 0.000 |
| tns-recovery-h200 | 151235.286 | 0.493 | 0.896% | 1.000 | 0.000 |


## Server monitoring

The table shows medians across repetitions. Lag is in milliseconds; dropped is the largest per-run sum and buffer is the largest per-run high-water mark.

| Scenario | Write records/s | Write lag | CPU | Maximum buffer | Maximum dropped |
|---|---:|---:|---:|---:|---:|
| tns-recovery-h100 | 150073.000 | 0.146 | 3.397% | 0.000 | 0.000 |
| tns-recovery-h1000 | 157732.714 | 0.089 | 3.327% | 0.000 | 0.000 |
| tns-recovery-h200 | 151238.875 | 0.112 | 3.380% | 0.000 | 0.000 |


`Dropped = 0` rules out drops counted by the corresponding QD endpoint, but it does not rule out normal FEED conflation.
The current measurements cannot locate TICKER supersession on the publisher or feed side. A monitoring value of
`n/a` means that the endpoint log emitted no parseable sample; it must not be interpreted as zero. When both rates
are available, similar server write and client read rates make transport loss unlikely, but they are interval
averages rather than a record-by-record audit.
Low average CPU, buffer, and network utilization also do not exclude conflation: a short burst only has to overtake
listener processing for the same record and symbol before the next monitoring sample.
