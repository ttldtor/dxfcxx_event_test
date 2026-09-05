# Repeated latency benchmark

Run-level values are aggregated using the median; the range shows the minimum and maximum across independent repetitions. Latencies are in milliseconds.

| Scenario | Role | Batch limit | Aggregation | Runs | Listener coverage median | Listener deficit median | Event p50 median | Event p99 median (range) | Event p99.9 median | Batch p99 median | Integrity |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| aggregation-feed-0 | feed | optimal | 0.000 ms | 3 | 99.806% | 18306.000 | 2.385 | 9.283 (9.039–10.322) | 13.708 | 10.659 | CHECK |
| aggregation-feed-10ms | feed | optimal | 10.000 ms | 3 | 68.917% | 2842068.000 | 7.107 | 14.921 (14.580–15.268) | 22.284 | 16.650 | CHECK |
| aggregation-feed-1ms | feed | optimal | 1.000 ms | 3 | 75.627% | 2227794.000 | 5.442 | 13.377 (12.771–14.059) | 17.991 | 14.026 | CHECK |
| aggregation-stream-0 | stream-feed | optimal | 0.000 ms | 3 | 100.000% | 0.000 | 2.618 | 10.033 (9.913–11.288) | 16.793 | 11.261 | CHECK |


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
| aggregation-feed-0 | 149836.200 | 0.475 | 0.998% | 0.000 | 0.000 |
| aggregation-feed-10ms | 149415.200 | 0.412 | 0.818% | 0.000 | 0.000 |
| aggregation-feed-1ms | 149927.600 | 0.381 | 0.786% | 0.000 | 0.000 |
| aggregation-stream-0 | n/a | n/a | n/a | n/a | n/a |


## Server monitoring

The table shows medians across repetitions. Lag is in milliseconds; dropped is the largest per-run sum and buffer is the largest per-run high-water mark.

| Scenario | Write records/s | Write lag | CPU | Maximum buffer | Maximum dropped |
|---|---:|---:|---:|---:|---:|
| aggregation-feed-0 | 149838.000 | 0.097 | 3.328% | 0.000 | 0.000 |
| aggregation-feed-10ms | 149415.000 | 0.117 | 3.354% | 0.000 | 0.000 |
| aggregation-feed-1ms | 149933.600 | 0.100 | 3.322% | 0.000 | 0.000 |
| aggregation-stream-0 | 149913.400 | 0.081 | 3.272% | 910.000 | 0.000 |


`Dropped = 0` rules out drops counted by the corresponding QD endpoint, but it does not rule out normal FEED conflation.
The current measurements cannot locate TICKER supersession on the publisher or feed side. Similar server write and
client read rates make transport loss unlikely, but they are interval averages rather than a record-by-record audit.
Low average CPU, buffer, and network utilization also do not exclude conflation: a short burst only has to overtake
listener processing for the same record and symbol before the next monitoring sample.
