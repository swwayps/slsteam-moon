# Diagnostics and thread-affinity switches

Everything here is off or at its default unless you set it. None of it changes
what SLSsteam does, only *where* work runs and whether a trace is written.

## `SLSSTEAM_AFFTRACE` — thread-affinity trace (off by default)

```bash
SLSSTEAM_AFFTRACE=1 <steam wrapper>
```

Accepted true values: `1`, `y`, `yes`, `on`, `true` (case-insensitive).
Anything else, including unset, leaves tracing off. When off, each hook-side
call is a single relaxed atomic load and an early return, so the Steam IPC hot
path is untouched in normal use and no file is created.

When on, records go to `~/.SLSsteam.afftrace.log`, created `0600` and truncated
at start. The file is hard-capped: `4096` event lines plus `48` per-hour summary
lines. There is no rotation — once the cap is hit, writing stops.

### What a line contains

```
seq=… t_mono_us=… kind=… src=… call=… mode=… tid=… owner_tid=… off_owner=…
ipc_depth=… ipc_frames=… queue_depth=… dur_us=… frames_during=…
```

| Field | Meaning |
|---|---|
| `seq` | monotonic record sequence |
| `t_mono_us` | `CLOCK_MONOTONIC` microseconds |
| `kind` | `IPC_OWNER`, `IPC_ENTER`, `IPC_EXIT`, `WATCH_ENTER`, `WATCH_EXIT`, `STEAMCALL_ENTER`, `STEAMCALL_EXIT`, `STEAMFN_ENTER`, `STEAMFN_EXIT`, `QUEUE_PUSH`, `QUEUE_DRAIN`, `QUEUE_STALE`, `HOUR_ROLLUP` |
| `src` | callback source label: `config`, `api`, `ipc`, `site`, `none` |
| `call` | symbolic call label: `on_modify`, `package0_inject`, `license_reconcile`, `install_app`, `cutlmemory_grow`, `notify_licenses_updated`, `run_ipc_frame`, `drain` |
| `mode` | execution site: `queued`, `direct`, `direct_no_owner`, `direct_overflow`, `direct_disabled`, `direct_stale`, `n/a` |
| `tid` / `owner_tid` | native thread id / latched owner IPC thread id |
| `off_owner` | `1` when the record's thread is not the owner thread |
| `ipc_depth` | owner IPC frames in flight |
| `ipc_frames` | cumulative owner IPC frame count |
| `queue_depth` | pending owner-thread commands |
| `dur_us` | duration, on exit records |
| `frames_during` | owner IPC frames entered during the call (drain / stale records: entries executed) |

Per-hour summary lines carry counts only:

```
seq=… t_mono_us=… kind=HOUR_ROLLUP hour=… watch_cb=… steam_calls=…
off_owner_calls=… overlaps=… ipc_frames=…
```

`overlaps` counts Steam-owned work units that ran with `off_owner=1` **and**
(`ipc_depth >= 1` or `frames_during >= 1`) — an off-owner Steam call while an
owner IPC frame was in flight or started during it. Comparing `overlaps` across
`hour` values answers whether that grows with session length.

### Frame accounting covers every dispatcher

`ipc_depth` / `ipc_frames` are accounted by **all** hooked `RunIPCFrame`
dispatchers (`IClientUtils`, `IClientApps`, `IClientAppManager`,
`IClientRemoteStorage`, `IClientUGC`, `IClientUser`, `IClientUserStats`), not
just `IClientUtils`. An earlier build only guarded `IClientUtils`, so a drain
that happened inside another dispatcher's frame was recorded as `ipc_depth=0`
and the overlap counters under-reported real owner activity.

Counting is gated on the frame actually running on the latched owner thread, so
a dispatcher that runs elsewhere cannot pollute the owner-frame counters — the
numbers still mean "owner-thread frames".

Per-frame IPC records are written only while a watcher-originated Steam-owned
call is in flight, so enabling the trace does not log the client's normal IPC
traffic.

### Privacy

Only the fields above are ever written. No account id, app id, package id,
depot id, payload, file path, token, raw pointer, save name, hostname, or
command argument is emitted, and none is passed to the trace in the first
place. The file is user-only (`0600`) and lives in `$HOME`.

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

| Situation | What happens | Trace `mode` |
|---|---|---|
| Normal | enqueued, executed on the owner's next IPC frame | `queued` |
| Caller already *is* the owner thread | runs inline; that is the target thread | `direct` |
| No owner latched yet (very early boot) | runs inline, as before | `direct_no_owner` |
| Queue at capacity (32 pending) | runs inline, after anything already pending | `direct_overflow` |
| Queue switched off | runs inline, as before | `direct_disabled` |
| Teardown in progress | not run | — |
| Owner never drains | runs inline after the staleness threshold | `direct_stale` |

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
* It is reported as a `warn` in `~/.SLSsteam.log` and as a `QUEUE_STALE` /
  `direct_stale` record in the trace, so it is never silent.
* `SLSSTEAM_OWNER_QUEUE_MAX_STALE_MS=0` turns it off, i.e. explicitly chooses
  "accept eventual execution on the owner thread, whenever that happens".

### `SLSSTEAM_OWNER_QUEUE` — disable the handoff entirely

```bash
SLSSTEAM_OWNER_QUEUE=off <steam wrapper>
```

Accepted false values: `0`, `off`, `no`, `n`, `false`, `disable`, `disabled`
(case-insensitive); the true spellings are also accepted. An unrecognised value
keeps the queue **enabled** and logs a warning — a typo must not silently switch
hardening off.

With the queue off, every watcher-originated Steam call runs on the calling
thread, which is exactly the pre-hardening behaviour.

`SLSSTEAM_OWNER_QUEUE_DEADLINE_MS` is **obsolete**: the handoff no longer
blocks, so there is no deadline to set. If it is still present in a wrapper
script it is ignored and a warning is logged.

### What the log says

`~/.SLSsteam.log` reports the policy once at startup and then which path each
event took, e.g.

```
OwnerWork: watcher-originated Steam calls hand off to the owner IPC thread (non-blocking; stale fallback on)
Config watcher: hot-add detected, package 0 injection + license broadcast dispatched queued-owner-thread
OwnerWork: drained 2 command(s) on the owner IPC thread
```

## Teardown and the pre-hook cleanup path

`Hooks::remove()` is not only the teardown path. `main.cpp`'s `load()` runs once
per audited module open, and its "the other module isn't mapped yet" retry goes
`load()` → `unload()` → `Hooks::remove()` **before anything is hooked** —
`LM_FindModule("steamui.so")` fails on the first `steamclient.so` `la_objopen`.

Closing the work queue from there would abandon every watcher-originated Steam
call for the whole session: no package-0 injection and no license broadcast for
as long as the client runs. So the queue is closed only when a hook-placement
pass was actually entered, and that decision is a single gate
(`OwnerQueue::PlacementGate`) with a regression test pinning both directions:
a pre-hook `Hooks::remove()` leaves the queue accepting work, a real teardown
closes it, and repeats of either are no-ops.

## Scope

This is defensive hardening plus diagnostics. A controlled VM run confirmed the
off-owner execution site exists and can be moved, but it did not reproduce any
client failure and did not observe the hypothesised overlap with an in-flight
owner frame. Nothing here is a confirmed fix for any client-lifecycle problem;
the trace exists so that question can be answered from a real session instead of
a bespoke build.
