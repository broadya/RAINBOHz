# playback-client — design

**Status:** first version, implemented. A CLI that plays a range of the score
loaded in a REST server, in a loop, through the default audio device, and
renders every loop afresh. It is kept as a demonstration and as a test
component; more advanced playback components will come later. The decisions
recorded here were taken by the author on 2026-10-03.

## 1. Where it sits

`architecture/overview.md` §5.3 describes `playback-client` as a client of
`playback-server`. That server does not exist yet. In the target architecture
it:

- sums the audio of many `rendering-engine` instances, one per partial set
  shard, in distributed mode;
- has a REST interface very close to `rendering-engine`'s;
- offers what an API gateway offers, such as rate limiting;
- serves the audio itself, or URLs to audio that object storage then serves.

That is work in progress. Until it exists, this client talks to
`rendering-engine`'s REST service directly
([`rendering-engine/api/openapi.yaml`](../../rendering-engine/api/openapi.yaml)),
standing in for `playback-server`. Three things in the design follow from that:

- **The REST layer is named for the API, not the server.** `PlaybackApi` is an
  interface; its one implementation speaks HTTP. Moving to `playback-server`
  is meant to change `--server`, or at most that one class, and nothing in the
  lane logic.
- **A render returns an object's URI, never audio.** In the target, URLs, not
  audio payloads, travel between components, and audio lives in object
  storage. Today the URI is a `file://` one on the server's machine, so the
  client fetches the object by copying it, and must run where it can read the
  server's `--out-dir`. The fetch is behind `ObjectFetcher`, so an `s3://`
  fetcher, against an S3 simulator say, slots in beside it (§7).
- **Errors are problem documents**, quoted in full. A 409 adds the hint that a
  score must be loaded; a 429, which a gateway may give, is reported as rate
  limiting. This version does not retry anything.

The client does not load a score. One must be loaded already
(`PUT /v1/score`); a 409 says so.

## 2. Command line

```
playback-client --start B:T --end B:T
                [--server http://127.0.0.1:8080/v1] [--loops 1]
                [--buffer-dir <dir>] [--keep-buffer]
                [--output-wav <file>] [--debug]
```

| Option | Meaning |
| --- | --- |
| `--server` | The API's base URL, `/v1` included, as the OpenAPI document's server URL. `http` only. |
| `--start`, `--end` | The range `[start, end)` on the score's root tempo map, as `beats:ticks` or bare beats, the server's own grammar. The end must be after the start. |
| `--loops` | How many times to play the range; at least 1, default 1. Each loop is a fresh fill. |
| `--buffer-dir` | Where the run's buffer directory is made. Defaults to the system temporary directory. |
| `--keep-buffer` | Keep the run's buffer directory at exit. By default it is removed. |
| `--output-wav` | Write what would be played to this file instead of the device (§5). |
| `--debug` | Log every REST call, frame and wait (§6). |

Exit status: 0 when every loop was played; 1 on any failure, or when
interrupted; 2 for a usage error.

## 3. Startup: fixing the range

Before any audio, the main thread makes four calls, in order:

1. `GET /engine-config` gives the sample rate and frame size. The channel mode
   must be `mono`; rendering implements nothing else yet.
2. `GET /tempo-maps/root` gives the root map's id and its `ticks_per_beat`.
   Each position's ticks must be below that, which only the server can tell.
3. `GET /tempo-maps/{id}/frame-position?at=` for the start, then for the end.
   Each answer is the sample at or before the position, as frame and offset.

The **frame plan** follows from those two answers alone (`planFrames()`): frames
from the start's frame to the end's. The first is played from the start's
offset, the last up to the end's offset, and every frame between them whole.
An end offset of 0 means the range stops on a frame boundary and needs nothing
from that frame. A range that converts to no samples at all is refused.

## 4. Lanes

The buffer directory holds two lanes, A and B. Each holds one complete pass
over the range as frame files. The network thread fills them in turn, one fill
per loop: fill *k* goes to lane A when *k* is even, B when odd. The playback
thread (the feeder) plays loop *j* from the same lane. So while one lane
plays, the other is being refilled.

The lanes' state is shared through `LaneBoard`, under one mutex and one
condition variable. Every transition is one step under the lock:

```
          beginFill            last frameWritten            switchTo(other)
  Free ──────────────▶ Filling ─────────────────▶ Ready ──────────────────▶ Free
   ▲      (network)            (same step)                (feeder leaves it)
   └──────────────────────────────────────────────────────────────────────┘
```

