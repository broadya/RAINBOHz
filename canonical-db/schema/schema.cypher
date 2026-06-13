// ============================================================
// TempoMap: Tempo maps define a directed acyclic graph that
// acts as the framework for all timing of a Composition.
//
// A TempoMap may have more than one TempoMapTimeline.
// This is often useful to allow for a wall clock timeline to be
// synchronised to a beat time timeline. It may have other creative
// uses.
//
// These TempoMapTimelines within a single TempoMap may also
// have parent / child TEMPO_MAP_SYNCHRONIZATION_POINT relationships
// providing that the the DAG semantics of TempoMaps remain intact.
// ============================================================
CREATE NODE TABLE TempoMap (
    id              SERIAL,
    PRIMARY KEY (id)
);

// ============================================================
// TempoMapTimeline: A TempoMapTimeline references musical time,
// expressed in ticks (similar to ppqn).
//
// ticks_per_beat allows for exact fractions with any kind of rhythm, while also allowing for
//
// It is possible to
// represent wall clock time by setting BPM at 60 and
// e.g. ticks_per_beat at 1000 for millisecond accuracy.
// This also allows sample rates to be used (e.g. set ticks_per_beat at 48000),
// even though the canonical model is generally sample rate neutral.
//
// ============================================================
CREATE NODE TABLE TempoMapTimeline (
    id              SERIAL,
    ticks_per_beat  INT32,
    PRIMARY KEY (id)
);

// ============================================================
// TempoMapTimelineEvent: An event in a tempo map determines
// the start tempo of the following segment.
//
// hold is a BOOLEAN flag that can be used to indicate a fixed
// tempo over the next tempo.
//
// If hold is false, then there is a linear tempo change to
// the next segment start tempo.
//
// ============================================================
CREATE NODE TABLE TempoMapTimelineEvent (
    id              SERIAL,
    position        INT64,
    start_tempo     DOUBLE, // BPM
    hold            BOOLEAN, // true means step change, false means linear
    PRIMARY KEY (id)
);

// ============================================================
// TEMPO_MAP_SYNCHRONIZATION_POINT: Defines the relationship in time between
// tempo maps. It is a relationship from child to parent.
// Where a child has more than one parent, the tempos between synchronization points
// are relative not absolute, with any segments either before the first synchronization 
// point being scaled by the same factor as the first relative segment. Equally any segments
// after the last synchronization point being scaled by the same factor as the last relative
// segment.
//
// It is invalid to create synchronization points that result in out-of-order playback, this
// will be detected in the compilation to the rendering model (if not earlier) and
// reported as an error.
// ============================================================
CREATE REL TABLE TEMPO_MAP_SYNCHRONIZATION_POINT (
    FROM TempoMap TO TempoMap,  // child to parent
    sync_point_parent INT64, // position in parent where this map is rooted in the other TempoMap, > 0 
    sync_point_child INT64, // offset of where this TempoMap starts, in its local TempoMap i.e. where the alignment point is. 
    MANY_ONE
);

// ============================================================
// Label: A general purpose tool to allow practitioners to 
// associate code with concepts in their own mental model.
// ============================================================
CREATE NODE TABLE Label (
    id      SERIAL,
    name    STRING,
    PRIMARY KEY (id)
);

// ============================================================
// PartialStateSet: container for a set of related partial states.
// ============================================================
CREATE NODE TABLE PartialStateSet (
    id      SERIAL,
    PRIMARY KEY (id)
);

// ============================================================
// Partial state at a given point in time.
// All numeric ranges enforced by the DSL->canonical compiler,
// not by the database (LadybugDB has no CHECK constraints).
//
// Ranges (compiler-enforced):
//   frequency    Hz,      allowed to be outside of audio range and even be negative, but will not be rendered.
//   phase        rad,     (-PI, PI]
//   amplitude    dB,      [-150, 0]      (-150 dB ~ -inf sentinel value)
//   azimuth      rad,     (-PI, PI]      Anticlockwise from front
//   elevation    rad,     (-PI/2, PI/2]  From horizontal plane
//   distance     m,       [1, 300]       1m = listener-adjacent, zero delay, avoid divide by zero errors
//
//   phase may explicitly take a NULL value, other values may not be NULL.
//   A NULL value for phase means that the partial runs "free" to this state.
// ============================================================
CREATE NODE TABLE PartialState (
    id           SERIAL,   
    frequency    FLOAT,
    phase        FLOAT,
    amplitude    FLOAT,
    PRIMARY KEY (id)
);

