# hzl-compiler

The compiler for HzL (Hertz Language), the practitioner-facing language of
RAINBOHz. It reads a score and every file the score reaches, lowers the
higher-level constructs to the lowest level of the language, and emits that
level as a Cypher script that `rendering-engine` loads into `engine-db`.
Python 3.12.

This is the actual route into the engine:

```
HzL source --> hzlc --> canonical model (Cypher) --> rendering-engine --> audio
```

The hand-written scores in `engine-db/examples/` remain the engine's test
route. `examples/tempo-map-score/` here is the HzL twin of one of them, and
`make test-hzl-oracle` proves the two routes give the same audio.
`examples/spatial-score/` is the twin of `spatial-musical-score.cypher`.

## Running it

From the repository root:

```bash
make hzl-compiler                 # create .venv and install the package into it, once
make hzl-compile                  # compile examples/tempo-map-score to build/
make hzl-compile HZL_SCORE=hzl-compiler/examples/demo/scores/demo.yaml
make run-hzl-score HZL_SCORE=hzl-compiler/examples/demo/scores/demo.yaml \
    RE_REQUEST=hzl-compiler/examples/demo/render-request.yaml   # ...and render it
make test-hzl-compiler            # pytest
make test-hzl-oracle              # compiled twin == hand-written score, by the oracle
```

Or directly, once installed:

```bash
.venv/bin/hzlc compile examples/demo/scores/demo.yaml -o build/demo.cypher
.venv/bin/hzlc compile --help
```

`hzlc compile` takes the score's file and works the source root out from the
score's `namespace`. It writes one `.cypher` file. `--sample-rate` and
`--channel-mode` set the `EngineConfig` the engine needs, which is not part of
the score; `--dump-canonical FILE.json` also writes the canonical model's
tables as JSON, for inspection. Errors name the file, the place in it and what
is wrong, in HzL terms, and exit 1.

## Layout

```
src/hzl_compiler/
  source.py, ref.py, nodes.py   finding files by (namespace, name); reading YAML with locations
  musical_time.py               exact beats:ticks
  hzl/                          one module per HzL file type: dataclass + parser + loader
  lower/state_sets.py           L3, L2: scale, harmonic_series, flattened cartesian product
  lower/timeline.py             L1: envelope -> a partial set's musical timeline
  lower/trajectory.py           L1: trajectory -> a sound source's spatial position timeline
  lower/score.py                L0: score -> canonical model
  canonical/model.py            the canonical model as tables (the lowest HzL level)
  canonical/cypher.py           ...emitted as Cypher
  cli.py                        hzlc
tests/                          pytest; tests/expected/ holds the golden scripts
examples/tempo-map-score/       HzL twin of engine-db/examples/tempo-map-score.cypher
examples/spatial-score/         HzL twin of engine-db/examples/spatial-musical-score.cypher (--channel-mode 1)
examples/demo/                  every construct of this version, two sound entities
docs/design.md                  how it works and why
docs/hzl-reference.md           the language, this version
```

`build/` and `.venv/` are gitignored.

## Status

First version, 2026-10-02; phase added 2026-10-03; spatial position
2026-10-05. Frequency, amplitude and phase per partial; spatial position
per sound source, as a `trajectory`; one tempo map per score; envelope
stages apply `Update` only; `beats:ticks` time; amplitude in dB; phase in
degrees or `free`; azimuth and elevation in degrees, distance in metres.
Spatial position per partial, modulators, labels as selection, layers as an
ordering, several tempo maps and the Ninja build are not in this version.
See `docs/design.md` §9 for the list and what each needs.
