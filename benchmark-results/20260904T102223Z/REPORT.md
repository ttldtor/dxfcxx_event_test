# Repeated latency benchmark

Run-level values are aggregated using the median; the range shows the minimum and maximum across independent repetitions. Latencies are in milliseconds.

| Scenario | Runs | Event p50 median | Event p99 median (range) | Event p99.9 median | Batch p99 median | Integrity |
|---|---:|---:|---:|---:|---:|---|
| q10k-t10k-s10k | 3 | 61.739 | 94.740 (81.408–96.862) | 104.459 | 104.467 | OK; missing batches reported |
| q1k-t1k-s1k | 3 | 5.500 | 10.069 (9.662–10.437) | 13.881 | 11.643 | OK |
| q50k-t50k-s50k | 3 | 344.079 | 519.444 (446.415–523.598) | 569.059 | 571.834 | OK |
| q5k-t5k-s5k | 3 | 30.504 | 49.413 (34.665–49.617) | 53.061 | 53.031 | OK |

Generated files: `latency-runs.csv`, `latency-comparison.csv`, `monitoring.csv`, `monitoring-summary.csv`, and `monitoring-comparison.csv`.

## Client monitoring

The table shows medians across repetitions. Lag is in milliseconds; dropped is the largest per-run sum and buffer is the largest per-run high-water mark.

| Scenario | Read records/s | Read lag | CPU | Maximum buffer | Maximum dropped |
|---|---:|---:|---:|---:|---:|
| q10k-t10k-s10k | 29900.833 | 10.224 | 0.207% | 0.000 | 0.000 |
| q1k-t1k-s1k | 3000.593 | 0.923 | 0.020% | 0.000 | 0.000 |
| q50k-t50k-s50k | 150001.508 | 29.509 | 1.337% | 0.000 | 0.000 |
| q5k-t5k-s5k | 15000.695 | 4.246 | 0.103% | 0.000 | 0.000 |
