# SPDX-License-Identifier: Apache-2.0

"""Lowers the tempo maps of a source tree together: the graph of their ties.

Every `tempo_map` file in the tree is compiled, not only the ones a placement
names. The score's map is the root and has no synchronisation points; every
other map ties itself to parents with them, and each tie runs from a map to a
map in a strictly lower layer (ADR 0024). Layers compare as strings, in code
point order, so the tie graph cannot have a cycle, and a map leads to the root
when one of its parents does.

The rules that need the maps in physical time, the anchors' order across
parents, stretch feasibility and the tempo range after compensation, are the
engine's (rendering-engine/docs/polytempo.md §6).
"""

from __future__ import annotations

from dataclasses import dataclass

from ..errors import Where
from ..hzl.tempo_map import TempoMap, load_tempo_map
from ..ref import Ref
from ..source import SourceTree


@dataclass(frozen=True)
class Tie:
    """One synchronisation point, lowered: a `TEMPO_MAP_SYNCHRONIZATION_POINT`.

    Attributes:
        child: The map the point is on.
        parent: The map it is tied to.
        child_ticks: `sync_point_child`, ticks of the child.
        parent_ticks: `sync_point_parent`, ticks of the parent.
    """

    child: Ref
    parent: Ref
    child_ticks: int
    parent_ticks: int


@dataclass(frozen=True)
class TempoMapGraph:
    """The tempo maps of a score, checked against each other.

    Attributes:
        root: The score's map.
        maps: Every map in the tree, the root first, then by layer and name.
        ties: Every synchronisation point, in the order of `maps` and then by
            position on the child.
    """

    root: Ref
    maps: dict[Ref, TempoMap]
    ties: tuple[Tie, ...]

    def lookup(self, tree: SourceTree, ref: Ref, where: Where) -> TempoMap:
        """The map `ref` names, for a placement written at `where`.

        Raises:
            HzlError: if `ref` names no `tempo_map`, reported as loading it
                reports it.
        """
        found = self.maps.get(ref)
        if found is None:
            load_tempo_map(tree, ref)
            raise where.error(f"{ref} is not a tempo map of this source tree")
        return found


def lower_tempo_maps(tree: SourceTree, root: Ref) -> TempoMapGraph:
    """Loads every tempo map in `tree` and checks their ties to `root`.

    Args:
        tree: The source tree.
        root: The score's tempo map.

    Returns:
        The graph.

    Raises:
        HzlError: if a map fails to load; the root has synchronisation points;
            a parent is not a tempo map, or not in a strictly lower layer than
            its child; a parent position has ticks beyond the parent's beat;
            two ties to one parent are not in the same order on both maps; or
            a map does not lead to the root.
    """
    root_map = load_tempo_map(tree, root)
    if root_map.sync_points:
        raise root_map.where.error(
            "the score's tempo map is the root of every tie and has no "
            "synchronization_points; it plays as written"
        )
    loaded = {ref: load_tempo_map(tree, ref) for ref in tree.refs_of_type("tempo_map")}
    loaded[root] = root_map
    order = [root] + sorted(
        (ref for ref in loaded if ref != root),
        key=lambda ref: (loaded[ref].header.layer, str(ref)),
    )
    maps = {ref: loaded[ref] for ref in order}

    # A parent is in a strictly lower layer, so it comes earlier in `order` and
    # has already passed these checks: a map with a tie leads to the root.
    ties: list[Tie] = []
    for ref in order[1:]:
        child = maps[ref]
        if not child.sync_points:
            raise child.where.error(
                f"tempo map {ref} has no synchronization_points, so it does not lead to the "
                f"score's tempo map {root}; every tempo map in the source tree is compiled"
            )
        last_on_parent: dict[Ref, tuple[int, int]] = {}
        for point in sorted(child.sync_points, key=lambda p: p.position_ticks):
            parent = maps.get(point.parent) or load_tempo_map(tree, point.parent)
            where = point.where.at("parent")
            if not parent.header.layer < child.header.layer:
                raise where.error(
                    f"a tie runs to a tempo map in a strictly lower layer; {point.parent} is in "
                    f"layer {parent.header.layer!r} and {ref} in {child.header.layer!r}"
                )
            parent_ticks = point.parent_position.to_ticks(
                parent.ticks_per_beat, point.where.at("parent_position")
            )
            previous = last_on_parent.get(point.parent)
            if previous is not None and parent_ticks <= previous[1]:
                raise point.where.error(
                    f"ties to one parent keep their order: {point.position} is after the tie at "
                    f"{previous[0]} ticks of this map, so its parent_position must be after "
                    f"{previous[1]} ticks of {point.parent}, got {parent_ticks}"
                )
            last_on_parent[point.parent] = (point.position_ticks, parent_ticks)
            ties.append(Tie(ref, point.parent, point.position_ticks, parent_ticks))
    return TempoMapGraph(root, maps, tuple(ties))
