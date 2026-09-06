# SDK release-stack comparison

This experiment compares two release stacks using the same benchmark revision, compiler, host, and controlled
workloads. It answers whether the newer stack shows a regression in the isolated publisher-to-C++-listener path,
both when every update is retained by `STREAM_FEED` and under normal conflating `FEED` semantics.

| Stack | dxFeed Graal CXX API | Graal Native SDK | Embedded `qd.version` |
|---|---:|---:|---:|
| Older | 5.0.0 | 2.6.2 | 3.342 |
| Newer | 7.0.0 | 3.2.0 | 3.347 |

The CXX API and SDK versions are captured in each run's `environment.txt`. The QD versions were recovered from the
`<qd.version>` Maven property embedded in the corresponding Native SDK DLL. The SDK 2.6.2 DLL also contains a QDS
monitoring manifest with implementation version 3.340; that is a component manifest, whereas 3.342 is the SDK's
declared QD dependency version.

The `STREAM_FEED` control used benchmark commit `771e69bd0566e2fce23fe0ce988c09ca3c6b1f00` plus the non-functional
environment-version recording change. The subsequent `FEED` control used commit
`5df5823ec970685c77faf1dbddac9a8d10aafad9`, which contains that recording change and the `STREAM_FEED` results but
does not alter the event path. Both controls use the same shuffled 375-symbol workload: 1,500 recurring events per
publication every 10 ms, nominally 150,000 events/s. Every profile has a 30-second warm-up, a one-minute
measurement, and three independent repetitions.

## STREAM_FEED results

| Aggregation | Stack | Coverage | Events/callback | Event p50 | Event p99 | Event p99.9 |
|---:|---|---:|---:|---:|---:|---:|
| 0 ms | Older | 100.000% | 336.6 | 3.064 ms | 11.647 ms | 24.289 ms |
| 0 ms | Newer | 100.000% | 332.9 | 2.953 ms | 10.960 ms | 18.726 ms |
| 1 ms | Older | 100.000% | 471.0 | 8.609 ms | 19.798 ms | 35.698 ms |
| 1 ms | Newer | 100.000% | 463.3 | 8.260 ms | 19.237 ms | 27.551 ms |
| 10 ms | Older | 100.000% | 504.5 | 9.931 ms | 23.009 ms | 30.319 ms |
| 10 ms | Newer | 100.000% | 502.9 | 9.777 ms | 22.280 ms | 31.449 ms |

Run-level latency values and callback counts are medians across three repetitions. Events per callback is the median
published event count divided by the median callback count.

The newer stack is lower at p50 and p99 in all three aggregation modes. The change ranges from -1.5% to -4.1% at
p50 and from -2.8% to -5.9% at p99. The same direction appears for every individual recurring event type (`Quote`,
`Trade`, `TradeETH`, and `Summary`), so an aggregate improvement is not hiding a per-type regression.

At p99.9, the newer stack is about 23% lower for 0 and 1 ms aggregation. At 10 ms it is 3.7% higher. The 10 ms
run-level ranges overlap, and individual maxima are unstable in both directions, so this small far-tail difference
does not establish a regression. More repetitions would be required for a claim specifically about p99.9 or maxima.

## Delivery and resources

- All 18 runs report `Integrity = OK`, 100% listener coverage, and zero listener deficit.
- Both client and server report zero QD `Dropped` records in every run.
- Median client read rates stay between 149,229 and 149,810 records/s on the older stack and between 149,463 and
  149,499 records/s on the newer stack. The offered and observed loads are therefore comparable.
- Median server write rates stay between 149,337 and 149,637 records/s on the older stack and between 149,388 and
  149,538 records/s on the newer stack.
- Client CPU is approximately 0.8-1.1% and server CPU approximately 3.3-3.4% on both stacks. There is no CPU
  saturation.
- Callback counts differ by no more than 1.7%. The result is not explained by a material change in notification
  batch shape.

## FEED results

The second control uses `FEED`, the optimal event batch limit, and an explicit aggregation period of zero. It checks
whether the normal TICKER state-replacement behavior or its latency materially changed between the release stacks.

