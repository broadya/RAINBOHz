# SPDX-License-Identifier: Apache-2.0

"""The canonical model as tables: one row list per node table, one edge list.

This is the lowest level of HzL. It is shaped like `engine-db` itself, one
row per node and one edge per relationship, with compiler-assigned local
keys, so that a serialisation is a plain walk over the rows. Cypher `CREATE`
statements are one such walk (cypher.py); a columnar format for Ladybug's
`COPY FROM` would be another, over the same rows.

Row classes carry the table name and give their properties as `engine-db`
spells them. A property whose value is None is absent, which the database
stores as NULL.
"""

from __future__ import annotations

from dataclasses import dataclass, field, fields
from typing import Any, ClassVar


@dataclass(frozen=True)
class NodeKey:
    """Identifies a row of the model: its table and its index in that table.

    Attributes:
        table: The node table name, as in the schema.
        index: 0-based position in the table's row list.
    """

    table: str
    index: int

    @property
    def variable(self) -> str:
        """The Cypher variable for this row, e.g. `partialState12`."""
        return self.table[0].lower() + self.table[1:] + str(self.index)


@dataclass(frozen=True)
class Row:
    """Base of every node row. Subclasses set `TABLE` and declare their columns."""

    TABLE: ClassVar[str]

    def properties(self) -> dict[str, Any]:
        """The row's properties as `engine-db` spells them, None values left out.

        Columns are spelt in the dataclass as in the schema, so the field
        names are the property names. `EngineConfigRow` overrides this because
        its schema columns are camelCase.
        """
        return {
            f.name: getattr(self, f.name) for f in fields(self) if getattr(self, f.name) is not None
        }


@dataclass(frozen=True)
class EngineConfigRow(Row):
    """The engine's singleton configuration; not part of the score."""

    TABLE: ClassVar[str] = "EngineConfig"
    sample_rate: int
    channel_mode: int

    def properties(self) -> dict[str, Any]:
        """Spells the two columns as the schema does: `sampleRate`, `channelMode`."""
        return {"sampleRate": self.sample_rate, "channelMode": self.channel_mode}


@dataclass(frozen=True)
class ScoreRow(Row):
    """A `Score`."""

    TABLE: ClassVar[str] = "Score"
    name: str


@dataclass(frozen=True)
class TempoMapRow(Row):
    """A `TempoMap`."""

    TABLE: ClassVar[str] = "TempoMap"
    name: str
    ticks_per_beat: int


@dataclass(frozen=True)
class TempoMapPointRow(Row):
    """A `TempoMapPoint`: position in ticks, tempo in BPM, hold flag."""

    TABLE: ClassVar[str] = "TempoMapPoint"
    position: int
    start_tempo: float
    hold: bool


@dataclass(frozen=True)
class SoundEntityRow(Row):
    """A `SoundEntity`."""

    TABLE: ClassVar[str] = "SoundEntity"
    name: str


@dataclass(frozen=True)
class SoundSourceRow(Row):
    """A `SoundSource`."""

    TABLE: ClassVar[str] = "SoundSource"
    name: str


@dataclass(frozen=True)
class PartialSetRow(Row):
    """A `PartialSet`."""

    TABLE: ClassVar[str] = "PartialSet"
    name: str


@dataclass(frozen=True)
class PartialRow(Row):
    """A `Partial`."""

    TABLE: ClassVar[str] = "Partial"
    name: str


@dataclass(frozen=True)
class PartialSetMusicalTimelineRow(Row):
    """A `PartialSetMusicalTimeline`; it has no columns of its own."""

    TABLE: ClassVar[str] = "PartialSetMusicalTimeline"


@dataclass(frozen=True)
class PartialSetMusicalTimelinePointRow(Row):
    """A `PartialSetMusicalTimelinePoint`: the curves out of this point.

    The curve on a point describes the path from it to the next point
    (rendering-engine/docs/precomputation.md §4).
    """

    TABLE: ClassVar[str] = "PartialSetMusicalTimelinePoint"
    frequency_curve_type: int
    frequency_curve_amount: float
    amplitude_curve_type: int
    amplitude_curve_amount: float


@dataclass(frozen=True)
class PartialStateSetRow(Row):
    """A `PartialStateSet`; it has no columns of its own."""

    TABLE: ClassVar[str] = "PartialStateSet"


@dataclass(frozen=True)
class PartialStateRow(Row):
    """A `PartialState`: Hz and dB, either NULL for "no envelope node here", and phase.

    `phase` (degrees) and `phase_free_run` are one of three combinations
    (the schema's comment on `PartialState`): a target (phase, False),
    free-running (None, True) or the default (None, None).
    """

    TABLE: ClassVar[str] = "PartialState"
    frequency: float | None
    amplitude: float | None
    phase: float | None = None
    phase_free_run: bool | None = None


