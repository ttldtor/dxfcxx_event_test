# FEED conflation diagnostic

This note compares three runs of the same 150,000 recurring events/s workload. The publisher task, symbol set,
publication cadence, warm-up, measured duration, and repetition count are identical. Only the client endpoint role
or an artificial delay at the beginning of the market-event listener changes.

## Results

| Client mode | Listener delay | Listener coverage | Listener deficit | Event p50 | Event p99 | Client read records/s | Server write records/s | QD dropped |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `FEED` | 0 ms | 99.872% | 24,190 | 2.354 ms | 9.403 ms | 149,514.364 | 149,514.545 | 0 |
| `STREAM_FEED` | 0 ms | 100.000% | 0 | 2.754 ms | 10.861 ms | n/a | 149,739.364 | 0 |
| `FEED` | 1 ms | 39.192% | 10,917,779 | 21.788 ms | 28.440 ms | 149,592.455 | 149,593.273 | 0 |

Source reports:

- [`FEED`, no listener delay](20260905T180542Z/REPORT.md)
- [`STREAM_FEED`, no listener delay](20260905T181426Z/REPORT.md)
- [`FEED`, 1 ms listener delay](20260905T182740Z/REPORT.md)

## Interpretation

Changing only the endpoint contract from `FEED` to `STREAM_FEED` removes the listener deficit. This rules out the
publisher workload, the local transport, and the event generator as sufficient explanations for the deficit.

The delayed `FEED` run provides stronger localization. The delay is executed at the start of the C++ market-event
listener, while the control-marker listener remains undelayed. Despite listener coverage falling to about 39%, the
client connector still reads approximately 149,592 records/s, matching the server write rate, and neither endpoint
reports QD drops. Therefore, records continue to cross the connection while intermediate `TICKER` states are
superseded before they are observed by the application listener. At least this induced loss of intermediate states
is client-side `FEED` conflation, not transport loss and not a QD `Dropped` event.

Low CPU utilization is expected in the delayed run: the callback spends most of its time sleeping. Conflation does
not require CPU, network, or endpoint-buffer saturation. It only requires a newer state for the same record and
symbol to arrive before the application consumes the previous state.

`STREAM_FEED` preserves every update in this workload, at the cost of somewhat higher latency and observable server
buffering (maximum 1,218 records versus zero in the normal `FEED` run). This is consistent with a stream contract
that queues updates instead of retaining only the latest ticker state.

## Scope and limitations

- The experiment proves that client-side `FEED` processing can produce the observed kind of listener deficit. It
  does not prove that every missing observation in the undelayed baseline is client-side; publisher-side
  conflation would require finer instrumentation to exclude completely.
- The historical `STREAM_FEED` run reports client monitoring as `n/a` because the benchmark originally set
  `monitoring.stat` as a Java system property. That role does not import system properties into its endpoint
  configuration. The benchmark now passes the property directly to `DXEndpoint::Builder`; the corrected control in
  [`20260906T111249Z`](20260906T111249Z/AGGREGATION-STREAM-CONTROL.md) contains client and server monitoring samples.
  Exact listener accounting in the historical run still shows zero deficit in all three repetitions.
- Historical `CHECK` results near measurement boundaries can include correlation artifacts because timestamp
  markers and market events use separate subscriptions. The client now excludes complete sequences observed during
  warm-up and retains a separate marker symbol through shutdown. Existing result CSV files are intentionally not
  rewritten; this correction does not change the large role-dependent coverage difference.
- The current workload uses 375 symbols and reproduces an event rate, not the customer's full subscription
  cardinality. Symbol-cardinality experiments should keep the event rate constant while rotating updates across a
  larger subscribed universe.

## Next experiment

Increase the subscribed symbol universe without increasing the 150,000 events/s publication rate. Rotate each
publication across that universe and compare `FEED` with `STREAM_FEED`. This separates the effect of record-key
cardinality and ticker-state retention from the effect of raw throughput.

This experiment is complete: [`Symbol-cardinality benchmark`](20260905T192009Z/CARDINALITY-ANALYSIS.md). With the
same 150,000 events/s workload, listener coverage is 99.855% for 375 symbols and exactly 100% for both 3,750 and
10,000 symbols. Spreading updates over more record keys removes the deficit while modestly increasing latency and
QD storage state. The next controlled variable is event-type order.

The event-order experiment is also complete: [`Event-type order benchmark`](20260905T195407Z/EVENT-ORDER-ANALYSIS.md).
In all four permutations, the event type placed last has the largest median listener deficit. The previously larger
Summary deficit is therefore mainly a position effect rather than a Summary-specific limitation. A deterministic
per-publication shuffle of the four type blocks confirms this result:
[`Shuffled event-type order benchmark`](20260905T202837Z/SHUFFLED-ORDER-ANALYSIS.md). The shuffle distributes the
deficit much more evenly between types and has no measurable generator cost, but it does not reduce the total
deficit. This is a diagnostic control, not a proposed product workaround.

The black-box controls now consistently point to per-record-key `FEED` supersession and a position-dependent window
between record processing and C++ listener observation. The next useful step is targeted instrumentation or source
tracing at the QD-to-listener boundary rather than further workload randomization.

That source trace is now documented in [`QD-FEED-DELIVERY-PATH.md`](QD-FEED-DELIVERY-PATH.md). It identifies the
QD TICKER storage update and per-key agent queue as the place where repeated states can be coalesced before Java
event construction and the native C++ callback. It also shows that `STREAM_FEED` selects the STREAM contract and
sets the receiving agent overflow strategy to `BLOCK`.

The notification batch-limit experiment provides the next boundary:
[`Event notification batch-limit benchmark`](20260905T210242Z/BATCH-LIMIT-ANALYSIS.md). With the limit forced to one,
the connector still reads approximately 149,439 records/s with zero reported drops, while the listener sees only
26.121% of the correlated states. The supersession is therefore client-side, after connector read and before the
native subscription callback. Ordinary limits change callback shape but do not produce a monotonic coverage change;
`STREAM_FEED` continues to deliver exactly 100%.

The aggregation-period controls further separate delayed notification from supersession. FEED coverage falls to
75.627% at 1 ms and 68.917% at 10 ms, while otherwise identical STREAM_FEED controls preserve exactly 100% at both
periods. STREAM_FEED still produces fewer, larger callbacks and higher latency, proving that aggregation is active
without discarding stream records. See the
[`corrected STREAM_FEED aggregation-period control`](20260906T103104Z/AGGREGATION-STREAM-CONTROL.md). All nine
rerun results have `Integrity = OK`; the previous boundary-correlation caveat no longer applies to this control.
