# Event-type order benchmark

This benchmark keeps the 375-symbol, 1,500-event, 10 ms `FEED` workload fixed and changes only the order of the four
event-type blocks passed to `publishEvents()`.

## Results

| Last block | Overall listener coverage | Overall listener deficit | Quote deficit | Trade deficit | TradeETH deficit | Summary deficit |
|---|---:|---:|---:|---:|---:|---:|
| Summary | 99.850% | 13,587 | 2,911 | 2,801 | 3,244 | **4,631** |
| Quote | 99.870% | 12,356 | **4,446** | 2,536 | 2,625 | 2,961 |
| Trade | 99.843% | 14,772 | 3,922 | **4,832** | 3,018 | 3,000 |
| TradeETH | 99.906% | 8,665 | 1,815 | 1,870 | **3,785** | 1,875 |

Each value is the median of three independent runs. All twelve runs passed. Server write and client read rates remain
close to 150,000 records/s, endpoint buffers remain zero, and both QD endpoints report zero dropped records.

## Interpretation

The event type placed last has the largest median listener deficit in every permutation. The earlier observation that
Summary loses more listener-visible states is therefore not an event-class-specific limitation. It is primarily a
serialization or processing-position effect.

The result is consistent with a batch being decoded or applied in order while the next publication begins replacing
the same 375 TICKER keys. Records near the end of the current publication have less time to become visible to the
application listener before the following publication updates those keys again.

Overall deficit varies between permutations, so position is not the only source of run-to-run variation. The
important invariant is that the largest per-type deficit follows the last block in all four cases.

## Shuffled control

The proposed deterministic shuffle is complete: [Shuffled event-type order benchmark](../20260905T202837Z/SHUFFLED-ORDER-ANALYSIS.md).
Long-run deficits are distributed much more evenly between event types, confirming the position effect. Shuffling
four block indices adds no measurable generator overhead, but it does not reduce the total listener deficit and is
therefore a diagnostic control rather than a mitigation.

Shuffling individual events inside each 375-event block is not needed for this question. It would add work and alter
memory-access locality without improving the event-type position test.
