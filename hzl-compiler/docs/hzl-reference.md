# HzL reference, version 0.1.0

What `hzlc` reads. This is the language as it stands in the first version of
the compiler; it descends from the proof of concept (`proof-of-concept/hzl-language/`),
with seconds replaced by musical time, amplitude in dB, and a score, a tempo
map and the two levels between a score and an envelope added. It is a
description of the current state, not a specification of where the language
is going; `architecture/overview.md` §5.1 says how early that design is.

---

## 1. Files and references

An HzL source tree is a directory. Every object is one YAML file at
`<root>/<namespace>/<name>.yaml`, and other files refer to it as

```yaml
{namespace: <namespace>, name: <name>}
```

A namespace is a `/`-separated path of plain names (`[A-Za-z0-9_][A-Za-z0-9_.-]*`),
so it may be nested. A name is one such segment. A file is a mapping with
exactly one top-level key, its type, whose value is the body.

The body begins with the common header:

```yaml
<type>:
  hzl_version: "0.1.0"     # the language version; must be this one
  hzl_type: <type>         # must equal the top-level key
  name: <name>             # must equal the file name without .yaml
  namespace: <namespace>   # must equal the directories under the root
  layer: "0: base"         # any string; carried, not enforced (§8)
  labels: [a, b]           # optional; strings, each once
```

A file whose header disagrees with where it is was found by the wrong
reference, and is refused. Unknown keys anywhere are an error: YAML would
accept a misspelt key silently and the compiler would then ignore it.

The compilation root is a `score`. `hzlc compile <score file>` finds the
source root from the score's namespace.

## 2. Musical time

Every musical position and duration is `beats:ticks`: two non-negative
integers, the second below the tempo map's `ticks_per_beat`. A bare integer
is that many beats and no ticks. A float is an error: fractions of a beat are
written as ticks, and a composer who needs finer resolution raises
`ticks_per_beat`. Nothing on this path is floating point. There are no bars
and no time signatures; rhythm is left neutral at this level.

```yaml
duration: "2:480"    # two and a half beats at 960 ticks per beat
position: "4"        # four beats
position: 4          # the same
```

The one time written in seconds is `offset_seconds` on an envelope stage (§7.6).

## 3. Values and units

| Property | Unit | Range | Where |
| --- | --- | --- | --- |
| `frequency` | Hz | any finite value; outside the audible range is not rendered | partial states, notes, series |
| `amplitude` | dB | [-144, 0]; -144 is silence | partial states, notes, series |
| `tempo` | BPM | [1, 1000] | tempo map points |
| `ticks_per_beat` | ticks | 1 or more | tempo map |
| `offset_seconds` | s | finite | envelope stages |
| `curve_amount` | — | [0, 1] | envelope stages |

`phase`, `azimuth`, `elevation`, `distance` and `pan`, which the proof of
concept had, are refused with "not in this version" wherever they appear.

## 4. The hierarchy: `score`, `sound_entity`, `sound_source`

These three mirror `engine-db`'s `Score → SoundEntity → SoundSource → PartialSet`.
Each level lists the next with a musical time; the times add.

```yaml
# scores/demo.yaml
score:
  ...header...
  tempo_map: {namespace: tempo_maps, name: main}
  sound_entities:
    - sound_entity: {namespace: entities, name: pad}
      position: "0:0"            # where the entity starts; default 0:0

# entities/pad.yaml
sound_entity:
  ...header...
  sound_sources:
    - sound_source: {namespace: sources, name: left}
      offset: "0:0"              # after the entity's start; default 0:0

# sources/left.yaml
sound_source:
  ...header...
  envelopes:
    - envelope: {namespace: envelopes, name: swell}
      offset: "0:0"              # after the source's start; default 0:0
```

An envelope starts at `entity.position + source.offset + envelope.offset`,
in ticks. Each envelope may be placed once in a score; a second placement is
an error. A `sound_source` is where a spatial position will be written when
the compiler carries one; a `position` key there is refused now.

Every file at these levels becomes one node, named `namespace/name`.

## 5. `tempo_map`

```yaml
tempo_map:
  ...header...
  ticks_per_beat: 960
  points:
    - tempo_map_point:
        position: "0:0"          # ticks of this map; the first must be 0:0
        tempo: 120.0             # BPM from here
        hold: false              # false: ramp linearly in position to the next tempo
    - tempo_map_point:
        position: "4:0"          # strictly after the previous
        tempo: 60.0
        hold: true               # the last point must hold
```

A ramp is linear in musical position (ADR 0013). A score has one tempo map
in this version.

## 6. State sets

A *state set* is a list of partials with values. Four file types are
state-set-valued; a reference to a state set may name any of them.

### 6.1 `partial_state_set`

```yaml
partial_state_set:
  ...header...
  partial_states:
    - partial_state:
        identifier: A            # required; string or integer; unique in the set; no '#' or '/'
        labels: [lead]           # optional
        frequency: 440.0         # optional
        amplitude: -6.0          # optional
```

A property left out means "this set says nothing about it" (§7.4).

### 6.2 `scale`

```yaml
scale:
  ...header...
  notes:
    - note: C4                   # required; unique; becomes the partial's label
      frequency: 261.63          # optional
      amplitude: -6.0            # optional
```

Lowers to a `partial_state_set` with one partial per note, identifier `0`,
`1`, ... in note order.

### 6.3 `harmonic_series`

```yaml
harmonic_series:
  ...header...
  base_frequency: 1.0                        # optional, default 1; multiplies frequency_expression
  number_of_harmonics: 16                    # required, 1 or more
  frequency_expression: "harmonic_index"     # or a constant: frequency: 440.0
  amplitude_expression: "-20*log10(harmonic_index)"   # or a constant: amplitude: -6.0
```

