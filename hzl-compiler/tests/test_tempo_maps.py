# SPDX-License-Identifier: Apache-2.0

from pathlib import Path

import pytest
from conftest import BASE, TreeBuilder, hzl
from test_score import ENV

from hzl_compiler.canonical.cypher import emit_cypher
from hzl_compiler.canonical.model import Model
from hzl_compiler.errors import HzlError
from hzl_compiler.lower.score import lower_score
from hzl_compiler.lower.tempo_maps import TempoMapGraph, Tie, lower_tempo_maps
from hzl_compiler.ref import Ref

MAIN = Ref("tempo_maps", "main")
ROOT_LAYER = "0: base"


def tempo_map(
    name: str,
    layer: str,
    sync: str = "",
    points: str = '[{tempo_map_point: {position: "0:0", tempo: 90.0, hold: true}}]',
    ticks_per_beat: int = 960,
) -> str:
    """A held map, with `sync` as its `synchronization_points` list when given."""
    body = f"ticks_per_beat: {ticks_per_beat}\npoints: {points}\n"
    if sync:
        body += f"synchronization_points: {sync}\n"
    return hzl("tempo_map", f"tempo_maps/{name}", body, layer=layer)


def tie(parent: str, position: str = "0:0", parent_position: str = "0:0") -> str:
    """One `synchronization_point` item, as flow YAML."""
    return (
        f'{{synchronization_point: {{position: "{position}", '
        f"parent: {{namespace: tempo_maps, name: {parent}}}, "
        f'parent_position: "{parent_position}"}}}}'
    )


def graph(tree: TreeBuilder, maps: dict[str, str]) -> TempoMapGraph:
    return lower_tempo_maps(tree({**BASE, **maps}), MAIN)


def compile_(tree: TreeBuilder, files: dict[str, str]) -> Model:
    return lower_score(tree({**BASE, "envelopes/env": ENV, **files}), Ref("scores", "score"))


def test_every_map_is_lowered_root_first_then_by_layer_and_name(tree: TreeBuilder) -> None:
    result = graph(
        tree,
        {
            "tempo_maps/b": tempo_map("b", "1: voices", f"[{tie('main', '0:0', '2:0')}]"),
            "tempo_maps/a": tempo_map("a", "2: echoes", f"[{tie('b', '0:0', '1:0')}]"),
            "tempo_maps/c": tempo_map("c", "1: voices", f"[{tie('main')}]"),
        },
    )
    assert [str(ref) for ref in result.maps] == [
        "tempo_maps/main",
        "tempo_maps/b",
        "tempo_maps/c",
        "tempo_maps/a",
    ]
    assert result.ties == (
        Tie(Ref("tempo_maps", "b"), MAIN, 0, 1920),
        Tie(Ref("tempo_maps", "c"), MAIN, 0, 0),
        Tie(Ref("tempo_maps", "a"), Ref("tempo_maps", "b"), 0, 960),
    )


def test_a_parent_position_counts_in_the_parent_s_ticks(tree: TreeBuilder) -> None:
    result = graph(
        tree,
        {
            "tempo_maps/v": tempo_map(
                "v", "1: v", f"[{tie('main', '0:0', '1:479')}]", ticks_per_beat=4
            )
        },
    )
    assert result.ties[0].parent_ticks == 960 + 479
    with pytest.raises(HzlError, match="ticks per beat"):
        graph(
            tree,
            {
                "tempo_maps/w": tempo_map(
                    "w", "1: w", f"[{tie('main', '0:0', '0:960')}]", ticks_per_beat=4
                )
            },
        )


@pytest.mark.parametrize("layer", [ROOT_LAYER, "0: a", "-1: earlier"])
def test_a_tie_runs_to_a_strictly_lower_layer(tree: TreeBuilder, layer: str) -> None:
    with pytest.raises(HzlError, match="strictly lower layer") as raised:
        graph(tree, {"tempo_maps/v": tempo_map("v", layer, f"[{tie('main')}]")})
    assert raised.value.where.path.endswith(
        "synchronization_points[0].synchronization_point.parent"
    )


def test_layers_compare_as_strings_not_as_numbers(tree: TreeBuilder) -> None:
    # "10" sorts before "9": the prefix is a convention, never parsed.
    files = {
        "tempo_maps/main": tempo_map("main", "000:Root"),
        "tempo_maps/nine": tempo_map("nine", "9:x", f"[{tie('main')}]"),
        "tempo_maps/ten": tempo_map("ten", "10:x", f"[{tie('nine')}]"),
    }
    with pytest.raises(HzlError, match="strictly lower layer"):
        graph(tree, files)


def test_the_root_map_has_no_ties(tree: TreeBuilder) -> None:
    files = {
        "tempo_maps/main": tempo_map("main", "1: root", f"[{tie('other')}]"),
        "tempo_maps/other": tempo_map("other", "0: other"),
    }
    with pytest.raises(HzlError, match="has no synchronization_points; it plays as written"):
        graph(tree, files)


