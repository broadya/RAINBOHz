---
status: proposed
date: 2026-10-24
---

# Render a frame's partial sets on worker threads, bit-identical to the serial engine

## Context and Problem Statement

Rendering runs on one core. On `echoes` (5,120 partials, eight 1 s frames,
`macos-max`, 2026-10-21) the kernel and WAV writing take 9.0 s of a 10.6 s
render, and once the database work is reduced they are most of what a
reload waits for. The CPU above 100% that `top` shows during a run comes
from Ladybug's worker threads during precomputation, not from rendering.

AGENTS.md has always meant rendering to be parallel ("Write for the parallel
version, implement the serial one"), with partial sets as the unit of
parallelism between threads, and `rendering-engine/docs/rendering.md` §7.5
lays out the structure. The serial version was arranged for it: only the
gather reads `engine-db`, preparation is pure functions over in-memory data,
and a partial set's rendering reads no other set's data.

This is the first time the engine creates threads of its own. How should
work be divided, how should threads be managed, and what must stay true of
the output?

## Decision Drivers

* **The server renders one frame per call** (`POST /v1/frames/{frame}/renderings`,
  [ADR 0016](0016-serve-rest-with-cpp-httplib-one-call-at-a-time.md)). A live
  reload waits for single frames, so the parallelism has to be inside a
  frame.
* **Accuracy and reproducibility.** The author values exact, checkable output
  over silent approximation. A rendering that depends on the machine's core
  count, or on timing, cannot be compared with a reference.
* **Floating-point addition is not associative.** Summing the same buffers
  in a different order changes the last bits of the result.
* **Threading bugs hide.** Data races and ordering faults depend on timing,
  so tests that pass are weak evidence. The design should rule them out by
  construction where it can, and be checked by a tool where it cannot.
* **Partial sets vary in size,** from a few partials to thousands.

## Considered Options

For the unit of work:

1. **Partial sets within a frame,** each rendered into its own buffer, the
   buffers combined in the existing order.
2. **Frames,** different frames on different threads.
3. **Chunks of partials** within a set.

For managing threads:

1. **Fork-join per call:** threads created for one parallel step and joined
   before it returns.
2. **A persistent pool** of idle workers, woken per step.

## Decision Outcome

Chosen: **partial sets within a frame, on threads created and joined per
step, with the output bit-identical to the serial engine for every thread
count.**

* **Unit.** Each partial set of a frame is planned and rendered into its own
  mono buffer on a worker thread. Every step that combines buffers (summing
  a source's sets, spatialising each source into the frame) runs afterwards
  on the calling thread, in the serial engine's order: sources by id, sets by
  id. So does every side effect: file writes, the frame's report, logging.
  Sets are processed in bounded waves, so memory does not grow with the
  number of sets.
* **Invariant.** The thread count never changes the output. Every file and
  every report field is byte-for-byte what the serial engine produces. A
  later change that would break this, such as splitting a set into chunks,
  needs a decision of its own.
* **Threads.** One fork-join primitive is the only code that creates
  threads. It hands out indices from an atomic counter, the calling thread
  works too, it joins every thread before returning (`std::jthread`, never
  detached), and it rethrows the failure with the lowest index. It holds no
  locks and no state between calls, and cannot be nested.
* **Rules for code that runs on worker threads.** It never touches
  `engine-db`, files, the report or a stream. It reads only data that
  nothing writes during the step, and writes only to what belongs to its
  own index.
* **Thread count** is a process option, `--render-threads`, with 0 (the
  default) meaning one per hardware thread. Because the output does not
  depend on it, it changes only speed.

Chunks of partials were not chosen: splitting a set changes the summation
order, so the last bits of the output. That would be needed only when one set
dominates a frame. The author decided (2026-10-24) not to do it yet.

Frames were not chosen: a batch run renders several frames, but the server
renders one per call, so frame-level threads would not speed up the path a
practitioner waits on.

A persistent pool was not chosen: it saves thread creation, tens of
microseconds per thread against frames of 0.1–1.5 s, and costs a work queue,
condition variables, shutdown ordering and an owner that outlives calls.
The fork-join primitive does not expose its threads, so a pool can replace
its inside if timing shows thread creation matters.

### Consequences

* Good, because one oracle checks the whole change, and keeps checking it:
  render with one thread and with several, and require identical bytes.
* Good, because the threading is confined to one primitive and one
  function's parallel step, and deadlock, detached threads and dangling
  references are impossible by construction.
* Good, because scaling needs no change to the gather, the preparation
  layer or the kernel. SIMD (rendering.md §8.4) multiplies on top.
* Bad, because a frame goes only as wide as its number of partial sets, and
  only as evenly as their sizes. A frame dominated by one set gains little.
* Bad, because the engine now creates threads, so a ThreadSanitizer build
  joins the checks a change to rendering must pass.
* Neutral, because threads are created per frame. The cost is measured; if
  it shows, the primitive's inside changes to a pool and nothing else does.
* Neutral, because Ladybug keeps its own thread pool. The two do not compete
  for cores, because rendering's threads run only after the gather has
  returned.

Impact on components:

* `rendering-engine` — rendering of a frame runs on `--render-threads`
  threads, in both batch and server mode. The server still handles one call
  at a time (ADR 0016); within a call, a frame's partial sets run in
  parallel. Gathering, validation and every write stay on the calling
  thread.
* `engine-db` — none. It is read only by the gather, before any worker
  starts.
* `playback-client` — none. Frames arrive sooner.
* `playback-server` (not built yet) — talks to engines that use every core
  for one call at a time. Parallelism across many engines, one per shard,
  is unchanged.

### Confirmation

* Rendering tests render the same requests with 1, 2, 3 and 8 threads and
  require byte-identical files and equal reports. The cases cover many sets
  across several waves, per-set files, a frame that fails, a silent frame
  and capped phases.
* A `make` check renders every example score with 1 and 8 threads and
  compares the frame files byte for byte. It is part of `make test`.
* The fork-join primitive's tests check that every index runs exactly once,
  that the lowest failure is rethrown, and that nesting is refused.
* The `macos-tsan` preset (ThreadSanitizer) runs the rendering-engine tests,
  and passes before a change to threaded code is trusted.

## More Information

* `rendering-engine/docs/rendering.md` §7.5, which this ADR implements, and
  §8.4, the SIMD work that follows.
* AGENTS.md, "C++ Conventions", "Write for the parallel version, implement
  the serial one".
* The timings are from the 2026-10-21 profile of `echoes` on `macos-max`.
