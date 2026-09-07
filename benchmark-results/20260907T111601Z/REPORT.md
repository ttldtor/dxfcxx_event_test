# Graal CXX and legacy C API capacity discovery

## Experiment definition

- **Objective:** Locate the recurring event rate at which each client delivery path first stops keeping pace with the same synthetic publisher.
- **Changed variable:** Client delivery semantics and publication rate: Graal CXX STREAM_FEED, Graal CXX FEED, and the default legacy C API contract at 150,000 through 500,000 events/s.
- **Controls:** Host, compiler, synthetic server, 375-symbol shuffled Q/T/E/S workload, 1,500 events per publication, zero Graal aggregation, warm-up, duration, and monitoring period.
- **Evaluation criteria:** Identify the first rate with reduced delivery coverage or throughput while verifying publisher target rate, skipped deadlines, QD drops, buffers, CPU, and RSS.
- **Limitations:** This single-repetition discovery suite is not a statistical comparison; FEED may supersede intermediate ticker states by design, and legacy events have no common benchmark publish timestamp for E2E latency comparison.

## Legacy C API delivery

The legacy C API has no benchmark marker event, so these values describe callback delivery and callback shape; they
are not timestamp-based E2E latency measurements. Rates are medians across repetitions.

| Scenario | Contract | Runs | Nominal events/s | Observed events/s median (range) | Callbacks median | Maximum `data_count` | CPU, one-core basis | CPU, host basis | RSS mean / maximum |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 150k-legacy-default | default | 1 | 150000.000 | 148724.897 (148724.897–148724.897) | 2981933.000 | 1 | 14.883% | 0.465% | 7.303 MiB / 7.305 MiB |
| 250k-legacy-default | default | 1 | 250000.000 | 247469.798 (247469.798–247469.798) | 4963500.000 | 1 | 20.332% | 0.635% | 7.304 MiB / 7.305 MiB |
| 300k-legacy-default | default | 1 | 300000.000 | 295325.575 (295325.575–295325.575) | 5915966.000 | 1 | 21.920% | 0.685% | 7.334 MiB / 7.340 MiB |
| 375k-legacy-default | default | 1 | 375000.000 | 368721.647 (368721.647–368721.647) | 7387367.000 | 1 | 31.041% | 0.970% | 7.297 MiB / 7.305 MiB |
| 500k-legacy-default | default | 1 | 500000.000 | 467970.992 (467970.992–467970.992) | 9385500.000 | 1 | 33.970% | 1.062% | 7.323 MiB / 7.324 MiB |


With the default legacy contract, the C API expands each base-symbol Quote, Trade, TradeETH, and Summary subscription
into the composite plus 26 regional symbols. A task can publish a configured subset of those regional record keys
while keeping its recurring event rate fixed. CPU uses both a one-core basis and a host-normalized basis; RSS is
sampled by the cross-platform `ttldtor/Process` library during the measurement interval.

## Results

Run-level values are aggregated using the median; the range shows the minimum and maximum across independent repetitions. Latencies are in milliseconds.

