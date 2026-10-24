# Playground

Musical experiments with RAINBOHz.

> **No stability guarantee.** Nothing here is a component, a supported
> interface, an example of good HzL or test data. The `pg` script, the
> layout and the pieces may change or be removed at any time, without
> notice or migration. Nothing else in the repository depends on it, and
> nothing should.

`.run/` is runtime state and is gitignored. Everything else here is tracked.

## Use

```sh
./pg start [piece]     # compile, start the engine, load, loop (default: beep)
./pg reload            # after editing the HzL: recompile and swap it in
./pg status            # what is running, and the end of each log
./pg stop              # stop playback and the engine
./pg new <name>        # new piece, a copy of beep (--from <piece> for another)
```

`pg` runs from any directory. It needs the repo's `.venv` and the macos-debug
builds (`make venv rendering-engine playback-client` in the repository root).
For another build, set `PG_PRESET=macos-release`. The debug build renders at
about 0.3× real time for `beep`, so it keeps up easily.

**Reload.** If `hzlc` refuses the HzL, the error is printed and the old
version keeps playing. Otherwise the new score is loaded into the running
engine. The client renders one loop ahead, so you hear the change one or
two loops later. If the loop's span changed (new `loop_start`/`loop_end`, or a
tempo change), the channel mode changed, or the client had stopped, `pg`
restarts the client, with a
short gap. If the engine rejects the score, it holds none and the client
stops. Fix the HzL and `pg reload` again.

## A piece

`pieces/<name>/` is an HzL source tree, plus the piece's settings file,
`<name>.yaml` (e.g. `pieces/beep/beep.yaml`):

```yaml
score: scores/beep.yaml   # the score file, relative to the piece
loop_start: "0:0"         # loop [start, end), beats:ticks
loop_end: "4:0"
channel_mode: 1           # optional: hzlc's --channel-mode; default 0, mono
```

The client plays mono and the stereo modes 1 (stereo from M/S), 2 (X/Y)
and 4 (Blumlein).

The HzL language: `../hzl-compiler/docs/hzl-reference.md`.

`beep` is a 440 Hz sawtooth (16 harmonics at 1/n), about 100 ms, on every
beat of one 4/4 bar at 120 BPM. Pitch: `base_frequency` in both
`harmonic_series` files. Tempo: `tempo_maps/main.yaml`. Beep shape: the
`offset_seconds` in `envelopes/beep.yaml`, which count from the beat
(stored as float32, so prefer values like 1/128 s).

`convergence` is 800 partials in 10 sources spread from -60 to +60 degrees,
stereo from M/S. Each harmonic of two sawteeth, 100 Hz on the left and 99 Hz
on the right, is a pair of partials that starts 10% above and 10% below it.
Over 10 s the pairs close in on their harmonics (lowest at the centre,
highest at the edges) and arrive in phase. Then it holds for 10 s. All
partials start at equal level, and the arithmetic of the glide puts them in
phase at the start too, so it opens with a click train that peaks above 0
dBFS. That is deliberate, as an experiment. Its HzL is **generated**: edit
`generate.py` and run it with
`../.venv/bin/python pieces/convergence/generate.py`. Don't edit the YAML.

**It needs the optimised build:** `PG_PRESET=macos-max pg start convergence`.
There it loads in about 4 s and renders at about 0.5 times real time. On the
debug build rendering runs 2.7 times slower than real time, too slow to
loop live. A `pg reload` blocks the engine for the whole load, so playback
may stall briefly.

`four-partials` and `two-partials` are earlier versions reduced to a few
partials, which glide from random frequencies to 100/99 Hz and to 100 Hz in
mono. They are small enough to play on any build.

## Current limits worth knowing

- HzL has no repeat, bars or time signatures: every event is written out
  (`beep.yaml` is 16 stages for 4 beeps). Each envelope can be placed
  only once in a score.
- The engine renders linear curves only. Other curve types are accepted
  but play as linear. The amplitude ramps between envelope nodes are linear
  in dB.
- One tempo map per score. Labels select nothing yet.

## Runtime files

`.run/` holds the compiled scores (`<piece>.tables/`, hzlc's Parquet tables
and manifest), rendered frames, the client's buffer, `state.json` and the
logs (`server.log`, `client.log`). Delete it any time nothing is running.
