# SDK release-stack comparison

This document records controlled release-stack comparisons using the same benchmark revision, compiler, host, and
workloads. The original experiment compares v5.0.0 with v7.0.0 in both `STREAM_FEED` and conflating `FEED` modes. A
follow-up compares v7.0.0 with v8.0.0 in `FEED` mode after the packed TimeAndSale field fix and Native SDK update.

| Stack | dxFeed Graal CXX API | Graal Native SDK | Embedded `qd.version` |
|---|---:|---:|---:|
| Older | 5.0.0 | 2.6.2 | 3.342 |
| Previous | 7.0.0 | 3.2.0 | 3.347 |
| Current | 8.0.0 | 3.2.13 | 3.353 |

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

## v7.0.0 to v8.0.0 FEED follow-up

The follow-up uses benchmark commit `ce767551fc7393c50e6ca3d28c0d5554305e8708` and the same shuffled
375-symbol, 150,000-events/s FEED control. It was run in A-B-A order to expose host drift: three v7 repetitions,
three v8 repetitions, and then three more v7 repetitions. No rebuild occurred between the measured blocks, and the
two build directories used the same compiler and benchmark sources.

| Block | Stack | Runs | Coverage | Event p50 | Event p99 | Event p99.9 | Callbacks | Client CPU | Server CPU |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| A1 | v7.0.0 / SDK 3.2.0 / QD 3.347 | 3 | 99.868% | 2.713 ms | 10.237 ms | 14.394 ms | 28,325 | 0.749% | 2.240% |
| B | v8.0.0 / SDK 3.2.13 / QD 3.353 | 3 | 99.840% | 2.841 ms | 10.996 ms | 15.420 ms | 28,261 | 0.764% | 2.439% |
| A2 | v7.0.0 / SDK 3.2.0 / QD 3.347 | 3 | 99.827% | 2.867 ms | 10.649 ms | 14.681 ms | 28,191 | 0.825% | 2.287% |

The direct A1-to-B comparison would suggest increases of 4.7% at p50, 7.4% at p99, and 7.1% at p99.9. The A2
control shows that the host also drifted during the experiment: its v7 p50 is slightly above v8 and its v7 p99 is
closer to v8 than A1 was. Combining the six v7 run-level observations gives medians of 2.780 ms, 10.569 ms, and
14.537 ms. Relative to those values, the three v8 medians are higher by 2.2%, 4.0%, and 6.1%, respectively.

Every aggregate v7 and v8 latency range overlaps. The upward central estimate is also present in individual event
types, most visibly in Trade p99, but nine sequential observations are not sufficient to distinguish a small stack
effect from host drift. The conservative result is therefore that this test does not establish a v8 latency
regression. It does identify a small signal worth checking with more balanced interleaved repetitions if a
few-percent change is operationally important. Nothing resembles the customer's large latency spikes.

Delivery behavior is stable. Combined v7 coverage is 99.8386% versus 99.8404% for v8, a difference of only 0.0018
percentage points. All nine runs have zero missing batches and clock anomalies. Client and server QD monitoring
report zero `Dropped`; client buffer high-water marks are zero and server maxima are at most one record. Callback
counts and CPU use are comparable and show no saturation. As in the earlier FEED experiment, the small listener gap
is compatible with normal TICKER supersession and is not evidence of transport loss.

The separate v8 TimeAndSale test validates HISTORY snapshot-to-live behavior, but it is not a v7/v8 performance
comparison: the v7 packed-index bug corrupts `TimeAndSale` timestamps when `setSequence()` follows `setTimeNanos()`,
so a valid equivalent v7 HISTORY baseline cannot be produced through the public setters.

## Conclusion and limitations

The original v5-to-v7 controls show no end-to-end latency or delivery regression: `STREAM_FEED` delivery remains
complete and v7 FEED median latency is lower. The v7-to-v8 follow-up also shows stable delivery and resources. Its
v8 latency medians are slightly higher than the combined v7 controls, but the ranges overlap and temporal drift is
visible, so the available observations do not establish a regression. None of the controls reproduces the
customer's large latency spikes.

The experiments compare complete release stacks. They cannot attribute a measured difference specifically to the
CXX API, Native SDK, or the QD updates from 3.342 to 3.347 and then 3.353. They run over loopback on one Windows host
and model actively ticking symbols rather than the customer's full subscription universe. The legacy customer
client also used the old C API, which is not part of this release-stack comparison. That API does not implement the
newer client-side FEED conflation mechanism, delivers events to its callback one at a time, and does not support the
`TextMessage` event used for exact publication correlation here. A legacy-client comparison therefore requires a
different, separately validated wire marker and explicit current-API `FEED` and `STREAM_FEED` controls. The
experiments isolate the current C++ API delivery path under controlled load; they do not certify the complete
production topology.

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
- [v7 A1 FEED report](20260906T215026Z/REPORT.md)
- [v7 A1 FEED environment](20260906T215026Z/environment.txt)
- [v8 FEED report](20260906T215855Z/REPORT.md)
- [v8 FEED environment](20260906T215855Z/environment.txt)
- [v7 A2 FEED report](20260906T220817Z/REPORT.md)
- [v7 A2 FEED environment](20260906T220817Z/environment.txt)
- [v8 TimeAndSale snapshot-to-live report](20260906T213400Z/REPORT.md)