| Scenario | Role | Batch limit | Aggregation | Runs | Listener coverage median | Listener deficit median | Event p50 median | Event p99 median (range) | Event p99.9 median | Batch p99 median | Integrity |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| 150k-graal-feed | feed | optimal | 0.000 ms | 1 | 53.718% | 1228263.000 | 9.585 | 73.575 (73.575–73.575) | 164.251 | 128.677 | OK |
| 150k-graal-stream | stream-feed | optimal | 0.000 ms | 1 | 100.000% | 0.000 | 3.465 | 958.454 (958.454–958.454) | 1074.310 | 961.705 | OK |
| 250k-graal-feed | feed | optimal | 0.000 ms | 1 | 99.311% | 35221.000 | 3.062 | 9.610 (9.610–9.610) | 17.341 | 11.360 | OK |
| 250k-graal-stream | stream-feed | optimal | 0.000 ms | 1 | 100.000% | 0.000 | 3.502 | 13.884 (13.884–13.884) | 26.761 | 14.724 | OK |
| 300k-graal-feed | feed | optimal | 0.000 ms | 1 | 99.158% | 53010.000 | 2.924 | 10.620 (10.620–10.620) | 15.738 | 11.489 | OK |
| 300k-graal-stream | stream-feed | optimal | 0.000 ms | 1 | 100.000% | 0.000 | 2.422 | 9.165 (9.165–9.165) | 21.301 | 9.725 | OK |
| 375k-graal-feed | feed | optimal | 0.000 ms | 1 | 98.538% | 114304.000 | 2.891 | 9.403 (9.403–9.403) | 14.743 | 11.411 | OK |
| 375k-graal-stream | stream-feed | optimal | 0.000 ms | 1 | 100.000% | 0.000 | 3.354 | 17.110 (17.110–17.110) | 35.348 | 17.958 | OK |
| 500k-graal-feed | feed | optimal | 0.000 ms | 1 | 98.194% | 180575.000 | 2.696 | 9.050 (9.050–9.050) | 16.142 | 10.176 | OK |
| 500k-graal-stream | stream-feed | optimal | 0.000 ms | 1 | 100.000% | 0.000 | 3.472 | 27.579 (27.579–27.579) | 59.405 | 28.170 | OK |


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
| 150k-graal-feed | 99040.667 | 1.123 | 0.857% | 0.000 | 0.000 |
| 150k-graal-stream | 143913.333 | 0.560 | 1.113% | 0.000 | 0.000 |
| 150k-legacy-default | n/a | n/a | n/a | n/a | n/a |
| 250k-graal-feed | 246490.333 | 0.401 | 1.893% | 0.000 | 0.000 |
| 250k-graal-stream | 246852.333 | 0.417 | 1.860% | 0.000 | 0.000 |
| 250k-legacy-default | n/a | n/a | n/a | n/a | n/a |
| 300k-graal-feed | 294433.667 | 0.438 | 2.200% | 0.000 | 0.000 |
| 300k-graal-stream | 297019.667 | 0.295 | 1.557% | 0.000 | 0.000 |
| 300k-legacy-default | n/a | n/a | n/a | n/a | n/a |
| 375k-graal-feed | 368026.333 | 0.419 | 2.777% | 0.000 | 0.000 |
| 375k-graal-stream | 368642.667 | 0.461 | 2.920% | 18.000 | 0.000 |
| 375k-legacy-default | n/a | n/a | n/a | n/a | n/a |
| 500k-graal-feed | 474069.333 | 0.467 | 3.567% | 0.000 | 0.000 |
| 500k-graal-stream | 471150.000 | 0.491 | 3.790% | 0.000 | 0.000 |
| 500k-legacy-default | n/a | n/a | n/a | n/a | n/a |


## Server monitoring

The table shows medians across repetitions. Lag is in milliseconds; dropped is the largest per-run sum and buffer is the largest per-run high-water mark.

| Scenario | Write records/s | Write lag | CPU | Maximum buffer | Maximum dropped |
|---|---:|---:|---:|---:|---:|
| 150k-graal-feed | 104155.000 | 0.544 | 2.055% | 6.000 | 0.000 |
| 150k-graal-stream | 144849.000 | 0.134 | 3.270% | 2898.000 | 0.000 |
| 150k-legacy-default | 148795.333 | 0.212 | 3.377% | 0.000 | 0.000 |
| 250k-graal-feed | 246514.333 | 0.142 | 3.653% | 0.000 | 0.000 |
| 250k-graal-stream | 246831.000 | 0.136 | 3.610% | 0.000 | 0.000 |
| 250k-legacy-default | 247535.333 | 0.189 | 3.610% | 0.000 | 0.000 |
| 300k-graal-feed | 294433.667 | 0.146 | 3.713% | 0.000 | 0.000 |
| 300k-graal-stream | 287134.000 | 0.083 | 3.380% | 0.000 | 0.000 |
| 300k-legacy-default | 295537.333 | 0.169 | 3.727% | 0.000 | 0.000 |
| 375k-graal-feed | 367566.750 | 0.159 | 3.955% | 0.000 | 0.000 |
| 375k-graal-stream | 368840.250 | 0.137 | 3.877% | 1255.000 | 0.000 |
| 375k-legacy-default | 368230.667 | 0.220 | 3.923% | 0.000 | 0.000 |
| 500k-graal-feed | 474051.333 | 0.150 | 4.227% | 0.000 | 0.000 |
| 500k-graal-stream | 471106.333 | 0.139 | 4.047% | 111.000 | 0.000 |
| 500k-legacy-default | 473765.000 | 0.181 | 4.000% | 0.000 | 0.000 |


`Dropped = 0` rules out drops counted by the corresponding QD endpoint, but it does not rule out normal FEED conflation.
The current measurements cannot locate TICKER supersession on the publisher or feed side. A monitoring value of
`n/a` means that the endpoint log emitted no parseable sample; it must not be interpreted as zero. When both rates
are available, similar server write and client read rates make transport loss unlikely, but they are interval
averages rather than a record-by-record audit.
Low average CPU, buffer, and network utilization also do not exclude conflation: a short burst only has to overtake
listener processing for the same record and symbol before the next monitoring sample.
