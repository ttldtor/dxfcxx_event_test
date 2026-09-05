# Repeated latency benchmark

Run-level values are aggregated using the median; the range shows the minimum and maximum across independent repetitions. Latencies are in milliseconds.

| Scenario | Role | Runs | Listener coverage median | Listener deficit median | Event p50 median | Event p99 median (range) | Event p99.9 median | Batch p99 median | Integrity |
|---|---|---:|---:|---:|---:|---:|---:|---:|---|
| 150k-10000-symbols | feed | 3 | 100.000% | 0.000 | 3.129 | 13.097 (12.141–13.102) | 19.375 | 14.617 | OK |
| 150k-375-symbols | feed | 3 | 99.855% | 26123.000 | 2.410 | 9.438 (7.587–9.704) | 13.646 | 10.385 | CHECK |
| 150k-3750-symbols | feed | 3 | 100.000% | 0.000 | 2.984 | 11.439 (8.957–11.634) | 17.887 | 12.882 | OK |


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
| 150k-10000-symbols | 149256.273 | 0.755 | 1.345% | 1.000 | 0.000 |
| 150k-375-symbols | 149627.364 | 0.596 | 1.189% | 0.000 | 0.000 |
| 150k-3750-symbols | 149261.500 | 0.698 | 1.332% | 0.000 | 0.000 |


## Server monitoring

The table shows medians across repetitions. Lag is in milliseconds; dropped is the largest per-run sum and buffer is the largest per-run high-water mark.

| Scenario | Write records/s | Write lag | CPU | Maximum buffer | Maximum dropped |
|---|---:|---:|---:|---:|---:|
| 150k-10000-symbols | 149258.727 | 0.151 | 3.441% | 0.000 | 0.000 |
| 150k-375-symbols | 149627.364 | 0.135 | 3.352% | 0.000 | 0.000 |
| 150k-3750-symbols | 149261.500 | 0.147 | 3.446% | 0.000 | 0.000 |


`Dropped = 0` rules out drops counted by the corresponding QD endpoint, but it does not rule out normal FEED conflation.
The current measurements cannot locate TICKER supersession on the publisher or feed side. Similar server write and
client read rates make transport loss unlikely, but they are interval averages rather than a record-by-record audit.
Low average CPU, buffer, and network utilization also do not exclude conflation: a short burst only has to overtake
listener processing for the same record and symbol before the next monitoring sample.
