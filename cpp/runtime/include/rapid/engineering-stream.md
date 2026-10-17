# Engineering telemetry stream

`/api/v1/live?history=<seconds>` returns engineering packets from
`Runtime::events()`. The native `Runtime` owns the event broker; there is no
separate `TraceEventBroker` class in this source tree.

## Additive public fields

- Packet `receiver_stream_id`: 32 lowercase hexadecimal characters generated
  with the existing OpenSSL-backed `unique_id()` once per `Runtime` instance.
  Every engineering packet carries it, including initial history, reconnect
  history, incremental push, empty/idle packets and packets reporting drops.
  A new broker instance gets a new ID. Sender sessions do not change it.
- Sample `event_id`: the broker's existing `next_event_` counter, exposed as a
  positive decimal **string**. It increases once per accepted broker event,
  starting at `"1"`. Replaying history preserves it. It is scoped to
  `receiver_stream_id`, not globally unique by itself. A string avoids loss of
  integer precision in JavaScript.

These are public correlation identifiers, not credentials or restart times.
They do not change companion authentication, replay acceptance, packet formats,
recordings, state-mode display frames or the existing engineering fields.
`native_http.cpp` already serializes `Runtime::events()` for both history and
live engineering push, so there is one packet constructor for this contract.

## Viewer policy

Age and plot samples by finite `received_monotonic` seconds, including zero.
Within an identified receiver stream, the axis is the maximum accepted sample
receipt time. Older replay fills gaps without rewinding the axis. Sender clock
regression, sender session changes and browser arrival do not establish epochs.

A valid changed stream ID clears history, duplicate keys and the axis before
accepting the packet, even if the packet contains no samples. This also applies
when an identified broker first replaces legacy/unidentified history. A broker
restart on the same device boot conservatively starts a fresh viewer history,
even when its steady clock did not reset.

Deduplicate identified events by `(receiver_stream_id, event_id)`. Distinct
broker events remain distinct even if all sample data and receipt times match.
Ignore callbacks from a closed/superseded WebSocket, including late messages and
close callbacks, so the old connection cannot restore its epoch.

Viewer pause ignores entire incoming packets, including epoch transitions.
History, keys and epoch remain frozen until the next packet after resume. If
that packet identifies a changed broker, it clears the frozen history. Resume
does not replay packets ignored during pause. Window controls still trim and
redraw immediately while paused. Expanding cannot restore evicted history.

## Legacy compatibility and limits

Packets without a valid stream ID remain usable; no restart is inferred from
missing/invalid identity or backwards times. Samples without usable broker IDs
use best-effort receipt/session/sequence/sender-time/value identity. This keeps
coincident samples with distinct sender times or values, but completely
identical legacy events cannot be proven distinct. Legacy packets cannot prove
a receiver reboot; new-clock samples can remain outside the old rolling axis.
Only the additive broker contract resolves that ambiguity.

## Tests

Opt in to the actual inline-script/handler suite with
`-DRAPID_TEST_TELEMETRY_VIEWER=ON` and provision Node (tested with Node 20).
The option defaults off, so ordinary native-only test builds do not acquire a
mandatory Node dependency. Direct execution:

```sh
node --test cpp/assets/tests/telemetry_history_tests.js
```

Native runtime tests cover instance identity, idle/history/incremental packets,
reconnect stability, event counters and existing bounds/drop accounting. Native
network tests drive authenticated UDP into the real daemon and verify history,
reconnect, idle and live engineering packets over the actual HTTP WebSocket.
State-mode display and remote-write security assertions remain in place.
