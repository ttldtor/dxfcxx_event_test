# Repeated latency benchmark

Run-level values are aggregated using the median; the range shows the minimum and maximum across independent repetitions. Latencies are in milliseconds.

| Scenario | Role | Runs | Delivery median | Not delivered median | Event p50 median | Event p99 median (range) | Event p99.9 median | Batch p99 median | Integrity |
|---|---|---:|---:|---:|---:|---:|---:|---:|---|
| 150k-100ms | feed | 3 | 99.993% | 6520.000 | 26.508 | 46.594 (45.312–48.738) | 57.492 | 53.877 | OK |
| 150k-10ms | feed | 3 | 99.862% | 125032.000 | 2.702 | 10.516 (10.314–10.813) | 13.082 | 11.345 | CHECK |
| 150k-1ms-stress | feed | 3 | 98.778% | 1103182.000 | 0.413 | 1.191 (1.184–1.224) | 6.639 | 1.209 | CHECK |
| 150k-1s | feed | 3 | 100.000% | 0.000 | 287.955 | 431.980 (393.768–438.925) | 466.936 | 469.600 | OK |


`Delivery median` is delivered recurring events divided by events expected for the correlated publications.
`Not delivered` is an observed delivery deficit, not proof of FEED conflation by itself: endpoint buffering,
`Dropped` records, incomplete publication correlation, and measurement boundaries must be checked alongside it.
Events without a delivered timestamp marker are reported separately as `uncorrelated_events` and excluded from the
delivery ratio.

Generated files: `latency-runs.csv`, `latency-comparison.csv`, `monitoring.csv`, `monitoring-summary.csv`, and `monitoring-comparison.csv`.

## Client monitoring

The table shows medians across repetitions. Lag is in milliseconds; dropped is the largest per-run sum and buffer is the largest per-run high-water mark.

| Scenario | Read records/s | Read lag | CPU | Maximum buffer | Maximum dropped |
|---|---:|---:|---:|---:|---:|
| 150k-100ms | 150009.492 | 2.601 | 1.074% | 0.000 | 0.000 |
| 150k-10ms | 149029.746 | 0.593 | 1.220% | 1.000 | 0.000 |
| 150k-1ms-stress | 150795.203 | 0.224 | 1.583% | 1.000 | 0.000 |
| 150k-1s | 149999.983 | 15.414 | 1.170% | 0.000 | 0.000 |


## Server monitoring

The table shows medians across repetitions. Lag is in milliseconds; dropped is the largest per-run sum and buffer is the largest per-run high-water mark.

| Scenario | Write records/s | Write lag | CPU | Maximum buffer | Maximum dropped |
|---|---:|---:|---:|---:|---:|
| 150k-100ms | 150009.492 | 0.172 | 1.534% | 0.000 | 0.000 |
| 150k-10ms | 149029.712 | 0.153 | 3.392% | 1.000 | 0.000 |
| 150k-1ms-stress | 150795.000 | 0.064 | 3.458% | 1.000 | 0.000 |
| 150k-1s | 149999.983 | 0.242 | 1.351% | 1.000 | 0.000 |


`Dropped = 0` rules out drops counted by the corresponding QD endpoint, but it does not rule out normal FEED conflation.
