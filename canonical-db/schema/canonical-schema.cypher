// ============================================================
// Units in the canonical schema.
//
// ------------------------------------------------------------
// frequency, partial nominal        Hz
// ------------------------------------------------------------
// valid range [20,2000]
// Only values (after modulation) between 20Hz and 20kHz will be rendered.
// Carrier may take values that are out of range, including negative values.
// Negative values will result in silence.
// (Near) out of range values will be rolled off towards silence.
//
// ------------------------------------------------------------
// amplitude, partial nominal        dB
// ------------------------------------------------------------
// valid range [-144,0]
// Corresponds to dynamic range of 24-bit audio
// according to the 6dB per bit commonly unsed approximation.
// 0dB = maximum value that can be represented
// -144B or below = zero amplitude, will be optimised to silence.
// Values above 0dB will be clipped to 0dB
// The reference rendering implementation sums to INT64, and so can
// accurately sum a very large number of partials while maintaining
// 24 bit resolution per partial. Other rendering implementations
// may not exhibit this behaviour, the canonical model is independent
// of any given rendering imeplemtation.
//
// ------------------------------------------------------------
// frequency, modulator             Hz
// ------------------------------------------------------------
// valid range [0, 20]
// This system is not intended to provide frequency modulation in the
// synthesiser meaning of the word. This isn't an "FM synth".
// As such, frequency modulation does take place, but it remains in the
// subsonic range.
// DC, 0Hz, is specifically allowed.
//
// ------------------------------------------------------------
// amplitude, modulator             unitless
// ------------------------------------------------------------
// no system imposed limits on range.
// Modulator values are multipliers on the parameter that
// they operate on. This does mean that modulating carrier
// results in a multiplication in dB (on a log scale), whereas
// modulating carrier frequency results in a multiplication in Hz
// (on a linear scale).
//
// ------------------------------------------------------------
// phase                            degrees
// ------------------------------------------------------------
// valid range [-180,180]
// In degrees because the canonical model is intended to be in the
// user's domain. Most composers, musicians and even sound engineers
// will typically have a mental model in degrees, in as much as they
// have deeply considered phase except for in terms of concepts such
// as time alignment and cancellation.
// May take a NULL value in some cases. This has semantics that means
// "allow phase accumulation to run freely".
//
// ------------------------------------------------------------
// time                             seconds
// ------------------------------------------------------------
// In the canonical model, time is considered to be continuous.
// In practice, there is granularty down to the floating-point
// precision limits.
//
// ------------------------------------------------------------
// azimtuth                         degrees
// ------------------------------------------------------------
// valid range [-180,180]
// In degrees because the canonical model is intended to be in the
// user's domain. Most composers, musicians and even sound engineers
// will typically have a mental model in degrees.
// Follows conventions from Ambisonics / Ambix.
//
// 0 = directly in front of the listener
// 90 = left of the listener
// -90 = right of the listener
// 180 = behind the listener
// -180 = behind the listener
//
// ------------------------------------------------------------
// elevation                        degrees
// ------------------------------------------------------------
// valid range [-90,90]
// In degrees because the canonical model is intended to be in the
// user's domain. Most composers, musicians and even sound engineers
// will typically have a mental model in degrees.
// Follows conventions from Ambisonics / Ambix.
//
// 0 = directly in front of the listener
// 90 = directly above the listener
// -90 = directly below the listener
//
// For azimuth and elevation, when modulators take values out of
// defined ranges, wraparound takes place. A linear modulator
// can create continuous rotation, which is a typical and useful
// use case.
//
// ------------------------------------------------------------
// distance                         metres
// ------------------------------------------------------------
// valid range [1,16384]
// 1m = zero delay, zero attenuation.
// Note that if zero were chosen, it would firstly make attenuation computations
// meaningless. Secondly,
// it would make spatial audio computations meaningless.
// Given that users will clearly want to have zero delay, but also want to be
// able to position the source in space, this is regarded as a practical choice.
// Choosing 16384 as a maximum corresponds to approx. 84dB attenuation and
// almost one minute of delay. It is hard to imagine any user working with
// larger values.
//
// ------------------------------------------------------------
// tempo                         BPM, beats per minute
// ------------------------------------------------------------
// valid range [1,1000]
// Intended to provide compatibility with DAWs, where the DAW sets the tempo.
// This is furthermore simply a reasonable range of possible tempo values.
// ============================================================


