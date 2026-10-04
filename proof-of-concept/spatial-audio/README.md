# spatial-audio PoC: perfect virtual microphones

Python oracle for RAINBOHz multi-channel and spatial audio, round 1. A C++20 PoC
follows, then integration into `rendering-engine`. Not part of any build.

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