def test_a_map_without_ties_does_not_lead_to_the_root(tree: TreeBuilder) -> None:
    with pytest.raises(HzlError, match="tempo_maps/lost has no synchronization_points"):
        graph(tree, {"tempo_maps/lost": tempo_map("lost", "1: lost")})


def test_a_map_tied_only_to_an_orphan_is_reported_at_the_orphan(tree: TreeBuilder) -> None:
    files = {
        "tempo_maps/lost": tempo_map("lost", "1: lost"),
        "tempo_maps/child": tempo_map("child", "2: child", f"[{tie('lost')}]"),
    }
    with pytest.raises(HzlError, match="tempo_maps/lost has no synchronization_points"):
        graph(tree, files)


def test_a_parent_must_be_a_tempo_map(tree: TreeBuilder) -> None:
    sync = (
        "[{synchronization_point: {position: 0, "
        "parent: {namespace: transformations, name: update}, parent_position: 0}}]"
    )
    with pytest.raises(HzlError, match="is a partial_state_set_transformation, but a tempo_map"):
        graph(tree, {"tempo_maps/v": tempo_map("v", "1: v", sync)})


def test_a_sync_point_is_at_one_of_the_map_s_points(tree: TreeBuilder) -> None:
    with pytest.raises(HzlError, match="no tempo_map_point at 1:0"):
        graph(tree, {"tempo_maps/v": tempo_map("v", "1: v", f"[{tie('main', '1:0')}]")})


def test_two_sync_points_cannot_share_a_tick(tree: TreeBuilder) -> None:
    sync = f"[{tie('main', '0:0', '0:0')}, {tie('main', '0:0', '2:0')}]"
    with pytest.raises(HzlError, match="0:0 already has a synchronization point"):
        graph(tree, {"tempo_maps/v": tempo_map("v", "1: v", sync)})


TWO_POINTS = (
    '[{tempo_map_point: {position: "0:0", tempo: 90.0, hold: false}},'
    ' {tempo_map_point: {position: "4:0", tempo: 90.0, hold: true}}]'
)


def test_ties_to_one_parent_keep_their_order(tree: TreeBuilder) -> None:
    # Written in either order in the file; compared by position on the child.
    sync = f"[{tie('main', '4:0', '3:0')}, {tie('main', '0:0', '2:0')}]"
    assert len(graph(tree, {"tempo_maps/v": tempo_map("v", "1: v", sync, TWO_POINTS)}).ties) == 2
    sync = f"[{tie('main', '4:0', '2:0')}, {tie('main', '0:0', '2:0')}]"
    with pytest.raises(HzlError, match="ties to one parent keep their order"):
        graph(tree, {"tempo_maps/v": tempo_map("v", "1: v", sync, TWO_POINTS)})


def test_non_hzl_yaml_in_the_tree_is_left_alone_but_broken_yaml_is_not(
    tree: TreeBuilder, tmp_path: Path
) -> None:
    (tmp_path / "piece.yaml").write_text("not: [hzl\n", encoding="utf-8")
    (tmp_path / "requests").mkdir()
    (tmp_path / "requests" / "render.yaml").write_text("frames: [{frame: 0}]\n", encoding="utf-8")
    assert list(graph(tree, {}).maps) == [MAIN]
    (tmp_path / "requests" / "broken.yaml").write_text("frames: [\n", encoding="utf-8")
    with pytest.raises(HzlError, match="cannot be told apart from a tempo_map"):
        graph(tree, {})


def test_only_the_root_hangs_from_the_score_and_ties_are_emitted(tree: TreeBuilder) -> None:
    model = compile_(
        tree, {"tempo_maps/voice": tempo_map("voice", "1: v", f"[{tie('main', '0:0', '8:0')}]")}
    )
    assert [m.properties()["name"] for m in model.rows["TempoMap"]] == [
        "tempo_maps/main",
        "tempo_maps/voice",
    ]
    from_score = [e for e in model.edges if e.rel == "NEXT_POINT" and e.src.table == "Score"]
    assert [e.dst.index for e in from_score] == [0]
    (sync,) = [e for e in model.edges if e.rel == "TEMPO_MAP_SYNCHRONIZATION_POINT"]
    assert (sync.src.index, sync.dst.index) == (1, 0)
    assert sync.properties == {"sync_point_parent": 7680, "sync_point_child": 0}
    assert (
        "COPY TEMPO_MAP_SYNCHRONIZATION_POINT FROM (UNWIND [\n"
        "    {from: 1, to: 0, sync_point_parent: 7680, sync_point_child: 0}\n"
        "] AS r RETURN r.from, r.to, r.sync_point_parent, r.sync_point_child) "
        "(from='TempoMap', to='TempoMap');"
    ) in emit_cypher(model)
