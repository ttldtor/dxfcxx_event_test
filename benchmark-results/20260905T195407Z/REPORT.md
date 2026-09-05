# Repeated latency benchmark

Run-level values are aggregated using the median; the range shows the minimum and maximum across independent repetitions. Latencies are in milliseconds.

| Scenario | Role | Runs | Listener coverage median | Listener deficit median | Event p50 median | Event p99 median (range) | Event p99.9 median | Batch p99 median | Integrity |
|---|---|---:|---:|---:|---:|---:|---:|---:|---|
| order-quote-last | feed | 3 | 99.870% | 12356.000 | 2.434 | 9.287 (9.107–9.555) | 13.412 | 10.453 | CHECK |
| order-summary-last | feed | 3 | 99.850% | 13587.000 | 2.349 | 9.299 (8.359–9.368) | 12.906 | 10.352 | CHECK |
| order-trade-eth-last | feed | 3 | 99.906% | 8665.000 | 2.371 | 9.000 (7.554–9.198) | 12.701 | 10.093 | CHECK |
| order-trade-last | feed | 3 | 99.843% | 14772.000 | 2.495 | 9.346 (9.073–9.739) | 13.803 | 10.337 | CHECK |


`Listener coverage median` is recurring events observed by the C++ listener divided by events expected for the
correlated publications. `Listener deficit` is the corresponding observation gap. It is not a transport-loss or
QD-drop counter. In `FEED` mode, the gap may contain TICKER states superseded before listener delivery; endpoint
buffering, `Dropped` records, incomplete publication correlation, and measurement boundaries must also be checked.
Events without a delivered timestamp marker are reported separately as `uncorrelated_events` and excluded from the
listener coverage.

Generated files: `latency-runs.csv`, `latency-comparison.csv`, `monitoring.csv`, `monitoring-summary.csv`, and `monitoring-comparison.csv`.

## Client monitoring

The table shows medians across repetitions. Lag is in milliseconds; dropped is the largest per-run sum and buffer is the largest per-run high-water mark.

| Scenario | Read records/s | Read lag | CPU | Maximum buffer | Maximum dropped |
|---|---:|---:|---:|---:|---:|
| order-quote-last | 149748.400 | 0.538 | 1.144% | 0.000 | 0.000 |
| order-summary-last | 149592.000 | 0.528 | 1.104% | 0.000 | 0.000 |
| order-trade-eth-last | 149688.000 | 0.477 | 1.172% | 0.000 | 0.000 |
| order-trade-last | 149631.200 | 0.520 | 1.163% | 0.000 | 0.000 |


## Server monitoring

The table shows medians across repetitions. Lag is in milliseconds; dropped is the largest per-run sum and buffer is the largest per-run high-water mark.

| Scenario | Write records/s | Write lag | CPU | Maximum buffer | Maximum dropped |
|---|---:|---:|---:|---:|---:|
| order-quote-last | 149733.200 | 0.147 | 3.414% | 0.000 | 0.000 |
| order-summary-last | 149592.200 | 0.140 | 3.408% | 0.000 | 0.000 |
| order-trade-eth-last | 149688.000 | 0.141 | 3.390% | 0.000 | 0.000 |
| order-trade-last | 149646.200 | 0.153 | 3.410% | 0.000 | 0.000 |


`Dropped = 0` rules out drops counted by the corresponding QD endpoint, but it does not rule out normal FEED conflation.
The current measurements cannot locate TICKER supersession on the publisher or feed side. Similar server write and
client read rates make transport loss unlikely, but they are interval averages rather than a record-by-record audit.
Low average CPU, buffer, and network utilization also do not exclude conflation: a short burst only has to overtake
listener processing for the same record and symbol before the next monitoring sample.
