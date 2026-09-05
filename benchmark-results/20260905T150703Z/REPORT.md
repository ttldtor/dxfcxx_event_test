# Repeated latency benchmark

Run-level values are aggregated using the median; the range shows the minimum and maximum across independent repetitions. Latencies are in milliseconds.

| Scenario | Role | Runs | Listener coverage median | Listener deficit median | Event p50 median | Event p99 median (range) | Event p99.9 median | Batch p99 median | Integrity |
|---|---|---:|---:|---:|---:|---:|---:|---:|---|
| 150k-100ms | feed | 3 | 99.992% | 6804.000 | 23.648 | 43.813 (40.020–45.748) | 54.790 | 52.161 | OK |
| 150k-10ms | feed | 3 | 99.877% | 111698.000 | 2.309 | 8.928 (8.176–9.020) | 12.250 | 9.847 | CHECK |
| 150k-1ms-stress | feed | 3 | 98.805% | 1079640.000 | 0.375 | 1.192 (1.127–1.199) | 6.291 | 1.253 | CHECK |
| 150k-1s | feed | 3 | 100.000% | 0.000 | 232.522 | 377.757 (362.516–395.231) | 441.282 | 432.411 | OK |


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
| 150k-100ms | 150010.000 | 2.792 | 1.007% | 0.000 | 0.000 |
| 150k-10ms | 149889.492 | 0.530 | 1.185% | 1.000 | 0.000 |
| 150k-1ms-stress | 150841.288 | 0.224 | 1.554% | 7.000 | 0.000 |
| 150k-1s | 150000.237 | 11.985 | 1.094% | 0.000 | 0.000 |


## Server monitoring

The table shows medians across repetitions. Lag is in milliseconds; dropped is the largest per-run sum and buffer is the largest per-run high-water mark.

| Scenario | Write records/s | Write lag | CPU | Maximum buffer | Maximum dropped |
|---|---:|---:|---:|---:|---:|
| 150k-100ms | 150007.500 | 0.165 | 1.428% | 0.000 | 0.000 |
| 150k-10ms | 149889.492 | 0.140 | 3.363% | 1.000 | 0.000 |
| 150k-1ms-stress | 150841.390 | 0.070 | 3.494% | 1.000 | 0.000 |
| 150k-1s | 150000.237 | 0.216 | 1.145% | 0.000 | 0.000 |


`Dropped = 0` rules out drops counted by the corresponding QD endpoint, but it does not rule out normal FEED conflation.
The current measurements cannot locate TICKER supersession on the publisher or feed side. Similar server write and
client read rates make transport loss unlikely, but they are interval averages rather than a record-by-record audit.
Low average CPU, buffer, and network utilization also do not exclude conflation: a short burst only has to overtake
listener processing for the same record and symbol before the next monitoring sample.