@dataclass(frozen=True)
class SpatialPositionMusicalTimelineRow(Row):
    """A `SpatialPositionMusicalTimeline`, a source's position; it has no columns of its own."""

    TABLE: ClassVar[str] = "SpatialPositionMusicalTimeline"


@dataclass(frozen=True)
class SpatialPositionMusicalTimelinePointRow(Row):
    """A `SpatialPositionMusicalTimelinePoint`: the curves out of this point, per property."""

    TABLE: ClassVar[str] = "SpatialPositionMusicalTimelinePoint"
    azimuth_curve_type: int
    azimuth_curve_amount: float
    elevation_curve_type: int
    elevation_curve_amount: float
    distance_curve_type: int
    distance_curve_amount: float


@dataclass(frozen=True)
class SpatialPositionStateRow(Row):
    """A `SpatialPositionState`: degrees, degrees and metres, each NULL for "no envelope node here".

    These are the canonical model's units; the transform converts azimuth
    and elevation to radians (rendering-engine/docs/transform.md §6.8).
    """

    TABLE: ClassVar[str] = "SpatialPositionState"
    azimuth: float | None
    elevation: float | None
    distance: float | None


@dataclass(frozen=True)
class LabelRow(Row):
    """A `Label`."""

    TABLE: ClassVar[str] = "Label"
    name: str


# Emission order. Every table a relationship names must come before the
# relationship is emitted, which is guaranteed by emitting all nodes first;
# this order is for the reader of the output.
TABLE_ORDER = (
    EngineConfigRow.TABLE,
    ScoreRow.TABLE,
    TempoMapRow.TABLE,
    TempoMapPointRow.TABLE,
    LabelRow.TABLE,
    SoundEntityRow.TABLE,
    SoundSourceRow.TABLE,
    SpatialPositionMusicalTimelineRow.TABLE,
    SpatialPositionMusicalTimelinePointRow.TABLE,
    SpatialPositionStateRow.TABLE,
    PartialSetRow.TABLE,
    PartialRow.TABLE,
    PartialSetMusicalTimelineRow.TABLE,
    PartialSetMusicalTimelinePointRow.TABLE,
    PartialStateSetRow.TABLE,
    PartialStateRow.TABLE,
)


@dataclass(frozen=True)
class Edge:
    """One relationship between two rows.

    Attributes:
        rel: The relationship table name.
        src: The FROM row.
        dst: The TO row.
        properties: The relationship's properties, if any.
    """

    rel: str
    src: NodeKey
    dst: NodeKey
    properties: dict[str, Any] = field(default_factory=dict)


class Model:
    """The rows and edges of one compiled score.

    Attributes:
        rows: Row lists keyed by table name, in `TABLE_ORDER`.
        edges: Every relationship, in the order added.
    """

    def __init__(self) -> None:
        """Creates an empty model with every table present."""
        self.rows: dict[str, list[Row]] = {table: [] for table in TABLE_ORDER}
        self.edges: list[Edge] = []
        self._labels: dict[str, NodeKey] = {}

    def add(self, row: Row) -> NodeKey:
        """Appends `row` to its table and returns its key."""
        table = self.rows[row.TABLE]
        table.append(row)
        return NodeKey(row.TABLE, len(table) - 1)

    def link(self, rel: str, src: NodeKey, dst: NodeKey, **properties: Any) -> Edge:
        """Appends an edge of table `rel` from `src` to `dst` with `properties`."""
        edge = Edge(rel, src, dst, dict(properties))
        self.edges.append(edge)
        return edge

    def label(self, name: str) -> NodeKey:
        """Returns the `Label` row for `name`, creating it on first use.

        Labels are shared: two partials with the same label point at one row.
        """
        if name not in self._labels:
            self._labels[name] = self.add(LabelRow(name))
        return self._labels[name]

    def count(self, table: str) -> int:
        """How many rows `table` has."""
        return len(self.rows[table])

    def to_dict(self) -> dict[str, Any]:
        """The model as plain data, for a JSON dump a person can inspect."""
        return {
            "tables": {
                table: [row.properties() for row in rows] for table, rows in self.rows.items()
            },
            "edges": [
                {
                    "rel": e.rel,
                    "from": e.src.variable,
                    "to": e.dst.variable,
                    "properties": e.properties,
                }
                for e in self.edges
            ],
        }
