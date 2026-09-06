# STREAM_FEED aggregation-period control

This control follows the FEED aggregation-period benchmark in `benchmark-results/20260905T224524Z`. It keeps the
same shuffled 375-symbol workload, 1,500 recurring events per publication, 10 ms publication cadence, and nominal
150,000 events/s rate. Only the client subscription contract and its aggregation period vary. Every scenario was
repeated three times.

## Results

| Aggregation period | Listener coverage | Listener deficit | Market callbacks | Mean events/callback | Event p50 | Event p99 |
|---:|---:|---:|---:|---:|---:|---:|
| 0 ms | 100.000% | 0 | 20,588 | 439.4 | 3.206 ms | 13.799 ms |
| 1 ms | 100.000% | 0 | 13,469 | 668.1 | 8.664 ms | 21.018 ms |
| 10 ms | 100.000% | 0 | 11,843 | 758.6 | 10.447 ms | 26.598 ms |

The table reports the median of three runs. Callback counts and weighted mean callback sizes include the complete
measurement interval.

## Interpretation

`STREAM_FEED` delivered every correlated recurring event at all three aggregation periods. Non-zero aggregation
still had a clear batching effect: the median callback count fell by 35% at 1 ms and 42% at 10 ms, while the mean
callback size increased from about 439 to 668 and 759 events. Event latency increased at the same time.

This separates notification aggregation from event supersession. The aggregation period can postpone and combine
listener notifications without inherently discarding stream records. Under otherwise identical FEED tests,
listener coverage fell from 99.806% at 0 ms to 75.627% at 1 ms and 68.917% at 10 ms. The strong contract-dependent
difference is consistent with intermediate TICKER states being superseded while FEED delivery is delayed; it is not
consistent with aggregation alone losing arbitrary transport records.

All runs report zero listener deficit and the server reports zero QD `Dropped`. Client QD monitoring remains `n/a`
because the current `STREAM_FEED` endpoint does not emit the monitoring-stat lines parsed by the analyzer. Exact
listener accounting is nevertheless available and complete.

## Integrity notes

Two of nine runs reported one uncorrelated 1,500-event publication and one missing control marker. The market-event
counts still match the correlated publication counts exactly, and the other seven runs have no marker anomaly. The
marker uses a separate subscription, so shutdown or window-boundary ordering can separate it from the corresponding
market events. This is a correlation-boundary issue, not evidence of lost market events, but it makes the analyzer
return exit code 1 and mark those two runs `CHECK`.

## Conclusion

The control strengthens the client-side FEED supersession explanation. Connector-read counters from the FEED runs
already matched server-write counters with zero reported drops. This experiment adds that the same aggregation
periods preserve 100% of updates when the client uses the non-conflating STREAM contract. The remaining unknown is
the exact internal queue or notification boundary at which FEED replaces an older state with a newer state.

Source files:

- [`REPORT.md`](REPORT.md)
- [`latency-runs.csv`](latency-runs.csv)
- [`latency-comparison.csv`](latency-comparison.csv)
- [`monitoring-comparison.csv`](monitoring-comparison.csv)
- [`run-manifest.csv`](run-manifest.csv)

