# Repeated latency benchmark

Run-level values are aggregated using the median; the range shows the minimum and maximum across independent repetitions. Latencies are in milliseconds.

| Scenario | Runs | Event p50 median | Event p99 median (range) | Event p99.9 median | Batch p99 median | Integrity |
|---|---:|---:|---:|---:|---:|---|
| 150k-100ms | 3 | 29.565 | 47.211 (46.224–47.335) | 54.532 | 51.362 | OK |
| 150k-10ms | 3 | 2.986 | 10.739 (10.607–10.832) | 15.029 | 11.833 | OK |
| 150k-1ms-stress | 3 | 0.367 | 0.934 (0.916–0.941) | 13.784 | 1.028 | OK |
| 150k-1s | 3 | 374.599 | 523.552 (410.982–538.988) | 555.153 | 559.664 | OK |

Generated files: `latency-runs.csv`, `latency-comparison.csv`, `monitoring.csv`, `monitoring-summary.csv`, and `monitoring-comparison.csv`.

## Client monitoring

The table shows medians across repetitions. Lag is in milliseconds; dropped is the largest per-run sum and buffer is the largest per-run high-water mark.

| Scenario | Read records/s | Read lag | CPU | Maximum buffer | Maximum dropped |
|---|---:|---:|---:|---:|---:|
| 150k-100ms | 150008.700 | 4.803 | 1.031% | 571.000 | 0.000 |
| 150k-10ms | 149753.678 | 0.562 | 1.246% | 486.000 | 0.000 |
| 150k-1ms-stress | 150823.915 | 0.185 | 1.605% | 151.000 | 0.000 |
| 150k-1s | 150001.254 | 30.320 | 1.255% | 633.000 | 0.000 |
