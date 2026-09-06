# STREAM_FEED monitoring and aggregation control

This rerun verifies the `monitoring.stat` configuration fix and repeats the aggregation-period control used to
separate notification batching from FEED supersession. The client uses `STREAM_FEED`; the workload contains 375
symbols, 1,500 recurring events per publication, a 10 ms publication cadence, and a nominal rate of 150,000 events/s.
Each aggregation period was repeated three times with a fresh server and client.

The `environment.txt` commit identifies the checked-out base revision. The binaries for this run were built from
that revision plus the working-tree change that passes `monitoring.stat` directly to both endpoint builders; the
change and these results are intended to be committed together.

## Results

| Aggregation period | Listener coverage | Mean events/callback | Event p50 | Event p99 | Event p99.9 | Maximum event latency |
|---:|---:|---:|---:|---:|---:|---:|
| 0 ms | 100.000% | 411.2 | 3.587 ms | 12.728 ms | 19.094 ms | 40.728 ms |
| 1 ms | 100.000% | 688.7 | 9.210 ms | 21.380 ms | 34.490 ms | 59.068 ms |
| 10 ms | 100.000% | 761.7 | 10.470 ms | 24.305 ms | 31.574 ms | 50.965 ms |

Run-level latency values and mean callback sizes are medians across three repetitions. The maximum column is the
largest observed value across the corresponding repetitions.

Non-zero aggregation substantially changes notification shape. Relative to zero aggregation, the median callback
count falls from 21,899 to 12,961 at 1 ms and 11,775 at 10 ms, while the mean callback size rises from about 411 to
689 and 762 events. Event latency rises at the same time. This is a batching and queueing trade-off; the experiment
does not establish callback size as an independent cause because aggregation period changes both waiting time and
batch size.

## Monitoring

Every client log contains `MonitoringEndpoint with monitoring.stat=10s` and eight or nine periodic monitoring
samples. The analyzer retained complete measurement intervals and produced client-side metrics for all scenarios.

| Aggregation period | Client read records/s | Server write records/s | Client read lag | Client CPU | Maximum client buffer | Maximum QD dropped |
|---:|---:|---:|---:|---:|---:|---:|
| 0 ms | 148,542.5 | 148,483.2 | 0.536 ms | 1.208% | 0 | 0 |
| 1 ms | 148,479.0 | 148,265.6 | 0.509 ms | 1.046% | 2,864 | 0 |
| 10 ms | 148,692.0 | 148,710.0 | 0.522 ms | 1.018% | 2,621 | 0 |

The client read and server write rates differ by at most 0.144%, which is within the alignment error expected from
independently sampled monitoring intervals. QD reports zero dropped records on both endpoints. All correlated
publications are complete, so the non-zero buffers with aggregation enabled indicate temporary queueing rather than
lost STREAM records. Average CPU is low on both sides (about 1.0-1.2% for the client and 3.4% for the server).

## Interpretation for the customer investigation

The isolated native C++/Graal/QD path sustains this 150,000-events/s workload without missing listener events or
reported QD drops. It does not reproduce pathological latency spikes: run-level p99.9 remains below 36 ms and the
largest individual event latency is below 60 ms. It does reproduce a stable increase in listener latency when
notifications are deliberately aggregated into larger callbacks.

This distinction matters when comparing the new C++ API with the legacy C API, whose callback path commonly exposes
one event at a time. Measurements based only on callback arrival intervals can attribute batching delay to API
latency even when transport delivery is complete. The result does not prove that the customer's complete production
path is healthy: it intentionally excludes external gateways and network variability, uses 375 actively updated
symbols rather than the customer's full subscription cardinality, and does not yet model TimeAndSale history and
snapshot traffic.

The immediately preceding run without working STREAM_FEED client monitoring had lower latency by roughly 0.5-0.7 ms
at p50 and 1.8-2.3 ms at p99 across the three scenarios. The consistent shift suggests monitoring overhead, but two
separate runs are not a controlled A/B measurement, so machine scheduling noise cannot be excluded.

Source files:

- [`REPORT.md`](REPORT.md)
- [`latency-runs.csv`](latency-runs.csv)
- [`latency-comparison.csv`](latency-comparison.csv)
- [`monitoring.csv`](monitoring.csv)
- [`monitoring-comparison.csv`](monitoring-comparison.csv)
- [`run-manifest.csv`](run-manifest.csv)
