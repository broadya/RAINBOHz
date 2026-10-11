# spatial-audio PoC: perfect virtual microphones

Python oracle for RAINBOHz multi-channel and spatial audio, round 1, and its
C++20 port in [`cpp/`](cpp/) (see [C++20 module](#c20-module)). The port is
integrated into `rendering-engine` (2026-10-04, `channel_mode` and
`spatial_gains`, rendering-engine/docs/rendering.md §8.5); this directory
remains the oracle, and is not part of any build.

## Model

- **Positions** use the AmbiX spherical convention. Azimuth is anticlockwise
  from the front (+90° = left). Elevation is up from the horizontal. Distance
  is in metres, at least 1. Axes: x front, y left, z up.
- **The receiver** is at the origin and never moves.
- **Distance gain** is 1/r: 0 dB at 1 m, −6.02 dB per doubling. There is no
  propagation delay and no Doppler yet, and sources are static.
- **Microphones** are *perfect* and *coincident*: gain depends on direction
  only, with no frequency dependence, and every microphone sees the same
  direction. Distance never changes the angle.
- **Gains are signed.** Negative means polarity reversed.
- **Patterns** are first order: a + (1 − a)·cos θ, with omni a = 1, cardioid
  0.5 and figure-8 0. Higher orders (up to 3rd-order Ambisonics) would be more
  implementations of the `Pattern` interface.
- **An array** is a list of microphones plus an optional linear decode matrix.

## Presets

| Preset | Raw mics | Outputs |
| --- | --- | --- |
| `mono` | omni | M |
| `ms_theoretical_raw` / `ms_theoretical` | omni M, figure-8 S facing left | M, S / L = (M+S)/2, R = (M−S)/2 |
| `xy` | cardioids at ±45° (angle and pattern are parameters) | L, R |
| `blumlein` | figure-8s at ±45° | L, R |
| `blumlein_rotated_raw` / `blumlein_rotated` | figure-8s at 0° and 90° | M, S / L, R = (M±S)/√2 (identical to `blumlein`) |
| `foa_a_tetra` | 4 cardioids, FLU FRD BLD BRU | A-format |
| `foa_b_from_a` | the same | AmbiX B-format W Y Z X (ACN, SN3D) |
| `foa_b_direct` | omni + figure-8s on y, z, x | AmbiX B-format, encoded directly |

### Theoretical M/S

- **M is omni.** It measures pressure only.
- **S is a figure-8 facing left.** S = cos(el)·sin(az), so it is 0 at the
  front, the back and the poles. Its equal-gain surfaces are cones about the y
  axis.
- **Decoded to stereo:**
  - Hard left (+90°) is 0 dB in L only.
  - Front and back are centred at −6 dB per channel.
  - A source moving overhead slides smoothly to the centre.
  - Nothing jumps anywhere, so there are no clicks.
- **The pan law is constant amplitude** (L + R = M). Other laws are future work.

### X/Y

- **Two perfect cardioids** (0.5 + 0.5·cos θ) at ±45°. Real cardioids narrow
  at high frequencies; these don't.
- A centre source is −1.38 dB in each channel. At ±45° a source is 0 dB in one
  channel and −6 dB in the other, so the image never goes hard left or right.
- **Cardioids never reverse polarity.** Each has its null 180° off its axis,
  at ∓135°.
- Overhead sources are centred at −6 dB each.
- `xy(half_angle_deg, a)` changes the angle and the pattern. Super- and
  hypercardioids (a < 0.5) add a reversed rear lobe.

### Blumlein

- The stereo stage spans ±45°. A source at ±90° comes out in antiphase.
- A rear source at 180° − φ sounds like the front source at φ, with L and R
  swapped and polarity inverted.
- A source overhead is silent.

## Use

```sh
python3.12 -m venv .venv && .venv/bin/pip install -e '.[dev]'
.venv/bin/pytest
.venv/bin/spatial-poc-report            # all presets: tables to stdout; PNG + WAV + .txt into out/
.venv/bin/spatial-poc-report ms_theoretical blumlein --no-wavs
```

The WAVs are 48 kHz float32. A 1 s, 440 Hz burst steps through the report
positions, and each `.txt` sidecar lists the channel order and positions. The
FOA B-format WAVs are AmbiX and can be checked in any AmbiX decoder.

## C++20 module

`cpp/` is the core calculation in C++20, now moved into `rendering-engine`,
where azimuth and elevation have distinct unit types (`Azimuth`,
`Elevation`) in place of the shared `Radians`. If `oracle_gains.csv` is
regenerated, copy it to `rendering-engine/test/oracle/spatial_gains.csv`
too.
Given a channel mode and a position, it returns the signed linear gain of each
output channel. It has no plotting, WAVs or rendering. It depends on nothing in
`rendering-engine` and has its own unit types (`Radians`, `Metres`,
`LinearGain`). It follows the engine's C++ conventions and takes the
repository's shared C++ configuration: the root `.clang-format`,
`.clang-tidy` and `.clangd`, `cmake/ProjectWarnings.cmake`, and the presets
in `cmake/RainbohzPresets.json`.

Angles are in radians, as in the rendering model. The module does no
wrapping: by the time a position reaches it, any wrapping into the standard
ranges has already been done upstream. `checkPosition()` rejects an azimuth
outside [−π, π], an elevation outside [−π/2, π/2], or a distance outside
[1, 16384] m. NaN is out of every range. The gain functions require a checked
position, are `noexcept` and never allocate.

| Mode (`EngineConfig.channelMode`) | `ChannelMode` | Channels | Oracle preset |
| --- | --- | --- | --- |
| 0 mono | `Mono` | M | `mono` |
| 1 stereo, derived from M/S | `StereoFromMidSide` | L, R | `ms_theoretical` |
| 2 stereo, derived from X/Y | `StereoFromXY` | L, R | `xy()` |
| 3 M/S | `MidSide` | M, S | `ms_theoretical_raw` |
| 4 Blumlein classic | `BlumleinClassic` | L, R (raw figure-8s at ±45°) | `blumlein` |
| 5 Blumlein rotated 45° | `BlumleinRotated` | M, S (raw figure-8s at 0°, 90°) | `blumlein_rotated_raw` |
| 6 FOA A-format | `FoaAFormat` | FLU, FRD, BLD, BRU | `foa_a_tetra` |
| 7 FOA B-format | `FoaBFormat` | W, Y, Z, X (ACN, SN3D) | `foa_b_direct` |

The Blumlein modes are raw microphone outputs. Mode 5 decoded to L/R would be
mode 4. A source overhead is silent in both Blumlein modes but full in M of
mode 3.

API (`include/rainbohz/spatial_poc/`):

- `channelModeFromInt(int8)` converts the database value and throws outside 0–7.
- `channelCount()` and `channelLabels()` describe a mode's outputs.
- `directionGains(mode, azimuth, elevation)` gives pattern × decode only.
- `distanceGain(distance)` gives 1/r.
- `positionGains(mode, position)` combines the two. They are kept separate
  because the engine will later handle distance together with delay and Doppler.

Build and test with the presets (clang, Ninja, GoogleTest fetched at the
engine's pin):

```sh
cd cpp
cmake --workflow --preset macos-debug     # configure, build, ctest; linux-debug on Linux
```

There are two kinds of test. `test_spatial_gains.cpp` ports the Python
property tests. `test_oracle.cpp` checks every row of `test/oracle/oracle_gains.csv`
to within 1e-12. The CSV is committed, so the C++ tests need no Python.
Regenerate it whenever the oracle changes:

```sh
.venv/bin/python cpp/test/oracle/export_oracle.py
```

Run `clang-tidy -p build/macos-debug` over `src/*.cpp test/*.cpp`, adding
`--extra-arg=-isysroot --extra-arg=$(xcrun --show-sdk-path)` on macOS with
Homebrew LLVM; the code is clean. Run `doxygen` in `cpp/` to write
`build/doxygen/html`.
