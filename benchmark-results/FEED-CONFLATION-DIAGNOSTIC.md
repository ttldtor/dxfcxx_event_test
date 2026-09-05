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
- The client monitoring lines are not emitted for the current `STREAM_FEED` endpoint, so its client monitoring
  values are reported as `n/a`, not zero. Exact listener accounting still shows zero deficit in all three runs.
- Small `excess_events` values in `FEED` runs can occur because control markers and market events use separate
  subscriptions and can be observed in a different order near publication boundaries. The report marks these runs
  as `CHECK`; this does not change the large role-dependent difference.
- The current workload uses 375 symbols and reproduces an event rate, not the customer's full subscription
  cardinality. Symbol-cardinality experiments should keep the event rate constant while rotating updates across a
  larger subscribed universe.

## Next experiment

Increase the subscribed symbol universe without increasing the 150,000 events/s publication rate. Rotate each
publication across that universe and compare `FEED` with `STREAM_FEED`. This separates the effect of record-key
cardinality and ticker-state retention from the effect of raw throughput.
