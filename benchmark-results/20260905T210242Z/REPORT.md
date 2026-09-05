# Repeated latency benchmark

Run-level values are aggregated using the median; the range shows the minimum and maximum across independent repetitions. Latencies are in milliseconds.

| Scenario | Role | Runs | Listener coverage median | Listener deficit median | Event p50 median | Event p99 median (range) | Event p99.9 median | Batch p99 median | Integrity |
|---|---|---:|---:|---:|---:|---:|---:|---:|---|
| batch-1-stress | feed | 3 | 26.121% | 6605927.000 | 7.032 | 13.231 (11.761–16.291) | 20.873 | 19.109 | OK |
| batch-1500 | feed | 3 | 99.914% | 8491.000 | 2.127 | 8.344 (7.935–8.886) | 13.069 | 9.244 | CHECK |
| batch-375 | feed | 3 | 99.871% | 12375.000 | 2.263 | 8.554 (8.074–8.793) | 12.378 | 9.630 | CHECK |
| batch-maximum | feed | 3 | 99.868% | 12770.000 | 2.077 | 8.294 (8.005–9.088) | 12.899 | 9.118 | CHECK |
| batch-optimal | feed | 3 | 99.839% | 14821.000 | 2.517 | 9.456 (9.046–10.708) | 14.066 | 10.526 | CHECK |
| stream-control | stream-feed | 3 | 100.000% | 0.000 | 2.419 | 9.297 (7.978–9.746) | 14.740 | 10.109 | CHECK |


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
| batch-1-stress | 149439.400 | 0.342 | 4.026% | 0.000 | 0.000 |
| batch-1500 | 149934.200 | 0.362 | 0.920% | 0.000 | 0.000 |
| batch-375 | 149847.000 | 0.413 | 1.104% | 0.000 | 0.000 |
| batch-maximum | 149689.600 | 0.368 | 0.985% | 0.000 | 0.000 |
| batch-optimal | 149672.167 | 0.429 | 1.148% | 0.000 | 0.000 |
| stream-control | n/a | n/a | n/a | n/a | n/a |


## Server monitoring

The table shows medians across repetitions. Lag is in milliseconds; dropped is the largest per-run sum and buffer is the largest per-run high-water mark.

| Scenario | Write records/s | Write lag | CPU | Maximum buffer | Maximum dropped |
|---|---:|---:|---:|---:|---:|
| batch-1-stress | 149439.400 | 0.128 | 3.388% | 0.000 | 0.000 |
| batch-1500 | 149934.200 | 0.115 | 3.363% | 0.000 | 0.000 |
| batch-375 | 149859.400 | 0.134 | 3.426% | 0.000 | 0.000 |
| batch-maximum | 149693.800 | 0.090 | 3.316% | 0.000 | 0.000 |
| batch-optimal | 149678.167 | 0.144 | 3.424% | 0.000 | 0.000 |
| stream-control | 150036.400 | 0.080 | 3.296% | 516.000 | 0.000 |


`Dropped = 0` rules out drops counted by the corresponding QD endpoint, but it does not rule out normal FEED conflation.
The current measurements cannot locate TICKER supersession on the publisher or feed side. Similar server write and
client read rates make transport loss unlikely, but they are interval averages rather than a record-by-record audit.
Low average CPU, buffer, and network utilization also do not exclude conflation: a short burst only has to overtake
listener processing for the same record and symbol before the next monitoring sample.
