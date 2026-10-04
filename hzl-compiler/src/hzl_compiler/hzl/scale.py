"""`scale`: a generator giving one partial per note, labelled with the note."""

from __future__ import annotations

from dataclasses import dataclass

from ..nodes import Node
from ..ref import Ref
from ..source import SourceTree
from .common import (
    HEADER_KEYS,
    Header,
    Phase,
    check_amplitude,
    check_frequency,
    optional_phase,
    parse_header,
    reject_not_in_this_version,
)


@dataclass(frozen=True)
class Note:
    """One note of a scale.

    Attributes:
        note: The note's name, which becomes the partial's label. Unique in
            the scale.
        frequency: Hz, or None.
        amplitude: dB, or None.
        phase: A target in degrees, `FREE_RUN`, or None for the default.
    """

    note: str
    frequency: float | None
    amplitude: float | None
    phase: Phase = None


@dataclass(frozen=True)
class Scale:
    """A parsed `scale` file.

    Attributes:
        header: The file's header.
        notes: In file order. The partial for the note at index i has
            identifier `str(i)`.
    """

    header: Header
    notes: tuple[Note, ...]


def parse_scale(node: Node) -> Scale:
    """Reads a `scale` body.

    Raises:
        HzlError: if there are no notes, a note lacks its name or repeats one,
            a value is out of range, or a key is unknown or not in this version.
    """
    node.only(*HEADER_KEYS, "notes")
    header = parse_header(node)
    items = node.require("notes", "list")
    if not items:
        raise node.error("a scale needs at least one note", "notes")
    notes: list[Note] = []
    names: set[str] = set()
    for index, item in enumerate(items):
        where = node.where.at("notes").at(index)
        if not isinstance(item, dict):
            raise where.error("each note is a mapping with at least a 'note' key")
        note_node = Node(item, where)
        reject_not_in_this_version(note_node)
        note_node.only("note", "frequency", "amplitude", "phase")
        name = str(note_node.require("note", "scalar"))
        if name in names:
            raise note_node.error(f"note {name!r} is repeated", "note")
        names.add(name)
        frequency = note_node.optional("frequency", "float")
        amplitude = note_node.optional("amplitude", "float")
        notes.append(
            Note(
                note=name,
                frequency=None if frequency is None else check_frequency(frequency, where.at("frequency")),
                amplitude=None if amplitude is None else check_amplitude(amplitude, where.at("amplitude")),
                phase=optional_phase(note_node),
            )
        )
    return Scale(header, tuple(notes))


def load_scale(tree: SourceTree, ref: Ref) -> Scale:
    """Loads and parses the `scale` that `ref` names."""
    return parse_scale(tree.load(ref, "scale"))