**The protocol.**

- The network thread writes only a **Free** lane. `beginFill()` waits for one.
- Each frame is fetched to `frame-NNNNNN.wav.part`, renamed into place, and
  only then published (`frameWritten()`). Publishing the last frame of a lane
  marks it **Ready**, in the same step.
- **The first loop streams.** The feeder reads frame *i* of lane A as soon as
  it is published (`waitForFrame()`). Playback starts with the first frame,
  not the whole lane.
- **Every later loop is all-or-nothing.** The feeder switches to the next lane
  only when it is Ready (`switchTo()`). The switch makes it the playing lane
  and frees the one just played, in the same step. That is what lets the
  network thread start refilling it.
- **If the next lane is not ready, playback blocks.** The device plays silence
  until it is, and the gap is logged (§6). It does not replay the old lane.
- Stopping, by error or interrupt, wakes every wait. From then on every wait
  returns false.

**The races, and what closes each.**

| # | Race | Closed by |
| --- | --- | --- |
| 1 | The network thread overwrites a lane the feeder is reading. | A lane is written only when Free. It is freed only when the feeder switches away from it, and the feeder switches only after reading its every file. A lane being read is never Free. |
| 2 | The feeder reads a half-written frame. | Write to `.part`, rename, then publish. The feeder reads only published frames. |
| 3 | The network thread publishes a lane's last frame and marks it Ready as two steps. A fast feeder reads the frame, switches away and frees the lane in between. The late Ready then makes a Free lane look full: the network thread waits forever for it, and the feeder later replays its stale audio. | The last `frameWritten()` marks the lane Ready in the same locked step. There is no separate "end fill" call. |
| 4 | The audio callback blocks on a lock or I/O and glitches. | All waiting is in the feeder. The callback only pops from a lock-free ring (§5). |
| 5 | Playback reaches the end of a lane before the next is Ready (an underrun). | The feeder blocks and the device plays silence, as decided. The gap is measured and logged. |
| 6 | The network thread renders more fills than will be played. | It makes exactly `--loops` fills, then ends. |
| 7 | Shutdown while a REST call is in flight. | Stopping is checked between calls, and the call in flight is cancelled (`PlaybackApi::cancel()`, which shuts its socket down), so a stalled server does not hold the shutdown for the read timeout. The main thread cancels again every 50 ms until the network thread ends, because a cancel cannot reach a call whose connection is not yet open. A failure that follows the stop is not reported as the session's error. Nothing is cancelled mid-write: a frame is published only after its `.part` file is renamed. |
| 8 | An error on one thread leaves the other waiting forever. | `fail()` records the first error and stops everything, which wakes every wait. |
| 9 | Ctrl-C arrives mid-session. | The SIGINT/SIGTERM handler sets a lock-free atomic and puts back the signal's default action. The main thread polls the atomic every 50 ms and stops the session in order, cleanup included. A second Ctrl-C ends the client at once, without cleanup. |
| 10 | The server overwrites `frame-NNNNNN.wav` with another render before the client has copied it. | One call at a time: the client copies each frame before its next call. Another client of the same server could still interleave. That risk is accepted for a demonstration client. |
| 11 | The score is reloaded mid-session, so the tempo-map id and the frame range go stale. | Not detected as such. The next call that fails stops the session. Versioned scores are server-side later work. |
| 12 | Two clients share a buffer directory, or a run reads an earlier run's files. | Each run makes its own `rainbohz-playback-XXXXXX` directory. Cleanup removes only that. |
| 13 | The audio device is lost mid-session (unplugged, say), so the callback stops draining the ring and the feeder waits for room for ever. | miniaudio's notification that the device stopped, when the client did not stop it, makes the next `write()` or `finish()` fail, which stops the session with an error. |
| 14 | Starting the second thread fails, or anything else throws between starting the threads and joining them, and a joinable `std::thread` ends the process. | A guard stops the session, aborts the sink, cancels the API and joins whatever is still running. |

`test_lane_board.cpp` checks each transition on its own. `test_session.cpp`
runs whole sessions over a fake API whose every sample is *fill × 100 + frame*.
The output therefore records which fill of which frame each loop played, and
a lane overwritten mid-read (race 1) or a loop from the wrong fill would show
up as a wrong value. Those tests also run under ThreadSanitizer, cleanly.

## 5. Audio output

Two sinks implement `AudioSink`.

**The device** (`DeviceSink`, miniaudio, @see ADR 0018)
is the system's default output device. Between it and the feeder is a
single-producer, single-consumer lock-free ring, about half a second long.