One partial per harmonic, `harmonic_index` from 1; identifier `"1"`, `"2"`, ...
and label `harmonic 1`, `harmonic 2`, .... An expression is arithmetic over
`harmonic_index`, evaluated by SymPy; it may use `pi`, `e`, `log`, `log10`,
`log2`, `exp`, `sqrt`, `sin`, `cos`, `abs`, `floor`, `ceiling`, `Max`, `Min`.
Giving both a constant and an expression for one property is an error, as is
a value out of range at any harmonic.

### 6.4 `virtual_partial_state_set`

```yaml
virtual_partial_state_set:
  ...header...
  transformation: {namespace: transformations, name: product}   # must be a Flattened Cartesian Product
  partial_state_sets:                                           # exactly two
    - partial_state_set: {namespace: scales, name: c_major}
    - partial_state_set: {namespace: harmonic_series, name: rolloff}
```

Every partial of the first with every partial of the second. Frequencies
multiply; amplitudes add (the dB form of a linear product) and are floored at
-144; a property only one side has is taken from that side. The identifier is
`<first>*<second>`, the labels the union of the two partials', and the set's
labels the union of the virtual set's, the transformation's and both
sources'. A chain of references that returns to itself is an error.

### 6.5 `partial_state_set_transformation`

```yaml
partial_state_set_transformation:
  ...header...
  transformation:
    transformation_type: "Update"            # or "Flattened Cartesian Product"
```

The file carries only the type for now. `Update` is used in envelopes;
`Flattened Cartesian Product` in virtual sets.

## 7. `envelope`

An envelope is a partial set and its history in musical time. Lowered, it
is one `PartialSet` with one `PartialSetMusicalTimeline`.

```yaml
envelope:
  ...header...
  initial_partial_state_set: {namespace: state_sets, name: start}
  envelope_stages:
    - envelope_stage:
        duration: "2:0"                      # required
        offset_seconds: 0.0                  # optional (§7.6)
        curve: linear                        # optional, default linear (§7.5)
        curve_amount: 0.0                    # optional, default 0
        frequency_curve: s_curve             # optional per-property override
        frequency_curve_amount: 1.0
        amplitude_curve: step
        amplitude_curve_amount: 0.0
        envelope_transforms:                 # optional; may be empty
          - envelope_transform:
              filter: "^harmonic [2-9]"      # optional regex over labels (§7.3)
              transformation: {namespace: transformations, name: update}   # must be Update
              source_partial_state_set: {namespace: state_sets, name: peak}
```

### 7.1 Partials and the first point

The initial set gives the partials: one per state, identifier, labels and
all. Each partial is named `<initial set's namespace>#<identifier>`. Every
partial must have both `frequency` and `amplitude` there, since the first
point starts its lifeline.

### 7.2 Stages and points

Each stage ends at a new point, `duration` after the previous one. The
timeline's points are therefore the initial point and one per stage, at the
cumulative durations.

### 7.3 `Update`

Within a stage, each transform is applied in order. A partial is *selected*
when the filter is absent, or matches any of its labels (`re.search`). For
each selected partial whose identifier the source set has a state for, the
state's present values become the partial's values at this point. A later
transform overrides an earlier one property by property. A source state
whose identifier is not a partial of this set is ignored.

### 7.4 What a stage leaves alone

A partial no transform touched, or a property the source state left out,
has no value at that point. In the canonical model that is "no knot here":
the partial's envelope for that property continues from its previous knot
to its next one. This is how a set can restate a few partials at a point
without restating them all.

At the **last** point every partial must have both properties: it ends the
lifeline. The compiler reports the partial and the envelope otherwise.

### 7.5 Curves

The stage's curve describes the path *through* the stage. The canonical
model stores a point's curve as the path from that point to the next
(rendering-engine/docs/precomputation.md §4), so stage *k*'s curve lands on
point *k*, where the stage starts; the last point's curve fields are linear
and have no meaning. Curves: `linear` (0), `step` (1), `accelerate` (2),
`decelerate` (3), `s_curve` (4), with `curve_amount` in [0, 1]. The engine
renders only linear today; other values are carried through unchanged.

### 7.6 `offset_seconds`

A physical offset of the point the stage ends at, in seconds, independent of
tempo: the way to give a percussive attack the same length at any tempo. A
stage of `duration: "0:0"` with `offset_seconds: 0.25` makes a point a
quarter of a second after the previous one whatever the tempo. When offsets
make points collide in physical time the transform keeps the composer's
order and lets time yield (ADR 0014); the compiler does not check for it.

## 8. Layers and namespaces

Both are parsed and carried on every file and nothing is enforced about
them: there is no layer ordering, and a reference may lead anywhere in the
tree. The idea of layers in lexicographic order as a guard against circular
dependencies is on the table and deliberately untouched by this version.
What the compiler does refuse is a cycle of state-set references (§6.4),
which it detects directly.

## 9. What is emitted

One `EngineConfig` statement from the compiler's options, then one
statement for the score: a `Score`, its `TempoMap` and points; `Label`s;
the `SoundEntity`/`SoundSource`/`PartialSet`/`Partial` hierarchy; per
envelope a `PartialSetMusicalTimeline`, its points each with a `POINT_AT`
(`sync_point` in ticks, `offset_seconds`), and at each point with states a
`PartialStateSet` of `PartialState`s. `HAS_LABEL` is emitted for partials
and the score only, which is what the schema declares. `phase` is NULL
everywhere; `phase_lock` false. See `docs/design.md`.
