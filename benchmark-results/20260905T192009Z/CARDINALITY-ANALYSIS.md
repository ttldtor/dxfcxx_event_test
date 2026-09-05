# Symbol-cardinality benchmark

This benchmark keeps the publication cadence and recurring event rate constant while changing only the subscribed
symbol universe. Every publication contains 375 Quote, 375 Trade, 375 TradeETH, and 375 Summary events. Publications
occur every 10 ms, for a nominal total of 150,000 events/s.

## Results

| Subscribed symbols | Approximate update interval per record key | Listener coverage | Listener deficit | Event p50 | Event p99 | Client read lag | Client CPU | QD storage records |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 375 | 10 ms | 99.855% | 26,123 | 2.410 ms | 9.438 ms | 0.596 ms | 1.189% | 1,875 |
| 3,750 | 100 ms | 100.000% | 0 | 2.984 ms | 11.439 ms | 0.698 ms | 1.332% | 18,750 |
| 10,000 | about 267 ms | 100.000% | 0 | 3.129 ms | 13.097 ms | 0.755 ms | 1.345% | 50,000 |

All nine runs passed. Both QD endpoints reported zero dropped records. Server write rate and client read rate remained
close to 150,000 records/s for every universe size. Endpoint buffers remained zero except for a one-record client
maximum in one 10,000-symbol run.

QD storage contains five record families per symbol: Quote, Trade, TradeETH, Summary, and Profile. The additional
control subscription accounts for subscription counts of 1,876, 18,751, and 50,001.

## Interpretation

The listener deficit disappears when updates are spread over more record keys, even though total throughput,
publication cadence, batch size, event-type mix, endpoint role, and listener code remain unchanged. This result is
not consistent with a simple 150,000-events/s capacity limit. It is consistent with `FEED` retaining the latest
state for a TICKER record key when repeated updates overtake listener delivery.

The 375-symbol result reproduces the earlier normal-`FEED` diagnostic. Its three listener deficits are 26,123,
26,674, and 15,937 events. Summary has the largest median deficit (9,873), followed by TradeETH (5,717), Trade
(5,250), and Quote (4,979). At 3,750 and 10,000 symbols, every event type has zero deficit in every repetition.

Increasing cardinality has a measurable but modest latency cost. Median event p50 rises from 2.410 to 3.129 ms and
p99 from 9.438 to 13.097 ms. Client CPU rises from about 1.19% to 1.35%. This is expected because subscription and
storage state scale from 1,875 to 50,000 records even though event throughput stays fixed.

## Limitations

- The benchmark records QD storage counts but does not sample operating-system working-set or private-memory usage.
  It therefore confirms state growth, not its exact byte cost.
- The 10,000-symbol rotation advances by 375 symbols per publication. Its per-key interval is an average because
  10,000 is not divisible by 375.
- The result supports per-key conflation but does not independently distinguish publisher-side from client-side
  supersession. The separate listener-delay experiment demonstrates that client-side `FEED` conflation is sufficient
  to produce this behavior.
- Event types are currently published in a fixed Quote, Trade, TradeETH, Summary order. Summary's larger deficit may
  therefore be partly an ordering effect.

## Next experiment

Repeat the 375-symbol profile with multiple event-type orders while keeping quantities, cadence, and throughput
unchanged. If the largest deficit follows the last type in the publication, the current per-type difference is an
ordering or serialization artifact rather than an event-class-specific limitation.
