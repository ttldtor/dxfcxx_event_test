# Repeated latency benchmark

Run-level values are aggregated using the median; the range shows the minimum and maximum across independent repetitions. Latencies are in milliseconds.

| Scenario | Role | Batch limit | Aggregation | Runs | Listener coverage median | Listener deficit median | Event p50 median | Event p99 median (range) | Event p99.9 median | Batch p99 median | Integrity |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| aggregation-stream-0 | stream-feed | optimal | 0.000 ms | 3 | 100.000% | 0.000 | 3.064 | 11.647 (11.606–13.352) | 24.289 | 12.889 | OK |
| aggregation-stream-10ms | stream-feed | optimal | 10.000 ms | 3 | 100.000% | 0.000 | 9.931 | 23.009 (22.694–23.477) | 30.319 | 25.003 | OK |
| aggregation-stream-1ms | stream-feed | optimal | 1.000 ms | 3 | 100.000% | 0.000 | 8.609 | 19.798 (19.310–20.602) | 35.698 | 22.608 | OK |


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
| aggregation-stream-0 | 149229.250 | 0.499 | 1.135% | 619.000 | 0.000 |
| aggregation-stream-10ms | 149810.250 | 0.376 | 0.910% | 2887.000 | 0.000 |
| aggregation-stream-1ms | 149390.000 | 0.410 | 1.002% | 3000.000 | 0.000 |


## Server monitoring

The table shows medians across repetitions. Lag is in milliseconds; dropped is the largest per-run sum and buffer is the largest per-run high-water mark.

| Scenario | Write records/s | Write lag | CPU | Maximum buffer | Maximum dropped |
|---|---:|---:|---:|---:|---:|
| aggregation-stream-0 | 149337.200 | 0.131 | 3.338% | 587.000 | 0.000 |
| aggregation-stream-10ms | 149637.000 | 0.129 | 3.340% | 298.000 | 0.000 |
| aggregation-stream-1ms | 149523.200 | 0.130 | 3.380% | 1016.000 | 0.000 |


`Dropped = 0` rules out drops counted by the corresponding QD endpoint, but it does not rule out normal FEED conflation.
The current measurements cannot locate TICKER supersession on the publisher or feed side. A monitoring value of
`n/a` means that the endpoint log emitted no parseable sample; it must not be interpreted as zero. When both rates
are available, similar server write and client read rates make transport loss unlikely, but they are interval
averages rather than a record-by-record audit.
Low average CPU, buffer, and network utilization also do not exclude conflation: a short burst only has to overtake
listener processing for the same record and symbol before the next monitoring sample.
