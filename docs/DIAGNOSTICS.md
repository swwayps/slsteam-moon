# Owner-thread handoff and its switches

Everything here is at its default unless you set it. None of it changes what
SLSsteam does, only *where* work runs.

## Owner-thread handoff for watcher-originated Steam calls

The config watcher and the API watcher are plain inotify threads, but the work
they trigger — appending to package 0 through the resolved `CUtlMemoryGrow`,
broadcasting `NotifyLicensesUpdated`, and the app-manager install call — is
Steam-owned. Those calls are handed to the thread Steam dispatches IPC frames
on, instead of running on the watcher thread. Same calls, same inputs, same
order; only the thread changes.

**The handoff does not block.** The watcher enqueues and returns (microseconds);
the owner thread drains the queue on its next IPC frame. This is deliberate,
and it replaced a bounded blocking wait after guest measurements:

* owner-thread wake cadence on an idle client was **~3.30 s** and **~3.41 s**,
  measured twice independently;
* adding five extra drain points (one per hooked dispatcher) did **not** shorten
  it (3.30 s → 3.41 s), so drain density is not the limiting factor — an idle
  client simply does not run IPC frames more often;
* so with the old 1500 ms deadline the wait *always* expired: the watcher paid
  1.5 s of latency and then ran the work off-owner anyway.

A hot-add is already asynchronous from the user's point of view (a `.lua` lands
in `stplug-in`, the library updates a moment later), so a few seconds of handoff
latency costs nothing, while the watcher callback goes back to milliseconds.

### Fallbacks

| Situation | What happens | Log label |
|---|---|---|
| Normal | enqueued, executed on the owner's next IPC frame | `queued-owner-thread` |
| Caller already *is* the owner thread | runs inline; that is the target thread | `owner-thread-inline` |
| No owner latched yet (very early boot) | runs inline, as before | `direct-no-owner` |
| Queue at capacity (32 pending) | runs inline, after anything already pending | `direct-queue-full` |
| Teardown in progress | not run | `not-run-shutdown` |
| Owner never drains | runs inline after the staleness threshold | warning (below) |

Every fallback runs whatever is already pending **before** the batch it was
given, inside one exclusive execution window, so the total order of Steam-owned
calls never inverts, and a single execution mutex guarantees two threads never
run this subsystem's Steam-owned work at the same time.

### `SLSSTEAM_OWNER_QUEUE_MAX_STALE_MS` — the "owner never drains" answer

Default `30000`, accepted range `0`–`600000`. Invalid values are ignored (with a
warning in `~/.SLSsteam.log`) and the default is used.

Fire-and-forget means nobody is waiting, so there has to be an answer to *what
if the owner never runs another IPC frame* (client wedged, dispatcher thread
replaced, IPC stopped). Two honest options were available: accept eventual
execution, or keep a bounded escape hatch. This build keeps the escape hatch,
because "the game silently never appears and nothing is logged" is a worse
failure than "the call ran on the watcher thread, exactly as it always used to".

* The threshold defaults to 30 s — about **9×** the measured 3.3–3.4 s cadence —
  so a healthy client never reaches it.
* It is evaluated on the watcher thread's existing idle tick (the inotify wait
  is a bounded `poll`, 500 ms, so there is no extra thread) and once per submit.
* It runs the **whole** pending set inline, in order.
* It is reported as a `warn` in `~/.SLSsteam.log`, so it is never silent.
* `SLSSTEAM_OWNER_QUEUE_MAX_STALE_MS=0` turns it off, i.e. explicitly chooses
  "accept eventual execution on the owner thread, whenever that happens".

### Removed settings

`SLSSTEAM_OWNER_QUEUE` (the off switch) and `SLSSTEAM_OWNER_QUEUE_DEADLINE_MS`
are no longer supported. Running this work on the watcher thread could
deadlock the owner IPC thread, and the handoff no longer blocks, so there is no
deadline to set. If either is still present in a wrapper script it is ignored
and a warning is logged. `SLSSTEAM_AFFTRACE` was removed with the
thread-affinity trace and has no effect.

### What the log says

`~/.SLSsteam.log` reports the policy once at startup and then which path each
event took, e.g.

```
OwnerWork: watcher-originated Steam calls hand off to the owner IPC thread (non-blocking; stale fallback on)
HotReload: generation 3 dispatched queued-owner-thread (managed=12 added=1 removed=0 planner_apps=14 depots=31 metadata=complete)
OwnerWork: drained 1 command(s) on the owner IPC thread
```

## Teardown before any hook is placed

`load()` runs once per audited module open. On the first `steamclient.so`
`la_objopen` `steamui.so` is not mapped yet, so `load()` just returns and the
next open retries; nothing is torn down there. A teardown that does happen
before any hook is placed (for example a failed `Hooks::setup()`) leaves the
work queue open: closing it would abandon every watcher-originated Steam call
for the rest of the session. The queue closes only once a hook-placement pass
was entered (`OwnerQueue::PlacementGate`, covered by `test-ownerqueue`).

## Scope

This is defensive hardening. A controlled VM run confirmed the off-owner
execution site exists and can be moved, but it did not reproduce any client
failure and did not observe the hypothesised overlap with an in-flight owner
frame. Nothing here is a confirmed fix for any client-lifecycle problem.
