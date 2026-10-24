---
status: proposed
date: 2026-10-25
---

# Render chunks with a Highway SIMD kernel, checked against the scalar reference

## Context and Problem Statement

The render kernel does one sine, one exponential and a few additions per
partial per sample (`rendering-engine/docs/rendering.md` §7, §8.4). It has
been serial and scalar, built as the reference the optimised version would be
checked against. Threads across partial sets
([ADR 0026](0026-render-a-frames-partial-sets-on-worker-threads-bit-identical-to-serial.md))
now use every core, so the next factor has to come from inside each core.

AGENTS.md has long settled the direction: Google Highway, lanes across
partials, Structure-of-Arrays. Still open were how to dispatch across
machines, what counts as correct output once the sums are reordered and libm
is replaced, which precision the lanes use, and what happens to the scalar
kernel.

## Decision Drivers

* **One binary for many machines.** Graviton (SVE, SVE2, NEON), Apple Silicon
  (NEON) and x86 (AVX2, AVX-512) all have to run well. Vector widths differ
  from 128 to 512 bits, and up to 2048 for SVE.
* **Accuracy and reference signals.** The author values exact, checkable
  output, and keeps the ability to render a reference signal.
* **Floating-point addition is not associative,** and a polynomial sine is not
  libm's sine. A vector kernel cannot be bit-identical to the scalar one
  without giving up most of what makes it fast.
* **Double is the engine's rule** for everything it computes. On 128-bit
  NEON that means two lanes per vector.
* **The kernel boundary was built for this** (§7.5). The change should replace
  the kernel's body and nothing above `renderPartialSet()`.

## Considered Options

For correctness:

1. **Within a bound of the scalar reference,** with the bits free to differ
   between vector widths and machines.
2. **Within a bound, and bit-identical on every machine,** by fixing the
   summation order and the FMA use at a width independent of the hardware.
3. **Bit-identical to the scalar kernel,** by changing the reference to use
   the same polynomials and lane order.

For precision: **double throughout**, or **float for the sine and the
exponential** with the phase accumulator kept in double.

For dispatch: **Highway's dynamic dispatch** (every target compiled in, the
best chosen at the first call), or **one static target per build preset**.

## Decision Outcome

Chosen: **a Highway kernel with dynamic dispatch, in double, whose output
agrees with the scalar reference within a stated bound, with the scalar
kernel kept and selectable** (author, 2026-10-24).

* **Layout.** Lanes across partials. Every chunk is re-anchored from the
  closed form (§7.2), so nothing but the capped-step count survives a chunk
  boundary. Each chunk's active partials are therefore anchored into the
  block's first lanes, in plan order, and every load is contiguous. Blocks
  are padded to 32 lanes (2048-bit SVE), and the kernel silences the padding
  lanes, so there is no tail loop.
* **Loop.** The samples are taken in tiles. For each vector of partials, its
  state stays in registers across the tile, and its contribution to each
  sample is added into that sample's vector accumulator. Each accumulator is
  reduced horizontally once, after the block's last vector. Two conditions
  are decided once per vector per tile, not per sample: whether any step can
  exceed π (the cap and the reduction by whole cycles are needed only then),
  and whether every lane stays in the audio band (the roll-off is then
  exactly 0 dB).
* **Correctness.** The bound is one rounding to `SampleValue` at the
  sample's magnitude, plus 10⁻¹² per partial summed (§10). The capped-step
  counts are exactly the reference's. For given inputs on a given machine
  the output is always the same, so ADR 0026's invariant holds for each
  kernel: the thread count never changes the output.
* **Precision.** Double throughout. Float transcendentals would double the
  lanes on 128-bit targets, but they are an approximation, and are left as a
  separate, measured experiment.
* **The reference.** `--render-kernel scalar` renders with the scalar
  kernel, in batch and server mode; `simd` is the default. The scalar kernel
  is unchanged and is the oracle the SIMD kernel's tests compare against.

Option 2 was not chosen: shards on different instance types would produce
different last bits, which the playback server's sum tolerates, and pinning
a virtual width would cost the wide targets their width. Option 3 would give
up libm as the reference.

### Consequences

* Good, because the change is confined to the kernel and to its one caller,
  `renderPartialSet()`. Gather, planning, threading and the spatial stage
  are unchanged.
* Good, because a reference signal can still be rendered from any score, and
  the two kernels can be compared on any score at any time.
* Good, because in practice the files are usually byte-identical anyway.
  Highway's sine and exponential differ from libm's by about 10⁻¹⁶, far below
  one float step, so narrowing to `SampleValue` almost always hides the
  difference. Every example score renders the same bytes with both kernels
  on Apple Silicon.
* Bad, because the guarantee is a bound, not bit identity. A file rendered on
  one machine may differ in its last bits from the same file rendered on a
  machine with another vector width.
* Bad, because the engine takes on a dependency, Highway, compiled from
  pinned source.
* Neutral, because on 128-bit NEON double has only two lanes. Measured on
  Apple Silicon (`macos-max`, 2026-10-25), the kernel alone is 3.1× the
  scalar kernel. On `convergence` (800 partials in 10 sets) a frame on one
  thread drops from 1.27 s to 0.65 s. On ten threads it drops from 0.33 s to
  0.30 s, where the serial gather and the summing now dominate.

Impact on components:

* `rendering-engine` — renders with the SIMD kernel by default;
  `--render-kernel scalar` selects the reference, in batch and server mode.
  The report's rendering line names the kernel. Highway is a private
  dependency: no Highway type crosses a public header.
* `engine-db` — none.
* `playback-client` — none. Frames arrive sooner.
* `playback-server` (not built yet) — may receive objects from shards on
  different hardware whose last bits differ within the bound. Its sum must
  not assume bit identity across shards.

### Confirmation

* Kernel tests run every behavioural case (the worked example, chirp, fade,
  re-anchoring, roll-off, adding into the buffer, large steps, the cap, NaN
  steps) under both kernels. They compare SIMD with the reference within the
  bound on lane counts around every vector width, from 1 to 513. They also
  require identical capped counts and phases within 10⁻⁹, and require that
  some phases are not bit-identical, as evidence that the SIMD path ran.
* Rendering tests render the two-set score and the spatial score in FOA
  B-format with both kernels and compare every sample within the bound. The
  thread-count tests of ADR 0026 run with the SIMD kernel.
* Every oracle score runs under both kernels (`oracle.<score>.scalar`,
  `oracle.<score>.simd`).

## More Information

* `rendering-engine/src/render_kernel_simd.cpp`, `render_kernel.hpp`, and
  `rendering-engine/docs/rendering.md` §7.5, §8.4 and §10.
* AGENTS.md, "Rendering Architecture" and "C++ Conventions".
* Revisit for float transcendentals, or §8.4's geometric recurrence for the
  amplitude, which replaces the exponential with one multiply. Profile first:
  on Apple Silicon the sine and the exponential are now most of the kernel.
