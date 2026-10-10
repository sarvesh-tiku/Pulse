# Failure handling

Every failure below can be injected on demand, has a defined response, and is
covered by an automated test. The "drill" commands reproduce them from the
command line; outputs shown are from real runs (M1 Pro, macOS).

| Failure | Injected by | Detected by | Response | Test |
|---|---|---|---|---|
| Packet loss | `--faults drop=P` | sequencer: `seq > expected` | buffer later frames, request retransmission, block new orders while the gap is open | `FeedHandler.RecoversEveryFaultTypeFromRewindStore` |
| Burst loss | `burst=P,burst_len=N` | same | same; gaps ≥ 64 msgs also pull working orders | `Pipeline.RecoveredFaultyFeedMatchesCleanFeed` |
| Duplicates | `dup=P` | `seq < expected` or already buffered | dropped, counted | `Sequencer.DropsDuplicates` |
| Reordering | `reorder=P` | early frame opens a gap, late frame fills it | healed passively or via rewind | `FeedHandler.WithoutRewindReorderIsHealedPassively` |
| Corrupt frame | `corrupt=P` | CRC32C mismatch | dropped at decode → becomes a gap → same recovery | `Codec.DetectsEverySingleBitFlipOutsideLength` |
| Truncated datagram | `truncate=P` | length > bytes available | rest of datagram discarded → gap → recovery | feed-handler fault test |
| Trailing loss | loss of the last frames | heartbeat announces next seq | gap → recovery | `Sequencer.HeartbeatRevealsTrailingLoss` |
| Unrecoverable gap | no rewind source / window overflow | recovery fails or patience exhausted | **Stale**: pull all orders, halt for the session | `Pipeline.WithoutRecoveryLossHaltsTrading` |
| Queue saturation | slow consumer | `push` fails → spins counted | bounded back-pressure; conflation on the book stage | `QueueStress.*`, `full-spins` in reports |
| Venue disconnect | `--disconnect A:B` | `SessionDown` report | block new orders, venue cancels resting orders; on reconnect: resend in-flight reports, then cancels | `Exchange.DisconnectCancelsRestingAndReplaysAfterReconnect`, `Pipeline.VenueDisconnectIsReconciled` |
| Requests lost while down | orders sent during disconnect | no response | `Expired` after timeout, exposure released | `Oms.UnansweredOrdersExpire` |
| Out-of-order reports | `--reorder-reports` | fill before ack, fill after cancel | fills applied exactly once regardless of state | `Oms.FillBeforeAckAndDuplicateReports`, `Oms.LateFillAfterCancelStillCounts` |
| Venue rejects | `--reject-prob P` | `Rejected` report | order terminal, exposure released | exchange tests |
| Runaway strategy | `--strategy runaway` | rate limiter, open-order / position limits | throttle → **kill switch**, cancel everything | `Pipeline.RunawayStrategyIsContainedByKillSwitch` |
| Strategy crash | `--strategy crash` | exception out of `on_book` | strategy disabled, its orders cancelled, others keep trading | `Pipeline.CrashingStrategyIsIsolated` |
| Loss limit | market moves | total P&L < −max_loss | kill switch | `Risk.MaxLossTrips` |

## The sequence-gap walkthrough

```
Expected sequence: 88291
Received sequence: 88293

→ frame 88293 buffered in the reorder window (slot = 88293 & (W-1))
→ FeedGap{first=88291, last=88292} emitted in-band
     core: gap_open → risk blocks new orders (MarketDataStale)
           gap < 64 msgs → working orders stay
→ retransmission request [88291, 88292]  (TCP rewind or session store)
→ frames arrive flagged kFlagRetransmit; 88291 delivered, 88292 delivered,
  88293 drained from the buffer
→ FeedRecovered{next=88294} emitted in-band → new orders allowed again
```

If the retransmission fails, or (without a rewind source) the gap stays open
for more than `passive_patience` further messages, or a frame arrives beyond
the reorder window, the sequencer gives up: it skips to the next buffered
message and emits `FeedStale`. The book may now be missing deletes, so the
core pulls every order and halts trading for the session. A production system
would request a snapshot here; that is listed as future work.

## Drills

See the "Failure drills" section of the README for commands and captured
output.
