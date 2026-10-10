# SPDX-License-Identifier: Apache-2.0

import pytest

from hzl_compiler.errors import HzlError, Where
from hzl_compiler.musical_time import MusicalTime

W = Where(None, "x")


@pytest.mark.parametrize(
    "value, expected",
    [
        ("2:480", MusicalTime(2, 480)),
        ("4", MusicalTime(4, 0)),
        (4, MusicalTime(4, 0)),
        (0, MusicalTime(0, 0)),
        (" 1 : 2 ", MusicalTime(1, 2)),
    ],
)
def test_parse_accepts_beats_ticks_and_whole_beats(value: object, expected: MusicalTime) -> None:
    assert MusicalTime.parse(value, W) == expected


@pytest.mark.parametrize("value", [1.5, -1, True, "1.5", "-1:0", "a:b", "1:", None, "", "1:2:3"])
def test_parse_rejects_everything_else(value: object) -> None:
    with pytest.raises(HzlError, match="beats:ticks"):
        MusicalTime.parse(value, W)


def test_to_ticks_is_exact_integer_arithmetic() -> None:
    assert MusicalTime(2, 480).to_ticks(960, W) == 2400
    assert MusicalTime(0, 0).to_ticks(1, W) == 0


def test_to_ticks_refuses_ticks_at_or_beyond_the_beat() -> None:
    with pytest.raises(HzlError, match="960 ticks per beat"):
        MusicalTime(1, 960).to_ticks(960, W)


def test_str_round_trips() -> None:
    assert str(MusicalTime(3, 7)) == "3:7"
