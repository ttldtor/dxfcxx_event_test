# Repeated latency benchmark

Run-level values are aggregated using the median; the range shows the minimum and maximum across independent repetitions. Latencies are in milliseconds.

| Scenario | Role | Batch limit | Aggregation | Runs | Listener coverage median | Listener deficit median | Event p50 median | Event p99 median (range) | Event p99.9 median | Batch p99 median | Integrity |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| aggregation-stream-0 | stream-feed | optimal | 0.000 ms | 3 | 100.000% | 0.000 | 2.918 | 10.660 (10.552–10.757) | 17.924 | 11.928 | OK |
| aggregation-stream-10ms | stream-feed | optimal | 10.000 ms | 3 | 100.000% | 0.000 | 9.912 | 21.987 (21.905–22.705) | 29.547 | 23.544 | OK |
| aggregation-stream-1ms | stream-feed | optimal | 1.000 ms | 3 | 100.000% | 0.000 | 8.682 | 19.625 (19.021–19.907) | 27.878 | 21.422 | OK |


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
| aggregation-stream-0 | n/a | n/a | n/a | n/a | n/a |
| aggregation-stream-10ms | n/a | n/a | n/a | n/a | n/a |
| aggregation-stream-1ms | n/a | n/a | n/a | n/a | n/a |


## Server monitoring

The table shows medians across repetitions. Lag is in milliseconds; dropped is the largest per-run sum and buffer is the largest per-run high-water mark.

| Scenario | Write records/s | Write lag | CPU | Maximum buffer | Maximum dropped |
|---|---:|---:|---:|---:|---:|
| aggregation-stream-0 | 149622.200 | 0.129 | 3.368% | 1198.000 | 0.000 |
| aggregation-stream-10ms | 149600.800 | 0.128 | 3.372% | 1422.000 | 0.000 |
| aggregation-stream-1ms | 149751.400 | 0.129 | 3.360% | 0.000 | 0.000 |


`Dropped = 0` rules out drops counted by the corresponding QD endpoint, but it does not rule out normal FEED conflation.
The current measurements cannot locate TICKER supersession on the publisher or feed side. A monitoring value of
`n/a` means that the endpoint log emitted no parseable sample; it must not be interpreted as zero. When both rates
are available, similar server write and client read rates make transport loss unlikely, but they are interval
averages rather than a record-by-record audit.
Low average CPU, buffer, and network utilization also do not exclude conflation: a short burst only has to overtake
listener processing for the same record and symbol before the next monitoring sample.
