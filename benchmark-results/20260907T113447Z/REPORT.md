# Graal CXX and legacy C API capacity confirmation

## Experiment definition

- **Objective:** Confirm whether either non-conflating client delivery path stops keeping pace before the current synthetic publisher reaches its capacity knee.
- **Changed variable:** Client delivery semantics and publication rate: Graal CXX STREAM_FEED, Graal CXX FEED, and the default legacy C API contract at 150,000, 375,000, and 500,000 nominal events/s.
- **Controls:** Host, compiler, synthetic server, 375-symbol shuffled Q/T/E/S workload, 1,500 events per publication, zero Graal aggregation, warm-up, duration, monitoring period, and rotating run order.
- **Evaluation criteria:** Repeat delivery coverage, throughput, latency, QD drops, buffers, CPU, and RSS while distinguishing a client limit from missed publisher deadlines.
- **Limitations:** The current publisher already falls below its nominal 500,000-event/s target; FEED may supersede intermediate ticker states by design; legacy events have no common benchmark publish timestamp; client listener work is not identical.

## Legacy C API delivery

The legacy C API has no benchmark marker event, so these values describe callback delivery and callback shape; they
are not timestamp-based E2E latency measurements. Rates are medians across repetitions.

| Scenario | Contract | Runs | Nominal events/s | Observed events/s median (range) | Callbacks median | Maximum `data_count` | CPU, one-core basis | CPU, host basis | RSS mean / maximum |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 150k-legacy-default | default | 3 | 150000.000 | 149013.280 (148968.200–149091.769) | 4476000.000 | 1 | 12.059% | 0.377% | 7.273 MiB / 7.301 MiB |
| 375k-legacy-default | default | 3 | 375000.000 | 368412.411 (367341.287–369120.224) | 11061840.000 | 1 | 26.156% | 0.817% | 7.263 MiB / 7.293 MiB |
| 500k-legacy-default | default | 3 | 500000.000 | 480789.052 (478271.183–488485.590) | 14449861.000 | 1 | 31.975% | 0.999% | 7.265 MiB / 7.297 MiB |


With the default legacy contract, the C API expands each base-symbol Quote, Trade, TradeETH, and Summary subscription
into the composite plus 26 regional symbols. A task can publish a configured subset of those regional record keys
while keeping its recurring event rate fixed. CPU uses both a one-core basis and a host-normalized basis; RSS is
sampled by the cross-platform `ttldtor/Process` library during the measurement interval.

## Results

Run-level values are aggregated using the median; the range shows the minimum and maximum across independent repetitions. Latencies are in milliseconds.

| Scenario | Role | Batch limit | Aggregation | Runs | Listener coverage median | Listener deficit median | Event p50 median | Event p99 median (range) | Event p99.9 median | Batch p99 median | Integrity |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| 150k-graal-feed | feed | optimal | 0.000 ms | 3 | 99.944% | 2546.000 | 2.862 | 8.543 (7.989–9.856) | 14.711 | 10.528 | OK |
| 150k-graal-stream | stream-feed | optimal | 0.000 ms | 3 | 100.000% | 0.000 | 3.332 | 8.202 (8.015–9.701) | 17.250 | 9.928 | OK |
| 375k-graal-feed | feed | optimal | 0.000 ms | 3 | 98.627% | 160136.000 | 2.599 | 9.472 (8.768–9.609) | 14.089 | 10.600 | OK |
| 375k-graal-stream | stream-feed | optimal | 0.000 ms | 3 | 100.000% | 0.000 | 3.320 | 21.357 (18.711–33.977) | 50.176 | 22.098 | OK |
| 500k-graal-feed | feed | optimal | 0.000 ms | 3 | 98.024% | 296627.000 | 2.498 | 8.694 (7.728–8.892) | 13.206 | 9.791 | OK |
| 500k-graal-stream | stream-feed | optimal | 0.000 ms | 3 | 100.000% | 0.000 | 3.081 | 29.758 (23.140–34.989) | 59.832 | 30.677 | OK |


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
| 150k-graal-feed | 149134.250 | 0.380 | 1.103% | 0.000 | 0.000 |
| 150k-graal-stream | 149193.000 | 0.465 | 1.074% | 210.000 | 0.000 |
| 150k-legacy-default | n/a | n/a | n/a | n/a | n/a |
| 375k-graal-feed | 367577.750 | 0.483 | 2.712% | 0.000 | 0.000 |
| 375k-graal-stream | 366219.750 | 1.975 | 2.832% | 308.000 | 0.000 |
| 375k-legacy-default | n/a | n/a | n/a | n/a | n/a |
| 500k-graal-feed | 480131.400 | 0.409 | 3.305% | 0.000 | 0.000 |
| 500k-graal-stream | 472550.000 | 0.511 | 3.542% | 308.000 | 0.000 |
| 500k-legacy-default | n/a | n/a | n/a | n/a | n/a |


## Server monitoring

The table shows medians across repetitions. Lag is in milliseconds; dropped is the largest per-run sum and buffer is the largest per-run high-water mark.

| Scenario | Write records/s | Write lag | CPU | Maximum buffer | Maximum dropped |
|---|---:|---:|---:|---:|---:|
| 150k-graal-feed | 149133.400 | 0.153 | 3.424% | 1.000 | 0.000 |
| 150k-graal-stream | 149319.000 | 0.132 | 3.360% | 0.000 | 0.000 |
| 150k-legacy-default | 148920.000 | 0.187 | 3.372% | 0.000 | 0.000 |
| 375k-graal-feed | 367757.200 | 0.132 | 3.844% | 0.000 | 0.000 |
| 375k-graal-stream | 366566.333 | 0.132 | 3.766% | 1398.000 | 0.000 |
| 375k-legacy-default | 368358.000 | 0.169 | 3.852% | 0.000 | 0.000 |
| 500k-graal-feed | 475656.000 | 0.136 | 4.056% | 0.000 | 0.000 |
| 500k-graal-stream | 474781.400 | 0.128 | 3.984% | 1193.000 | 0.000 |
| 500k-legacy-default | 485549.400 | 0.162 | 3.974% | 0.000 | 0.000 |


`Dropped = 0` rules out drops counted by the corresponding QD endpoint, but it does not rule out normal FEED conflation.
The current measurements cannot locate TICKER supersession on the publisher or feed side. A monitoring value of
`n/a` means that the endpoint log emitted no parseable sample; it must not be interpreted as zero. When both rates
are available, similar server write and client read rates make transport loss unlikely, but they are interval
averages rather than a record-by-record audit.
Low average CPU, buffer, and network utilization also do not exclude conflation: a short burst only has to overtake
listener processing for the same record and symbol before the next monitoring sample.