// ============================================================
// Partial: A given partial in a particular SoundEntity.
// ============================================================
CREATE NODE TABLE Partial (
    id          SERIAL,
    PRIMARY KEY (id)
);

// ============================================================
// STATE_OF: Determines to which Partial a particular PartialState applies.
// This relationship maintains the identity of a partial during its lifetime.
// ============================================================
CREATE REL TABLE STATE_OF (
    FROM PartialState TO Partial,
    MANY_ONE
);

// ============================================================
// Spatial position state at a given point in time.
// All numeric ranges enforced by the DSL->canonical compiler,
// not by the database (LadybugDB has no CHECK constraints).
//
// Ranges (compiler-enforced):
//   azimuth      rad,     (-PI, PI]      Anticlockwise from front
//   elevation    rad,     (-PI/2, PI/2]  From horizontal plane
//   distance     m,       [1, 300]       1m = listener-adjacent, zero delay, avoid divide by zero errors
// ============================================================
CREATE NODE TABLE SpatialPositionState (
    id          SERIAL,
    azimuth      FLOAT DEFAULT 0.0,
    elevation    FLOAT DEFAULT 0.0,
    distance     FLOAT DEFAULT 1.0,
    PRIMARY KEY (id)
);

// ============================================================
// A set of Partial.
// A logical grouping of partials that may have various purposes.
// For example, this set is used to determine which partials
// modulators apply to.
// ============================================================
CREATE NODE TABLE PartialSet (
    id          SERIAL,
    PRIMARY KEY (id)
);

// ============================================================
// MEMBER_OF_SET: each Partial is a member of exactly one PartialSet.
// MANY_ONE enforces "many partials, one set" at the edge layer.
// ============================================================
CREATE REL TABLE MEMBER_OF_SET (
    FROM Partial TO PartialSet,
    FROM PartialState TO PartialStateSet,
    MANY_ONE
);

// ============================================================
// SoundEntity: Objects representing a sound present during a
// particular period of time.
// ============================================================
CREATE NODE TABLE SoundEntity (
    id      SERIAL,
    PRIMARY KEY (id)
);

// ============================================================
// PartialSetTimeline: Objects representing a sequence of states
// and transformations for a particular set of partials.
// ============================================================
CREATE NODE TABLE PartialSetTimeline (
    id      SERIAL,
    PRIMARY KEY (id)
);

// ============================================================
// PartialSetTimelineEvent: An event within a PartialSetTimeline.
// Defines the state of the partials for that timeline at a given
// point in time.
//
// Curve types, INT8:
// 0 linear
// 1 hold
// 2 accelerate
// 3 decelerate
// 4 s-curve
//
// Curve amount, FLOAT (has no effect if type is linear)
// 0 linear
// 0.5 50/50 mix linear/curve
// 1 curve
//
// phase_lock determines whether, when phase adjustment takes place, 
// the phase at the start of this event will be included in the
// adjustment.
// ============================================================
CREATE NODE TABLE PartialSetTimelineEvent (
    id                      SERIAL,
    amplitude_curve_type    INT8,
    amplitude_curve_amount  FLOAT,
    frequency_curve_type    INT8,
    frequency_curve_amount  FLOAT,
    phase_curve_type        INT8,
    phase_curve_amount      FLOAT,
    phase_lock              BOOLEAN,
    PRIMARY KEY (id)
);

// ============================================================
// SpatialPositionTimelineEvent: An event within a SpatialPositionTimeline.
// Defines the spatial position of the related partials for that timeline at a given
// point in time.
//
// Curve types, INT8:
// 0 linear
// 1 hold
// 2 accelerate
// 3 decelerate
// 4 s-curve
//
// Curve amount, FLOAT (has no effect if type is linear)
// 0 linear
// 0.5 50/50 mix linear/curve
// 1 curve
//
// ============================================================
CREATE NODE TABLE SpatialPositionTimelineEvent (
    id                      SERIAL,
    azimuth_curve_type      INT8,
    azimuth_curve_amount    FLOAT,
    elevation_curve_type    INT8,
    elevation_curve_amount  FLOAT,
    distance_curve_type     INT8,
    distance_curve_amount   FLOAT,
    PRIMARY KEY (id)
);

