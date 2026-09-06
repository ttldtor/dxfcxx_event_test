# STREAM_FEED aggregation-period control

This is the full rerun of the STREAM_FEED aggregation control after correcting publication correlation at the
warm-up and shutdown boundaries. It uses the same shuffled 375-symbol workload, 1,500 recurring events per
publication, 10 ms publication cadence, and nominal 150,000 events/s rate. Each aggregation period was repeated
three times.

## Results

| Aggregation period | Listener coverage | Listener deficit | Market callbacks | Mean events/callback | Event p50 | Event p99 |
|---:|---:|---:|---:|---:|---:|---:|
| 0 ms | 100.000% | 0 | 20,732 | 436.8 | 2.918 ms | 10.660 ms |
| 1 ms | 100.000% | 0 | 13,027 | 692.3 | 8.682 ms | 19.625 ms |
| 10 ms | 100.000% | 0 | 11,868 | 759.0 | 9.912 ms | 21.987 ms |

The table reports the median of three runs. Callback counts and weighted mean callback sizes include the complete
measurement interval and the short, correlated shutdown tail.

## Integrity

All nine clients exited with code zero and every run reports `Integrity = OK`. Every correlated recurring event was
delivered: there are no listener deficits, excess events, uncorrelated events, missing batches, pending batches, or
clock anomalies. The server reports zero QD `Dropped` records in every scenario.

This confirms that the two historical `CHECK` results were benchmark correlation artifacts. They do not reproduce
after the client excludes complete publication sequences that cross the warm-up boundary and retains a dedicated
timestamp-marker symbol until shutdown delivery becomes quiet.

## Interpretation

Aggregation changes notification shape and latency without losing STREAM records. Relative to zero aggregation,
the 1 ms period reduces the median market callback count by about 37% and increases the mean callback size from
about 437 to 692 events. The 10 ms period reduces callbacks by about 43% and increases the mean callback size to
about 759 events. Median and tail latency rise accordingly.

The result reproduces the important conclusion of the earlier control while removing its integrity caveat:
`STREAM_FEED` preserves 100% of recurring updates at all three aggregation periods. By contrast, the otherwise
equivalent FEED experiment delivered 99.806%, 75.627%, and 68.917% at 0, 1 ms, and 10 ms. Delayed notification alone
therefore does not discard arbitrary records; the loss of intermediate states depends on the conflating FEED
contract.

Client-side QD monitoring remains `n/a` because this STREAM_FEED endpoint does not emit the periodic lines recognized
by the parser. It must not be interpreted as zero. Exact listener accounting and server-side monitoring remain
available.

Source files:

- [`REPORT.md`](REPORT.md)
- [`latency-runs.csv`](latency-runs.csv)
- [`latency-comparison.csv`](latency-comparison.csv)
- [`monitoring-comparison.csv`](monitoring-comparison.csv)
- [`run-manifest.csv`](run-manifest.csv)