- The feeder writes into the ring. When the ring is full it polls, sleeping
  2 ms at a time.
- miniaudio's real-time callback pops from the ring, copies the mono signal to
  every device channel, and writes zeros when the ring runs dry. It takes no
  lock, does no I/O, does not allocate and does not log.
- The callback counts the silent samples it plays between the first write and
  the start of the play-out at the end. That count is the underrun.
- A device that does not run at the score's rate is resampled by miniaudio,
  and the client logs that it is.

**A WAV file** (`--output-wav`) takes samples as fast as they come and leaves
out the gaps, so its content is exactly the range, trimmed, `--loops` times
over. That is what the end-to-end test checks it against.

After its last loop, the feeder plays the output out. The device sink waits for
the ring to empty, then for the device's own buffer, then stops. The main
thread stays free to notice Ctrl-C meanwhile.

## 6. Timing and the log

Everything goes to standard error, one line per message:

```
14:24:36.032 [info ] [main] range 1:240 to 6:480: frame 0 +32620 to frame 5 +13084, 6 frame(s), 220464 samples (4.593 s)
14:24:36.117 [info ] [playback] first frame ready after 0.085 s; playing
14:24:36.436 [info ] [network] lane a filled (fill 1 of 3): 6 frame(s) in 0.404 s; render mean 66.8 ms, max 83.9 ms; fetch total 2.9 ms; real-time factor 0.088 (4.593 s of audio)
```

(From the end-to-end test, a debug build, the tempo-map example score.)

The thread is `main`, `network` or `playback`. `info`, `warn` and `error` lines
always appear. `debug` lines appear only with `--debug`: each REST call with its
status and time, each frame's render and fetch time, and each wait.

The **real-time factor** of a fill is its wall-clock time divided by the
duration of the audio it holds. Below 1, rendering keeps up with playback, and
in steady state the next lane is ready before it is needed. Above 1, gaps are
certain. A gap is reported as a warning with its length in samples and
milliseconds, from the device's underrun count. The feeder's own waits, which
the ring may hide, are debug lines.

## 7. Files and objects

```
<buffer-dir>/rainbohz-playback-XXXXXX/
    lane-a/frame-000003.wav
    lane-b/frame-000003.wav
```

The names follow rendering-engine's own frame files. A frame read from a lane
must be mono, at the score's sample rate, and exactly one frame long. Anything
else means the buffer does not hold what the session asked for, and is an
error.

`ObjectFetcher` copies an object named by URI to a local file. This version
has one fetcher, for `file://` URIs as rendering-engine writes them, and
refuses every other scheme by name. A fetcher for object storage is added
behind the same interface.

## 8. Tests

| Test | What it covers |
| --- | --- |
| `test_units_and_parsing.cpp` | Positions, the frame plan (trims, a boundary end, one frame, empty and reversed ranges), the ring, the command line |
| `test_files.cpp` | `file://` decoding and fetching, the buffer directory, WAV reading and writing |
| `test_lane_board.cpp` | Each transition of §4, and that stopping wakes every wait |
| `test_session.cpp` | Whole sessions over a fake API: one loop, many loops, fast network with slow playback, slow network, streaming of the first loop, errors on either thread, an interrupt mid-call, and a stalled call cancelled on an interrupt and on a playback error |
| `test_playback_api.cpp` | The HTTP API's `cancel()` against a real, stalling HTTP server: a stalled call ends when cancelled, and a call after a cancel fails at once |
| `playback.e2e` | The real binary against a real `rendering-engine` server: no score loaded (exit 1 and the hint), a reversed range (exit 2), three loops to `--output-wav` checked sample for sample against the server's frames, the buffer removed, and `--keep-buffer` keeping both lanes |

`make test-playback-client` runs them all. The end-to-end test needs
`rendering-engine` built for the same preset, and fails otherwise; the make
target builds it first.

## 9. Not yet

- **No `playback-server`.** It is the target of `--server`, when it exists
  (§1).
- **Local files only.** Object storage, and an S3 simulator to stand in for
  it, come later.
- **Mono only**, as rendering. Channel modes beyond mono are refused.
- **The default device only.** No device choice, latency setting or
  multi-channel layout.
- **No retries**: not on 429, not on a failed call.
- **No crossfade at the loop point.** The loop is sample-exact, so a range
  that does not loop cleanly clicks.
- **Labels are not passed.** Rendering ignores them in this version.