// ============================================================
// FrequencyModulatorTimelineEvent: An event within a FrequencyModulatorTimeline.
// Defines the modulator state for that timeline at a given
// point in time.
// ============================================================
CREATE NODE TABLE FrequencyModulatorTimelineEvent (
    id  SERIAL,
    PRIMARY KEY (id)
);

// ============================================================
// AmplitudeModulatorTimelineEvent: An event within an AmplitudeModulatorTimeline.
// Defines the modulator state for that timeline at a given
// point in time.
// ============================================================
CREATE NODE TABLE AmplitudeModulatorTimelineEvent (
    id  SERIAL,
    PRIMARY KEY (id)
);

// ============================================================
// PhaseModulatorTimelineEvent: An event within a PhaseModulatorTimeline.
// Defines the modulator state for that timeline at a given
// point in time.
// ============================================================
CREATE NODE TABLE PhaseModulatorTimelineEvent (
    id SERIAL,
    PRIMARY KEY (id)
);

// ============================================================
// AzimuthModulatorTimelineEvent: An event within an AzimuthModulatorTimeline.
// Defines the modulator state for that timeline at a given
// point in time.
// ============================================================
CREATE NODE TABLE AzimuthModulatorTimelineEvent (
    id SERIAL,
    PRIMARY KEY (id)
);

// ============================================================
// ElevationModulatorTimelineEvent: An event within an ElevationModulatorTimeline.
// Defines the modulator state for that timeline at a given
// point in time.
// ============================================================
CREATE NODE TABLE ElevationModulatorTimelineEvent (
    id SERIAL,
    PRIMARY KEY (id)
);

// ============================================================
// DistanceModulatorTimelineEvent: An event within a DistanceModulatorTimeline.
// Defines the modulator state for that timeline at a given
// point in time.
// ============================================================
CREATE NODE TABLE DistanceModulatorTimelineEvent (
    id SERIAL,
    PRIMARY KEY (id)
);

// ============================================================
// SpatialPositionTimeline: The timeline of a SoundEntity in space
// ============================================================
CREATE NODE TABLE SpatialPositionTimeline (
    id SERIAL,
    PRIMARY KEY (id)
);

// ============================================================
// FrequencyModulatorTimeline: The timeline of a FrequencyModulator
// ============================================================
CREATE NODE TABLE FrequencyModulatorTimeline (
    id SERIAL,
    PRIMARY KEY (id)
);

// ============================================================
// AmplitudeModulatorTimeline: The timeline of an AmplitudeModulator
// ============================================================
CREATE NODE TABLE AmplitudeModulatorTimeline (
    id SERIAL,
    PRIMARY KEY (id)
);

// ============================================================
// PhaseModulatorTimeline: The timeline of a PhaseModulator
// ============================================================
CREATE NODE TABLE PhaseModulatorTimeline (
    id SERIAL,
    PRIMARY KEY (id)
);

// ============================================================
// AzimuthModulatorTimeline: The timeline of an AzimuthModulator
// ============================================================
CREATE NODE TABLE AzimuthModulatorTimeline (
    id SERIAL,
    PRIMARY KEY (id)
);

// ============================================================
// ElevationModulatorTimeline: The timeline of an ElevationModulator
// ============================================================
CREATE NODE TABLE ElevationModulatorTimeline (
    id SERIAL,
    PRIMARY KEY (id)
);

// ============================================================
// DistanceModulatorTimeline: The timeline of an DistanceModulator
// ============================================================
CREATE NODE TABLE DistanceModulatorTimeline (
    id SERIAL,
    PRIMARY KEY (id)
);

