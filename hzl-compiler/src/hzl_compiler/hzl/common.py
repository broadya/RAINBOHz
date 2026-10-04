"""What every HzL file shares: the header, and the value checks the schema delegates.

`engine-db` has no CHECK constraints. Its schema says which ranges are
"compiler-enforced", and this module is where that enforcement lives, so that
a bad value is reported at the line it was written rather than when the
engine refuses the score.
"""

from __future__ import annotations

import math
from dataclasses import dataclass
from enum import Enum

from ..errors import Where
from ..nodes import Node
from ..ref import Ref

# The header keys every HzL type carries. `hzl_version`, `hzl_type`, `name` and
# `namespace` are checked against the file's location when it is loaded
# (source.py); `layer` and `labels` are read here.
HEADER_KEYS = ("hzl_version", "hzl_type", "name", "namespace", "layer", "labels")

# PartialState.amplitude is dB in [-144, 0]; -144 dB stands for silence.
MIN_AMPLITUDE_DB = -144.0
MAX_AMPLITUDE_DB = 0.0

# PartialState.phase is a target in degrees in [-180, 180].
MIN_PHASE_DEG = -180.0
MAX_PHASE_DEG = 180.0


class FreeRun(Enum):
    """The one HzL phase that is not a number: `phase: free`."""

    FREE = "free"


# A free-running point: the partial accumulates phase up to here without a
# target (rendering-engine/docs/transform.md §6.6).
FREE_RUN = FreeRun.FREE

# A partial's phase as HzL gives it: a target in degrees, `FREE_RUN`, or None
# for the default, which plays no role in phase.
Phase = float | FreeRun | None

# Properties the PoC had and this version does not carry. Each is refused
# with a message rather than dropped, so that a composer who writes one is
# told it had no effect. The reason text follows the key.
NOT_IN_THIS_VERSION = {
    "azimuth": "spatial position is not in this version of the compiler; leave it out",
    "elevation": "spatial position is not in this version of the compiler; leave it out",
    "distance": "spatial position is not in this version of the compiler; leave it out",
    "pan": "spatial position is not in this version of the compiler; leave it out",
}


@dataclass(frozen=True)
class Header:
    """The common header of an HzL file.

    Attributes:
        hzl_type: The file's type, its top-level key.
        name: The file name without extension.
        namespace: The slash-delimited directories under the source root.
        layer: Carried as written; nothing enforces an order of layers yet.
        labels: Labels in file order, each once.
    """

    hzl_type: str
    name: str
    namespace: str
    layer: str
    labels: tuple[str, ...]

    @property
    def ref(self) -> Ref:
        """The reference that names this file."""
        return Ref(self.namespace, self.name)

    @property
    def qualified_name(self) -> str:
        """`namespace/name`, the name the canonical model gives the object."""
        return f"{self.namespace}/{self.name}"


def parse_header(node: Node) -> Header:
    """Reads the header of a file body whose location has already been checked.

    Args:
        node: The file body. `source.SourceTree` has verified its version,
            type, name and namespace against the reference it was loaded by.

    Returns:
        The header.

    Raises:
        HzlError: if `layer` is missing or not a string, or labels are malformed.
    """
    return Header(
        hzl_type=node.require("hzl_type", "str"),
        name=str(node.require("name", "scalar")),
        namespace=node.require("namespace", "str"),
        layer=str(node.require("layer", "scalar")),
        labels=node.labels(),
    )


def check_frequency(value: float, where: Where) -> float:
    """Checks a frequency in Hz is finite.

    The schema allows any finite value, including negative or beyond the
    audible range; such a partial is simply not rendered.

    Raises:
        HzlError: if the value is NaN or infinite.
    """
    if not math.isfinite(value):
        raise where.error(f"frequency must be a finite number of Hz, got {value!r}")
    return value


def check_amplitude(value: float, where: Where) -> float:
    """Checks an amplitude is finite dB within [-144, 0].

    Raises:
        HzlError: if the value is outside the range or not finite.
    """
    if not (math.isfinite(value) and MIN_AMPLITUDE_DB <= value <= MAX_AMPLITUDE_DB):
        raise where.error(
            f"amplitude is in dB and must lie in [{MIN_AMPLITUDE_DB:g}, {MAX_AMPLITUDE_DB:g}], got {value!r}"
        )
    return value


def check_phase(value: float, where: Where) -> float:
    """Checks a phase target is finite degrees within [-180, 180].

    Raises:
        HzlError: if the value is outside the range or not finite.
    """
    if not (math.isfinite(value) and MIN_PHASE_DEG <= value <= MAX_PHASE_DEG):
        raise where.error(
            f"phase is in degrees and must lie in [{MIN_PHASE_DEG:g}, {MAX_PHASE_DEG:g}], got {value!r}"
        )
    return value


def optional_phase(node: Node, key: str = "phase") -> Phase:
    """Reads a phase: degrees for a target, `free` for a free-running point.

    An absent key is the default (None). An explicit `null` is refused rather
    than read as the default: the proof of concept wrote `null` for a
    free-running phase, and the two must not be confused silently.

    Raises:
        HzlError: if the value is null, a string other than `free`, or a
            number out of range.
    """
    if not node.has(key):
        return None
    value = node.optional(key, "scalar")
    if value is None:
        raise node.error(f"{key} may not be null; write 'free' for a free-running phase, or leave it out", key)
    if isinstance(value, str):
        if value != FREE_RUN.value:
            raise node.error(f"{key} must be degrees or 'free', got {value!r}", key)
        return FREE_RUN
    return check_phase(float(value), node.where.at(key))


def parse_identifier(value: object, where: Where) -> str:
    """Reads a partial identifier, which may be written as a string or an integer.

    The identifier is joined to a namespace as `namespace#identifier` to name
    the partial, so it may not itself contain `#`, nor `/`.

    Raises:
        HzlError: if the value is not a non-empty string or an integer, or
            contains `#` or `/`.
    """
    if isinstance(value, bool) or not isinstance(value, (str, int)):
        raise where.error("identifier must be a string or an integer")
    text = str(value)
    if not text or "#" in text or "/" in text:
        raise where.error(f"identifier {text!r} must be non-empty and contain neither '#' nor '/'")
    return text


def reject_not_in_this_version(node: Node) -> None:
    """Refuses every property this version does not carry, if present."""
    for key, reason in NOT_IN_THIS_VERSION.items():
        node.reject(key, reason)