| Stack | Coverage | Listener deficit | Callbacks | Events/callback | Event p50 | Event p99 | Event p99.9 |
|---|---:|---:|---:|---:|---:|---:|---:|
| Older | 99.830% | 16,459 | 27,312 | 330.3 | 2.463 ms | 9.708 ms | 13.948 ms |
| Newer | 99.849% | 14,414 | 25,521 | 354.0 | 2.280 ms | 8.643 ms | 13.169 ms |

The offered load is comparable: the median correlated publication count differs by 0.2%. Listener coverage is
0.020 percentage points higher on the newer stack, and its median listener deficit is 12.4% smaller. Run-level
coverage ranges overlap (`99.824-99.860%` versus `99.841-99.878%`), so this establishes stable behavior rather than
a statistically significant improvement.

The newer stack has 7.4% lower median p50, 11.0% lower median p99, and 5.6% lower median p99.9. The three-run ranges
also overlap, so the conservative conclusion is that there is no latency regression. Each recurring event type
agrees with that conclusion: Quote and Summary coverage improve slightly, Trade is effectively unchanged, and
TradeETH improves slightly. No type shows a material regression hidden by the aggregate.

The recurring listener observes 6.6% fewer callbacks on the newer stack, with 7.2% more events per callback, but
still has lower median latency.
Both stacks report zero QD `Dropped` records, zero client buffer high-water mark, and exactly 1,875 stored market
records. Client CPU is 0.78% versus 0.70%, and server CPU is 2.32% versus 2.35%. The maximum server buffer changes
from zero to one record. None of these values indicates saturation.

Within each stack, the client read and server write rates match closely. The aggregate rate in `REPORT.md` includes
monitoring intervals that overlap startup and the idle drain tail, so its difference between stacks must not be
interpreted as an offered-load difference; correlated publication counts provide the direct workload check.

## Conclusion and limitations

There is no measured end-to-end latency or delivery regression in the newer release stack for either workload.
`STREAM_FEED` delivery remains complete. Normal `FEED` supersession remains small and statistically compatible with
the older stack, while its median latency values are lower. Resource use is comparable. Neither control reproduces
the customer's large latency spikes.

The experiment compares complete release stacks. It cannot attribute the measured difference specifically to the
CXX API, Native SDK, or the QD update from 3.342 to 3.347. It runs over loopback on one Windows host, uses
`STREAM_FEED` or `FEED`, and models 375 actively ticking symbols rather than the customer's full subscription
universe or TimeAndSale snapshot/history traffic. The legacy customer client also used the old C API, which is not
part of this release-stack comparison. The experiment isolates the current C++ API delivery path under controlled
load; it does not certify the complete production topology.

Source results:

- [Older stack report](20260906T115956Z/REPORT.md)
- [Older stack environment](20260906T115956Z/environment.txt)
- [Older stack aggregate latency CSV](20260906T115956Z/latency-comparison.csv)
- [Older stack aggregate monitoring CSV](20260906T115956Z/monitoring-comparison.csv)
- [Newer stack report](20260906T121742Z/REPORT.md)
- [Newer stack environment](20260906T121742Z/environment.txt)
- [Newer stack aggregate latency CSV](20260906T121742Z/latency-comparison.csv)
- [Newer stack aggregate monitoring CSV](20260906T121742Z/monitoring-comparison.csv)
- [Older stack FEED report](20260906T125418Z/REPORT.md)
- [Older stack FEED environment](20260906T125418Z/environment.txt)
- [Older stack FEED aggregate latency CSV](20260906T125418Z/latency-comparison.csv)
- [Older stack FEED aggregate monitoring CSV](20260906T125418Z/monitoring-comparison.csv)
- [Newer stack FEED report](20260906T130202Z/REPORT.md)
- [Newer stack FEED environment](20260906T130202Z/environment.txt)
- [Newer stack FEED aggregate latency CSV](20260906T130202Z/latency-comparison.csv)
- [Newer stack FEED aggregate monitoring CSV](20260906T130202Z/monitoring-comparison.csv)