// ============================================================
// NEXT_EVENT: Timelines are linked lists. This REL Defines
// those linked lists.
// ============================================================
CREATE REL TABLE NEXT_EVENT (
    // Head of the linked lists
    FROM TempoMapTimeline TO TempoMapTimelineEvent,
    FROM PartialSetTimeline TO PartialSetTimelineEvent,
    FROM SpatialPositionTimeline TO SpatialPositionTimelineEvent,
    FROM FrequencyModulatorTimeline TO FrequencyModulatorTimelineEvent,
    FROM AmplitudeModulatorTimeline TO AmplitudeModulatorTimelineEvent,
    FROM PhaseModulatorTimeline TO PhaseModulatorTimelineEvent,
    FROM AzimuthModulatorTimeline TO AzimuthModulatorTimelineEvent,
    FROM ElevationModulatorTimeline TO ElevationModulatorTimelineEvent,
    FROM DistanceModulatorTimeline TO DistanceModulatorTimelineEvent,

    // Further nodes of the linked lists
    FROM TempoMapTimelineEvent TO TempoMapTimelineEvent,
    FROM PartialSetTimelineEvent TO PartialSetTimelineEvent,
    FROM SpatialPositionTimelineEvent TO SpatialPositionTimelineEvent,
    FROM FrequencyModulatorTimelineEvent TO FrequencyModulatorTimelineEvent,
    FROM AmplitudeModulatorTimelineEvent TO AmplitudeModulatorTimelineEvent,
    FROM PhaseModulatorTimelineEvent TO PhaseModulatorTimelineEvent,
    FROM AzimuthModulatorTimelineEvent TO AzimuthModulatorTimelineEvent,
    FROM ElevationModulatorTimelineEvent TO ElevationModulatorTimelineEvent,
    FROM DistanceModulatorTimelineEvent TO DistanceModulatorTimelineEvent,
    ONE_ONE
);

// ============================================================
// EVENT_AT: Defines when a Timeline event occurs within a 
// given tempo map.
//
// The offset in wall-clock time allows for percussive sounds to have
// constant length at different tempi. This can also be useful for
// attack and release portions of other types of sounds.
// ============================================================
CREATE REL TABLE EVENT_AT (
    FROM PartialSetTimelineEvent TO TempoMapTimeline,
    FROM SpatialPositionTimelineEvent TO TempoMapTimeline,
    FROM FrequencyModulatorTimelineEvent TO TempoMapTimeline,
    FROM AmplitudeModulatorTimelineEvent TO TempoMapTimeline,
    FROM PhaseModulatorTimelineEvent TO TempoMapTimeline,
    FROM AzimuthModulatorTimelineEvent TO TempoMapTimeline,
    FROM ElevationModulatorTimelineEvent TO TempoMapTimeline,
    FROM DistanceModulatorTimelineEvent TO TempoMapTimeline,
    sync_point INT64, // position in the TempoMapTimeline in units of ticks for that timeline, > 0 
    offset_seconds FLOAT,  // offset in wall clock time.
    MANY_ONE
);

// ============================================================
// HAS_TIMELINE: Defines the hierarchy of timelines within a
// given SoundEntity, and so also determines which objects 
// modulators apply to.
// ============================================================
CREATE REL TABLE HAS_TIMELINE (
    FROM TempoMap TO TempoMapTimeline,
    FROM SoundEntity TO SpatialPositionTimeline,
    FROM SpatialPositionTimeline TO PartialSetTimeline,
    FROM SpatialPositionTimeline TO AzimuthModulatorTimeline,
    FROM SpatialPositionTimeline TO ElevationModulatorTimeline,
    FROM SpatialPositionTimeline TO DistanceModulatorTimeline,
    FROM PartialSet TO FrequencyModulatorTimeline,
    FROM PartialSet TO AmplitudeModulatorTimeline,
    FROM PartialSet TO PhaseModulatorTimeline,
    ONE_MANY
);

// ============================================================
// FrequencyModulatorState: The state of a FrequencyModulator
// ============================================================
CREATE NODE TABLE FrequencyModulatorState (
    id              SERIAL,

    // Linear component
    gradient        FLOAT,
    dc_offset       FLOAT,

    // Summed with a set of sinusoidal components
    sin_components  STRUCT(
        amplitude       FLOAT,
        frequency       FLOAT,
        phase_offset    FLOAT
    )[],

    PRIMARY KEY (id)
);

