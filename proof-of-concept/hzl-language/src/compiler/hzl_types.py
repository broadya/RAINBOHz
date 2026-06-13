from dataclasses import dataclass, field
from abc import ABC, abstractmethod
from typing import List, Tuple, Optional, Dict
from uuid import UUID

# ============================================================
# Data classes for HzL language
# ============================================================

@dataclass
class Envelope:
    """
    Represents an envelope (for frequency, amplitude, etc.)
    with lists of levels, times and curve types.
    """
    levels: List[float]
    times: List[float]
    curves: List[str]
    elapsed_time: float = 0.0

@dataclass
class PhaseCoordinate:
    """
    Represents a phase coordinate at a particular time.
    """
    time: float
    value: float | bool | None = None
    # If float --> phase value
    # If boolean and true --> natural phase
    # If boolean and false --> illegal state
    # If None - undefined phase

@dataclass
class PartialState:
    """
    Represents the state of a partial at any moment in time.
    Contains musical parameters: frequency, amplitude, phase coordinates.
    Labels are the semantic tags.
    """
    labels: List[str] = field(default_factory=list)
    frequency: Optional[float] = None
    amplitude: Optional[float] = None
    azimuth: Optional[float] = None
    elevation: Optional[float] = None
    distance: Optional[float] = None
    phase: float | bool | None = None
    namespace: Optional[str] = None
    identifier: Optional[str] = None
 
@dataclass
class Partial:
    """
    Represents a single partial.
    Contains musical parameters: frequency, amplitude, phase coordinates.
    Also, labels are the semantic tags.
    Identifiers is a transient list of pairs [stage, identifier]
    used for internal bookkeeping.
    """
    labels: List[str] = field(default_factory=list)

    frequency_envelope: Envelope = field(default_factory=lambda: Envelope([], [], []))
    amplitude_envelope: Envelope = field(default_factory=lambda: Envelope([], [], []))
    azimuth_envelope:   Envelope = field(default_factory=lambda: Envelope([], [], []))
    elevation_envelope: Envelope = field(default_factory=lambda: Envelope([], [], []))
    distance_envelope:  Envelope = field(default_factory=lambda: Envelope([], [], []))
    phase_coordinates: List[PhaseCoordinate] = field(default_factory=list)
    elapsed_time: float = 0.0
    current_state: Optional[PartialState] = None

    namespace: Optional[str] = None
    identifier: Optional[str] = None

@dataclass
class PartialSet:
    """
    Represents a set of partials with common metadata.
    This is the output format for partial state information.
    """
    name: str
    namespace: str
    layer: str
    labels: List[str] = field(default_factory=list)
    partials: Dict[str, Partial] = field(default_factory=dict)  # fully qualifier identifier (namespace/identifier) --> Partial

# ============================================================
# Data classes for partial transformation
# ============================================================

@dataclass
class PartialStateSetTransformation(ABC):
    """
    Base class only
    Represents a transformation of a partial state set.
    """
    name: str  # Of the transformation itself
    namespace: str  # Of the transformation itself
    layer: str  # Of the transformation itself
    labels: List[str] = field(default_factory=list) # Of the transformation itself
    time: Optional[float] = None #For this transformation
    elapsed_time: Optional[float] = None # So far in the envelope including this transformation
    curve: Optional[str] = None
    partial_set_1: Optional[PartialSet] = None


@dataclass
class FlattenedCartesianProductTransformation(PartialStateSetTransformation):
    """
    Defines the FCP transformation
    """
    partial_set_2: Optional[PartialSet] = None

    def transform(self) :
        pass #TODO

@dataclass
class UpdateTransformation(PartialStateSetTransformation):
    """
    Defines the Update transformation
    """
    partial_set_2: Optional[PartialSet] = None

    def transform(self) :
        pass #TODO

# ============================================================
# Data classes for envelope specification
# ============================================================

@dataclass
class EnvelopeTransform:
    """
    Represents a transform to be applied within an envelope stage.
    - filter: A regex used to select which partials (by label) are affected.
    - transformation_ref: A tuple (namespace, name) referencing the partial state set transformation.
    """
    filter: str
    transformation_ref: Tuple[str, str]
    source_partial_state_set: Optional[Tuple[str, str]] = None

@dataclass
class EnvelopeStage:
    """
    Represents one stage (transition) in an envelope.
    - time: The duration (in seconds, relative) for this stage.
    - envelope_transforms: A list of transforms for this stage.
    The stage order is determined by the list order.
    """
    time: float
    curve: str
    envelope_transforms: List[EnvelopeTransform] = field(default_factory=list)


@dataclass
class EnvelopeSpec:
    """
    Represents an envelope specification.
    Contains:
      - Common metadata (name, namespace, layer, labels).
      - A reference to the initial partial state set (as a tuple: (namespace, name)).
      - A list of envelope stages describing the evolution over time.
    In the output, the name and namespace will receive a "_derived" suffix.
    """
    hzl_version: str
    hzl_type: str
    name: str
    namespace: str
    layer: str
    labels: List[str] = field(default_factory=list)
    # Reference to the starting PartialStateSet
    initial_partial_state_ref: Tuple[str, str] = field(default_factory=tuple)
    envelope_stages: List[EnvelopeStage] = field(default_factory=list)