// ============================================================
// TempoMap: Tempo maps define a directed acyclic graph that
// acts as the framework for all timing of a Score.
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
// TempoMapTimelinePoint: An point in a tempo map determines
// the start tempo of the following segment.
//
// hold is a BOOLEAN flag that can be used to indicate a fixed
// tempo over the next tempo.
//
// If hold is false, then there is a linear tempo change to
// the next segment start tempo.
//
// ============================================================
CREATE NODE TABLE TempoMapTimelinePoint (
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
//   phase        degrees, [-180, 180]
//   amplitude    dB,      [-144, 0]      (-144 dB ~ -inf sentinel value)
//
//   phase may explicitly take a NULL value, other values may not be NULL.
//   A NULL value for phase means that the partial runs "free" to this state.
//
//  All values are "nominal" because they can be affected by modulators
//  and in the case of frequency also by phase (phase takes priority where
//  there is a conflict).
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
// SoundSource: A source, with a point spatial position (albeit a dynamic
// spatial postion). Can be used to represent a channel e.g. one
// side of a stereo signal. Can equally be used to represent
// one given source in a complex spatial audio field.
// ============================================================
CREATE NODE TABLE SoundSource (
    id  SERIAL,
    PRIMARY KEY(id)
);

// ============================================================
// SpatialPositionState: Spatial position state at a given point in time.
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
// MEMBER_OF_SET_MANY_MANY: Defines set membership where a
// particular class may be a member of many sets.
//
// Not currently in use
// ============================================================
//CREATE REL TABLE MEMBER_OF_SET_MANY_MANY (
//    FROM Partial TO PartialSet,
//    MANY_MANY
//);

// ============================================================
// MEMBER_OF_SET_MANY_ONE: Defines set membership where a
// partcular class may only be a member of one set.
// ============================================================
CREATE REL TABLE MEMBER_OF_SET_MANY_ONE (
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
// Score: A singleton. The top-level object representing the entire
// piece of music. Its relationship to a single NEXT_POINT into the
// tempo DAG indicates time = 0.
// ============================================================
CREATE NODE TABLE Score (
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
// PartialSetTimelinePoint: An point within a PartialSetTimeline.
// Defines the state of the partials for that timeline at a given
// point in time.
//
// Curve types, INT8:
// 0 linear
// 1 step
// 2 accelerate     implemented as sin(3π/2) to sin(2π)
// 3 decelerate     implemented as sin(0) to sin(π/2)
// 4 s-curve        implemented assin(-π/2) to sin(π/2)
//
// Curve amount, FLOAT (has no effect if type is linear)
// 0 linear
// 0.5 50/50 mix linear/curve
// 1 curve
//
// phase_lock determines whether, when phase adjustment takes place,
// the phase at the start of this point will be included in the
// adjustment.
// ============================================================
CREATE NODE TABLE PartialSetTimelinePoint (
    id                      SERIAL,
    amplitude_curve_type    INT8 DEFAULT 0,
    amplitude_curve_amount  FLOAT DEFAULT 0.0,
    frequency_curve_type    INT8 DEFAULT 0,
    frequency_curve_amount  FLOAT DEFAULT 0.0,
    phase_curve_type        INT8 DEFAULT 0,
    phase_curve_amount      FLOAT DEFAULT 0.0,
    phase_lock              BOOLEAN,
    PRIMARY KEY (id)
);

// ============================================================
// SpatialPositionTimelinePoint: An point within a SpatialPositionTimeline.
// Defines the spatial position of the related partials for that timeline at a given
// point in time.
//
// Curve types, INT8:
// 0 linear
// 1 step
// 2 accelerate     implemented as sin(3π/2) to sin(2π)
// 3 decelerate     implemented as sin(0) to sin(π/2)
// 4 s-curve        implemented assin(-π/2) to sin(π/2)
//
// Curve amount, FLOAT (has no effect if type is linear)
// 0 linear
// 0.5 50/50 mix linear/curve
// 1 curve
//
// ============================================================
CREATE NODE TABLE SpatialPositionTimelinePoint (
    id                      SERIAL,
    azimuth_curve_type      INT8 DEFAULT 0,
    azimuth_curve_amount    FLOAT DEFAULT 0.0,
    elevation_curve_type    INT8 DEFAULT 0,
    elevation_curve_amount  FLOAT DEFAULT 0.0,
    distance_curve_type     INT8 DEFAULT 0,
    distance_curve_amount   FLOAT DEFAULT 0.0,
    PRIMARY KEY (id)
);

// ============================================================
// FrequencyModulatorTimelinePoint: An point within a FrequencyModulatorTimeline.
// Defines the modulator state for that timeline at a given
// point in time.
// ============================================================
CREATE NODE TABLE FrequencyModulatorTimelinePoint (
    id  SERIAL,
    PRIMARY KEY (id)
);

// ============================================================
// AmplitudeModulatorTimelinePoint: An point within an AmplitudeModulatorTimeline.
// Defines the modulator state for that timeline at a given
// point in time.
// ============================================================
CREATE NODE TABLE AmplitudeModulatorTimelinePoint (
    id  SERIAL,
    PRIMARY KEY (id)
);

// ============================================================
// PhaseModulatorTimelinePoint: An point within a PhaseModulatorTimeline.
// Defines the modulator state for that timeline at a given
// point in time.
// ============================================================
CREATE NODE TABLE PhaseModulatorTimelinePoint (
    id SERIAL,
    PRIMARY KEY (id)
);

// ============================================================
// AzimuthModulatorTimelinePoint: An point within an AzimuthModulatorTimeline.
// Defines the modulator state for that timeline at a given
// point in time.
// ============================================================
CREATE NODE TABLE AzimuthModulatorTimelinePoint (
    id SERIAL,
    PRIMARY KEY (id)
);

// ============================================================
// ElevationModulatorTimelinePoint: An point within an ElevationModulatorTimeline.
// Defines the modulator state for that timeline at a given
// point in time.
// ============================================================
CREATE NODE TABLE ElevationModulatorTimelinePoint (
    id SERIAL,
    PRIMARY KEY (id)
);

// ============================================================
// DistanceModulatorTimelinePoint: An point within a DistanceModulatorTimeline.
// Defines the modulator state for that timeline at a given
// point in time.
// ============================================================
CREATE NODE TABLE DistanceModulatorTimelinePoint (
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
// NEXT_POINT: Timelines are linked lists. This REL Defines
// those linked lists.
// ============================================================
CREATE REL TABLE NEXT_POINT (
    // Head of the linked lists
    FROM TempoMapTimeline TO TempoMapTimelinePoint,
    FROM PartialSetTimeline TO PartialSetTimelinePoint,
    FROM SpatialPositionTimeline TO SpatialPositionTimelinePoint,
    FROM FrequencyModulatorTimeline TO FrequencyModulatorTimelinePoint,
    FROM AmplitudeModulatorTimeline TO AmplitudeModulatorTimelinePoint,
    FROM PhaseModulatorTimeline TO PhaseModulatorTimelinePoint,
    FROM AzimuthModulatorTimeline TO AzimuthModulatorTimelinePoint,
    FROM ElevationModulatorTimeline TO ElevationModulatorTimelinePoint,
    FROM DistanceModulatorTimeline TO DistanceModulatorTimelinePoint,
    FROM Score TO TempoMapTimelinePoint, // Defines time = zero

    // Further nodes of the linked lists
    FROM TempoMapTimelinePoint TO TempoMapTimelinePoint,
    FROM PartialSetTimelinePoint TO PartialSetTimelinePoint,
    FROM SpatialPositionTimelinePoint TO SpatialPositionTimelinePoint,
    FROM FrequencyModulatorTimelinePoint TO FrequencyModulatorTimelinePoint,
    FROM AmplitudeModulatorTimelinePoint TO AmplitudeModulatorTimelinePoint,
    FROM PhaseModulatorTimelinePoint TO PhaseModulatorTimelinePoint,
    FROM AzimuthModulatorTimelinePoint TO AzimuthModulatorTimelinePoint,
    FROM ElevationModulatorTimelinePoint TO ElevationModulatorTimelinePoint,
    FROM DistanceModulatorTimelinePoint TO DistanceModulatorTimelinePoint,
    ONE_ONE
);

// ============================================================
// POINT_AT: Defines when a Timeline point occurs within a
// given tempo map.
//
// The offset in wall-clock time allows for percussive sounds to have
// constant length at different tempi. This can also be useful for
// attack and release portions of other types of sounds.
// ============================================================
CREATE REL TABLE POINT_AT (
    FROM PartialSetTimelinePoint TO TempoMapTimeline,
    FROM SpatialPositionTimelinePoint TO TempoMapTimeline,
    FROM FrequencyModulatorTimelinePoint TO TempoMapTimeline,
    FROM AmplitudeModulatorTimelinePoint TO TempoMapTimeline,
    FROM PhaseModulatorTimelinePoint TO TempoMapTimeline,
    FROM AzimuthModulatorTimelinePoint TO TempoMapTimeline,
    FROM ElevationModulatorTimelinePoint TO TempoMapTimeline,
    FROM DistanceModulatorTimelinePoint TO TempoMapTimeline,
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
    FROM SoundSource TO SpatialPositionTimeline,
    FROM PartialSet TO PartialSetTimeline,
    FROM SoundSource TO AzimuthModulatorTimeline,
    FROM SoundSource TO ElevationModulatorTimeline,
    FROM SoundSource TO DistanceModulatorTimeline,
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

    // Sum of all offsets from all components of the modulator.
    offset          FLOAT,

    // Linear component
    linear_gradient        FLOAT,

    // Summed with a set of sinusoidal curve components
    curve_components  STRUCT(
        curve_scaling_factor  FLOAT,
        curve_start_phase     FLOAT,
        curve_frequency       FLOAT
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

    // Sum of all offsets from all components of the modulator.
    offset          FLOAT,

    // Linear component
    linear_gradient        FLOAT,

    // Summed with a set of sinusoidal curve components
    curve_components  STRUCT(
        curve_scaling_factor  FLOAT,
        curve_start_phase     FLOAT,
        curve_frequency       FLOAT
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

    // Sum of all offsets from all components of the modulator.
    offset          FLOAT,

    // Linear component
    linear_gradient        FLOAT,

    // Summed with a set of sinusoidal curve components
    curve_components  STRUCT(
        curve_scaling_factor  FLOAT,
        curve_start_phase     FLOAT,
        curve_frequency       FLOAT
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

    // Sum of all offsets from all components of the modulator.
    offset          FLOAT,

    // Linear component
    linear_gradient        FLOAT,

    // Summed with a set of sinusoidal curve components
    curve_components  STRUCT(
        curve_scaling_factor  FLOAT,
        curve_start_phase     FLOAT,
        curve_frequency       FLOAT
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

    // Sum of all offsets from all components of the modulator.
    offset          FLOAT,

    // Linear component
    linear_gradient        FLOAT,

    // Summed with a set of sinusoidal curve components
    curve_components  STRUCT(
        curve_scaling_factor  FLOAT,
        curve_start_phase     FLOAT,
        curve_frequency       FLOAT
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

    // Sum of all offsets from all components of the modulator.
    offset          FLOAT,

    // Linear component
    linear_gradient        FLOAT,

    // Summed with a set of sinusoidal curve components
    curve_components  STRUCT(
        curve_scaling_factor  FLOAT,
        curve_start_phase     FLOAT,
        curve_frequency       FLOAT
    )[],

    PRIMARY KEY (id)
);

// ============================================================
// POINT_STATE: Defines the state of a given point on a timeline.
// ============================================================
CREATE REL TABLE POINT_STATE (
    FROM SpatialPositionTimelinePoint TO SpatialPositionState,
    FROM PartialSetTimelinePoint TO PartialStateSet,
    FROM FrequencyModulatorTimelinePoint TO FrequencyModulatorState,
    FROM AmplitudeModulatorTimelinePoint TO AmplitudeModulatorState,
    FROM PhaseModulatorTimelinePoint TO PhaseModulatorState,
    FROM AzimuthModulatorTimelinePoint TO AzimuthModulatorState,
    FROM ElevationModulatorTimelinePoint TO ElevationModulatorState,
    FROM DistanceModulatorTimelinePoint TO DistanceModulatorState,
    MANY_ONE
);

// ============================================================
// HAS_LABEL: Various classes can be annotated with labels
// ============================================================
CREATE REL TABLE HAS_LABEL (
    FROM SoundSource TO Label,
    FROM TempoMap TO Label,
    FROM PartialSet TO Label,
    FROM SoundEntity TO Label,
    MANY_MANY
);

// ============================================================
// AGGREGATE_OF_MANY_MANY: General object-oriented design concept of
// aggregation, with many-to-many.
// Currently empty
// ============================================================
// CREATE REL TABLE AGGREGATE_OF_MANY_MANY (
//    FROM X TO Y,
//    MANY_MANY
// );

// ============================================================
// AGGREGATE_OF_MANY_MANY: General object-oriented design concept of
// aggregation, with one-to-many
// ============================================================
CREATE REL TABLE AGGREGATE_OF_ONE_MANY (
    FROM Score TO SoundEntity,
    FROM SoundEntity TO SoundSource,
    FROM SoundSource TO PartialSet,
    FROM PartialSet TO Partial,
    ONE_MANY
);