// ============================================================
// AmplitudeModulatorState: The state of an AmplitudeModulator
// Currently defined as equivalent to FrequencyModulatorState,
// but is not subject to the same constraints, could be expanded.
// ============================================================
CREATE NODE TABLE AmplitudeModulatorState (
    id              SERIAL,

    // Linear component
    gradient        FLOAT,
    dc_offset       FLOAT,

    // Summed with a set of sinusoidal components
    sin_components  STRUCT(
        amplitude       FLOAT,
        frequency       FLOAT,
        phase_offset    FLOAT
    )[],

    PRIMARY KEY (id)
);

// ============================================================
// PhaseModulatorState: The state of a PhaseModulator
// Currently defined as equivalent to FrequencyModulatorState,
// but is not subject to the same constraints, could be expanded.
// ============================================================
CREATE NODE TABLE PhaseModulatorState (
    id              SERIAL,

    // Linear component
    gradient        FLOAT,
    dc_offset       FLOAT,

    // Summed with a set of sinusoidal components
    sin_components  STRUCT(
        amplitude       FLOAT,
        frequency       FLOAT,
        phase_offset    FLOAT
    )[],

    PRIMARY KEY (id)
);

// ============================================================
// AzimuthModulatorState: The state of a AzimuthModulator
// Currently defined as equivalent to FrequencyModulatorState,
// but is not subject to the same constraints, could be expanded.
// ============================================================
CREATE NODE TABLE AzimuthModulatorState (
    id              SERIAL,

    // Linear component
    gradient        FLOAT,
    dc_offset       FLOAT,

    // Summed with a set of sinusoidal components
    sin_components  STRUCT(
        amplitude       FLOAT,
        frequency       FLOAT,
        phase_offset    FLOAT
    )[],

    PRIMARY KEY (id)
);

// ============================================================
// ElevationModulatorState: The state of a ElevationModulator
// Currently defined as equivalent to FrequencyModulatorState,
// but is not subject to the same constraints, could be expanded.
// ============================================================
CREATE NODE TABLE ElevationModulatorState (
    id              SERIAL,

    // Linear component
    gradient        FLOAT,
    dc_offset       FLOAT,

    // Summed with a set of sinusoidal components
    sin_components  STRUCT(
        amplitude       FLOAT,
        frequency       FLOAT,
        phase_offset    FLOAT
    )[],

    PRIMARY KEY (id)
);

// ============================================================
// DistanceModulatorState: The state of a DistanceModulator
// Currently defined as equivalent to FrequencyModulatorState,
// but is not subject to the same constraints, could be expanded.
// ============================================================
CREATE NODE TABLE DistanceModulatorState (
    id              SERIAL,

    // Linear component
    gradient        FLOAT,
    dc_offset       FLOAT,

    // Summed with a set of sinusoidal components
    sin_components  STRUCT(
        amplitude       FLOAT,
        frequency       FLOAT,
        phase_offset    FLOAT
    )[],

    PRIMARY KEY (id)
);

// ============================================================
// EVENT_STATE: Defines the state of a given event on a timeline.
// ============================================================
CREATE REL TABLE EVENT_STATE (
    FROM SpatialPositionTimelineEvent TO SpatialPositionState,
    FROM PartialSetTimelineEvent TO PartialStateSet,
    FROM FrequencyModulatorTimelineEvent TO FrequencyModulatorState,
    FROM AmplitudeModulatorTimelineEvent TO AmplitudeModulatorState,
    FROM PhaseModulatorTimelineEvent TO PhaseModulatorState,
    FROM AzimuthModulatorTimelineEvent TO AzimuthModulatorState,
    FROM ElevationModulatorTimelineEvent TO ElevationModulatorState,
    FROM DistanceModulatorTimelineEvent TO DistanceModulatorState,
    MANY_ONE
);

// ============================================================
// HAS_LABEL: Various types can be annotated with labels
// ============================================================
CREATE REL TABLE HAS_LABEL (
    FROM SpatialPositionTimeline TO Label,
    FROM TempoMap TO Label,
    FROM PartialSet TO Label,
    MANY_MANY
);


