# QD FEED delivery-path analysis

This note connects the benchmark's black-box FEED/STREAM_FEED results to the public QD and dxFeed Graal CXX API
implementations. The QD links are pinned to public QD commit
[`60cd687dcf84daf4691b4762c86c0bcf0d4f1493`](https://github.com/devexperts/QD/tree/60cd687dcf84daf4691b4762c86c0bcf0d4f1493),
tagged as release 3.354. The benchmark version comparison uses QD 3.342 and 3.347 as embedded in the corresponding
Native SDK artifacts, so the source trace demonstrates the current design and is not a byte-for-byte audit of those
two older QD releases.

## Contract selection

The public `DXEndpoint.Role` documentation says that normal `FEED` dynamically conflates data when event-processing
threads cannot keep up. It separately states that `STREAM_FEED` does not conflate or skip file events:
[`DXEndpoint.java`, lines 440-470](https://github.com/devexperts/QD/blob/60cd687dcf84daf4691b4762c86c0bcf0d4f1493/dxfeed-api/src/main/java/com/dxfeed/api/DXEndpoint.java#L440-L470).

The implementation makes that distinction concrete. `STREAM_FEED` and `STREAM_PUBLISHER` instantiate only the
`QDContract.STREAM` collector; ordinary roles instantiate TICKER, STREAM, and HISTORY collectors:
[`DXEndpointImpl.java`, lines 205-208](https://github.com/devexperts/QD/blob/60cd687dcf84daf4691b4762c86c0bcf0d4f1493/dxfeed-impl/src/main/java/com/dxfeed/api/impl/DXEndpointImpl.java#L205-L208).
Stream roles also request stream-only event delegates:
[`DXEndpointImpl.java`, lines 528-530](https://github.com/devexperts/QD/blob/60cd687dcf84daf4691b4762c86c0bcf0d4f1493/dxfeed-impl/src/main/java/com/dxfeed/api/impl/DXEndpointImpl.java#L528-L530).

For the market types used by this benchmark, ordinary subscription delegates use the TICKER contract, while
stream-only delegates use STREAM:
[`MarketFactoryImpl.java`, lines 374-473](https://github.com/devexperts/QD/blob/60cd687dcf84daf4691b4762c86c0bcf0d4f1493/dxfeed-impl/src/main/java/com/dxfeed/event/market/MarketFactoryImpl.java#L374-L473).

## Where an intermediate FEED state disappears

The TICKER collector stores every incoming record into keyed storage, then schedules subscribed agents:
[`Ticker.java`, lines 176-204](https://github.com/devexperts/QD/blob/60cd687dcf84daf4691b4762c86c0bcf0d4f1493/qd-core/src/main/java/com/devexperts/qd/impl/matrix/Ticker.java#L176-L204).
The key is the record and symbol pair. Scheduling uses one update-queue entry for that subscribed key:
[`Ticker.java`, lines 210-235](https://github.com/devexperts/QD/blob/60cd687dcf84daf4691b4762c86c0bcf0d4f1493/qd-core/src/main/java/com/devexperts/qd/impl/matrix/Ticker.java#L210-L235).

`AgentQueue.linkToQueue` does not append a second entry when the same item is already queued; it only ensures that
the queue bit remains set:
[`AgentQueue.java`, lines 70-100](https://github.com/devexperts/QD/blob/60cd687dcf84daf4691b4762c86c0bcf0d4f1493/qd-core/src/main/java/com/devexperts/qd/impl/matrix/AgentQueue.java#L70-L100).
When the agent later retrieves the queued item, it asks TICKER storage for the current record data:
[`AgentQueue.java`, lines 116-147](https://github.com/devexperts/QD/blob/60cd687dcf84daf4691b4762c86c0bcf0d4f1493/qd-core/src/main/java/com/devexperts/qd/impl/matrix/AgentQueue.java#L116-L147).

Therefore, if the same record and symbol is updated again before its queued notification is retrieved, keyed TICKER
storage contains the newer state while the queue still has one entry. The application observes the latest state,
not both transitions. This is normal TICKER supersession; it is not an endpoint-buffer overflow and need not
increment the QD `Dropped` counter.

The receiving `DXFeedImpl` converts retrieved records into a new event list and then dispatches it to subscription
listeners:
[`DXFeedImpl.java`, lines 961-976](https://github.com/devexperts/QD/blob/60cd687dcf84daf4691b4762c86c0bcf0d4f1493/dxfeed-impl/src/main/java/com/dxfeed/api/impl/DXFeedImpl.java#L961-L976).
The CXX API receives that already-constructed native event list, maps it to C++ objects, and synchronously invokes
the listener; it does not add another record-and-symbol conflation store in this callback path:
[`DXFeedSubscription.cpp`, lines 24-69](https://github.com/dxFeed/dxfeed-graal-cxx-api/blob/v7.0.0/src/api/DXFeedSubscription.cpp#L24-L69).

## STREAM_FEED behavior

In addition to selecting STREAM rather than TICKER, `DXFeedImpl` explicitly sets the receiving agent's overflow
strategy to `BLOCK` for `STREAM_FEED`:
[`DXFeedImpl.java`, lines 820-826](https://github.com/devexperts/QD/blob/60cd687dcf84daf4691b4762c86c0bcf0d4f1493/dxfeed-impl/src/main/java/com/dxfeed/api/impl/DXFeedImpl.java#L820-L826).
This explains why a STREAM_FEED consumer can retain every intermediate update while showing temporary buffering or
backpressure instead of ordinary TICKER supersession.

## How this maps to the benchmark evidence

- With the same 150,000 events/s workload, changing only `FEED` to `STREAM_FEED` removed the listener deficit.
- Adding a 1 ms sleep at the beginning of the C++ FEED listener reduced coverage to about 39%, even though client
  connector read rate continued to match server write rate and both endpoints reported `Dropped = 0`.
- Increasing the aggregation period reduced FEED coverage but left STREAM_FEED coverage at exactly 100%.
- Increasing symbol cardinality at a fixed event rate removed the small undelayed FEED deficit because consecutive
  updates were spread across more record-and-symbol keys.

Together with the source path above, these controls locate the induced FEED deficit on the receiving side after
network read and before C++ listener invocation. Low average CPU and network utilization do not contradict this:
supersession requires only that a newer state for the same key enter TICKER storage before the existing queue entry
is retrieved.

The evidence does not prove that every observation gap in the customer's environment has this cause. It does show
that listener event counts cannot be interpreted as transport-loss counts when the client uses normal FEED
semantics. See [`FEED-CONFLATION-DIAGNOSTIC.md`](FEED-CONFLATION-DIAGNOSTIC.md) for the controlled measurements.
