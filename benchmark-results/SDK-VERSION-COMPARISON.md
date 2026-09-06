# SDK release-stack comparison

This experiment compares two release stacks using the same benchmark revision, compiler, host, workload, and suite
configuration. It answers whether the newer stack shows a regression in the isolated publisher-to-C++-listener path.

| Stack | dxFeed Graal CXX API | Graal Native SDK | Embedded `qd.version` |
|---|---:|---:|---:|
| Older | 5.0.0 | 2.6.2 | 3.342 |
| Newer | 7.0.0 | 3.2.0 | 3.347 |

The CXX API and SDK versions are captured in each run's `environment.txt`. The QD versions were recovered from the
`<qd.version>` Maven property embedded in the corresponding Native SDK DLL. The SDK 2.6.2 DLL also contains a QDS
monitoring manifest with implementation version 3.340; that is a component manifest, whereas 3.342 is the SDK's
declared QD dependency version.

Both stacks were built from benchmark commit `771e69bd0566e2fce23fe0ce988c09ca3c6b1f00` plus the non-functional
environment-version recording change. Each stack ran the same shuffled 375-symbol workload: 1,500 recurring events
per publication every 10 ms, nominally 150,000 events/s. `STREAM_FEED` was used with aggregation periods of 0, 1 ms,
and 10 ms. Every profile had a 30-second warm-up, a one-minute measurement, and three independent repetitions.

## Results

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

## Conclusion and limitations

There is no measured end-to-end latency or delivery regression in the newer release stack for this workload. Its
typical and p99 latency are modestly better, delivery remains complete, and resource use is comparable. This result
also does not reproduce the customer's large latency spikes.

The experiment compares complete release stacks. It cannot attribute the measured difference specifically to the
CXX API, Native SDK, or the QD update from 3.342 to 3.347. It runs over loopback on one Windows host, uses
`STREAM_FEED`, and models 375 actively ticking symbols rather than the customer's full subscription universe or
TimeAndSale snapshot/history traffic. It isolates the API delivery path under controlled load; it does not certify
the complete production topology.

Source results:

- [Older stack report](20260906T115956Z/REPORT.md)
- [Older stack environment](20260906T115956Z/environment.txt)
- [Older stack aggregate latency CSV](20260906T115956Z/latency-comparison.csv)
- [Older stack aggregate monitoring CSV](20260906T115956Z/monitoring-comparison.csv)
- [Newer stack report](20260906T121742Z/REPORT.md)
- [Newer stack environment](20260906T121742Z/environment.txt)
- [Newer stack aggregate latency CSV](20260906T121742Z/latency-comparison.csv)
- [Newer stack aggregate monitoring CSV](20260906T121742Z/monitoring-comparison.csv)
