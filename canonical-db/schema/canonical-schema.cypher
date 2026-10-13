// ============================================================
// canonical-schema.cypher
// ============================================================
// This is the schema that specifies how a score is defined
// within the user's mental model from a music
// and acoustics perspective.
//
// It is independent of computing concepts such as how the
// precomputation and rendering processes work.
//
// It is independent of sound engineering concepts
// such as sample rate, channels, virtual microphones, bit
// depth etc.
// ============================================================

// ============================================================
// Key paths through the data model:
// Timeline is in terms of
// HAS_MUSICAL_TIMELINE, measured in ticks of a beat, but also
// allowing seconds in some cases.
//
// 1. The core structure of the model
//
// Score 1->m SoundEntity 1->m SoundSource 1->m PartialSet 1->m Partial
//
// Score is a piece of music.
//
// SoundEntity is a compositional concept. It could be a whole piece
// (macro), a movement / section / phrase (meso), a note / sound object,
// or part of a sound object (micro).
//
// SoundSource is part of a SoundEntity with a defined spatial position.
// It could represents a source that may move in
// space.
//
// PartialSet is a set of partials that are logically
// related according to a compositional or sound design concept.
//
// Partial is a single partial
//
// Higher levels of abstraction may be possible in the HzL
// language, but any such concepts flatten to this model.
//
// 2. Nominal parameters of a partial
// PartialSet -> PartialSetMusicalTimeline -> PartialSetMusicalTimelinePoint -> PartialSetState -> PartialState (freq / ampl / phase) <- Partial
//
// 3. Partial modulators
// PartialSet -> FrequencyModulatorMusicalTimeline
// PartialSet -> AmplitudeModulatorMusicalTimeline
// PartialSet -> PhaseModulatorMusicalTimeline
// PartialSet -> FrequencyModulatorMusicalTimeline
//
// 4. Spatial position
// SoundSource -> SpatialPositionMusicalTimeline
//
// 5. Spatial position modulators
// SoundSource -> AzimuthModulatorMusicalTimeline
// SoundSource -> ElevationModulatorMusicalTimeline
// SoundSource -> DistanceModulatorMusicalTimeline
//
// 6. Timing
// Musical timelines link to positions in a TempoMap that are
// integer counts of ticks. The number of ticks per beat depends
// on the TempoMap. They can also have fixed time offets. These
// are useful e.g. for fixed percussive attack portions of a
// sound.
//
// 7. Morphing
// Although not yet implemented, an important concept in the
// design is that of morphing, a feature found in many
// synthesisers. For RAINBOHz, morphing will be implemented by
// defining overalapping timelines of both partial states and
// then interpolation of parameters between
// them. This interpolation will include phase compensation.
// Branching and merging considered to be a special case of
// morphing that requires certain conditions to be met at the
// start and end of the morph. Morphing could potentially be a
// feature only of higher layers of HzL.
// This is still to be determined at the time of writing.
// ============================================================

// ============================================================
// CORE STRUCURE
// ============================================================

// ============================================================
// Score: An entire piece of music to be rendered. Expected to
// be a singleton, but it is possible that some (future) use
// cases could call for multiple scores.
// ============================================================
CREATE NODE TABLE Score (
    id      SERIAL,
    name    STRING,
    PRIMARY KEY(id)
);

// ============================================================
// SoundSource: A source, with a point spatial position (albeit
// a dynamic spatial position). Can be used
// to represent one given source with a trajectory in space.
// ============================================================
CREATE NODE TABLE SoundSource (
    id  SERIAL,
    name STRING,
    PRIMARY KEY(id)
);

// ============================================================
// SoundEntity: A sound present during a particular period of
// time within a Score.
// ============================================================
CREATE NODE TABLE SoundEntity (
    id      SERIAL,
    name    STRING,
    PRIMARY KEY (id)
);

// ============================================================
// A set of Partial. A logical grouping of partials that have
// some commonality, depending upon the intent of the
// practitioner.
// ============================================================
CREATE NODE TABLE PartialSet (
    id          SERIAL,
    name        STRING,
    PRIMARY KEY (id)
);

// ============================================================
// Partial: An individual Partial. A partial may only be a
// member of one PartialSet.
// ============================================================
CREATE NODE TABLE Partial (
    id      SERIAL,
    name    STRING,
    PRIMARY KEY(id)
);

// ============================================================
// HAS_SOUNDENTITY: A Score consists of compositional elements
// that are defined by SoundEntities. It's a decision for
// practitioners at what level a SoundEntity is placed in the
// hierarchy of elements used to create a Score. It has a
// broader meaning than a "sound object" as it is commonly
// understood.
// ============================================================
CREATE REL TABLE HAS_SOUNDENTITY (
    FROM Score TO SoundEntity,
    ONE_MANY
);

// ============================================================
// HAS_SOUNDSOURCE: A SoundEntity consists of SoundSource
// elements. These are elements where all associated partials
// have the same spatial position at a given point in time.
// ============================================================
CREATE REL TABLE HAS_SOUNDSOURCE (
    FROM SoundEntity TO SoundSource,
    ONE_MANY
);

// ============================================================
// HAS_PARTIALSET: A SoundSource consists of PartialSet
// elements. These are elements where all associated partials
// have the same modulators and will often have time
// synchronised changes in frequency, amplitude and phase -
// although this is not a constraint.
// ============================================================
CREATE REL TABLE HAS_PARTIALSET (
    FROM SoundSource TO PartialSet,
    ONE_MANY
);

// ============================================================
// HAS_PARTIAL: A ParialSet is itself literally a set that has
// members. Each of those members is an individual partial. The
// partials in a PartialSet share common spatial position and
// common modulators through this model.
// ============================================================
CREATE REL TABLE HAS_PARTIAL (
    FROM PartialSet TO Partial,
    ONE_MANY
);

// ============================================================
// UNITS FOR SPECIFICATION OF A SCORE
// ============================================================
// These are units that align with the mental model of a typical
// user.
//
// ------------------------------------------------------------
// frequency, partial nominal        Hz
// ------------------------------------------------------------
// valid range [20,20000]
// Only values (after modulation) between 20Hz and 20kHz will be
// rendered. Carrier may take values that are out of range,
// including negative values. Negative values will result in
// silence. (Near) out of range values will be rolled off
// towards silence.
//
// ------------------------------------------------------------
// amplitude, partial nominal        dB
// ------------------------------------------------------------
// valid range [-144,0]
// Corresponds to dynamic range of 24-bit audio according to the
// 6dB per bit commonly used approximation.
// 0dB = maximum value that can be represented
// -144B or below = zero amplitude, will be optimised to
// silence. Values above 0dB are expected to cause clipping.
//
// ------------------------------------------------------------
// frequency of a modulator             Hz
// ------------------------------------------------------------
// valid range [0, 20]
// This system is not intended to provide frequency modulation
// as the term is understood within syntheiser design. This
// isn't an "FM synth". Frequency modulation does take
// place, but it remains in the subsonic range. "DC", 0Hz, is
// specifically allowed.
//
// ------------------------------------------------------------
// amplitude of modulator for frequency         unitless
// ------------------------------------------------------------
// No system imposed limits on range - but if very high
// frequencies result from modulation (into the MHz range) then
// behaviour is undefined.
// Modulator values are multipliers on the parameter that they
// operate on. This means that the modulating carrier results
// in a multiplication in Hz, which is musically useful as it
// corresponds to musical intervals. If there are multiple
// such modulators, then they multiply together.
//
// ------------------------------------------------------------
// amplitude of modulator for phase             degrees
// ------------------------------------------------------------
// No system imposed limits on range. It is intentionally useful
// to be able to ramp continuously through this parameter
// exactly to create the classic "phaser" effect. If there are
// multiple such modulators, then they add together.
//
// ------------------------------------------------------------
// amplitude of modulator for azimuth   degrees
// ------------------------------------------------------------
// No system imposed limits on range. It is intentionally useful
// to be able to ramp continuously through this parameter
// exactly to create rotation in space. If there are multiple
// such modulators, then they add together.
//
// ------------------------------------------------------------
// amplitude of modulator for elevation   degrees
// ------------------------------------------------------------
// Valid range [-180,180]
// Note that modulating elevation outside of this range does not
// cause a wraparound or "rotation in space" effect.
//
// ------------------------------------------------------------
// amplitude of modulator for amplitude     dB
// ------------------------------------------------------------
// valid range [-144,144]
// If there are multiple such modulators, then they add
// together.
//
// ------------------------------------------------------------
// amplitude of modulator for distance     m
// ------------------------------------------------------------
// valid range [-16384,16384]
// If there are multiple such modulators, then they add
// together.
//
// ------------------------------------------------------------
// phase                            degrees
// ------------------------------------------------------------
// valid range [-180,180]
// In degrees because the canonical model is intended to be in
// the user's domain. Most composers, musicians and even sound
// engineers will typically have a mental model in degrees, in
// as much as they may have envisaged phase other than in terms
// of concepts such as time alignment and cancellation. Linked
// to a Boolean flag indicating that the partial can "run
// freely" to a certain point in time; in this case phase is not
// explicitly set by the user but is simply the consequence of
// the partial continuing "naturally" at its given frequency.
//
// ------------------------------------------------------------
// time                             beats:ticks or seconds
// ------------------------------------------------------------
// In the canonical model, time is considered to be anchored to
// tempo maps. Each tempo map provides a certain tick resolution
// per beat. The position in ticks and beats is always expressed
// as an integer in ticks. If hihger resolution is
// required, then this can be specified in the tempo map. In
// addition, time offsets from a tick position can be specified
// in a certain number of seconds, independently of
// tempo. This allows for e.g. percussive sounds
// where the length of a sound is not affected by tempo.
//
// ------------------------------------------------------------
// azimtuth                         degrees
// ------------------------------------------------------------
// valid range [-180,180]
// In degrees because the canonical model is intended to be in
// the user's domain. Most composers, musicians and sound
// engineers will typically have a mental model in degrees.
// Follows conventions from Ambisonics / Ambix. NB This means
// that moving to the right from the listener's perspective is a
// negative change in azimuth.
// For azimuth, when modulators take values out of
// defined ranges, wraparound takes place. A linear modulator
// can create continuous rotation, which is a typical and
// useful use case. Note that this is not possible for
// elevation.
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
// In degrees because the canonical model is intended to be in
// the user's domain. Most composers, musicians and sound
// engineers will typically have a mental model in degrees.
// Follows conventions from Ambisonics / Ambix.
//
// 0 = directly in front of the listener
// 90 = directly above the listener
// -90 = directly below the listener
//
// ------------------------------------------------------------
// distance                         metres
// ------------------------------------------------------------
// valid range [1,16384]
// 1m = "the reference point" with zero delay,
// zero Doppler shift, zero attenuation.
// Note that if zero were chosen as the minimum value, it would
// firstly make attenuation computations meaningless. Secondly,
// it would make spatial audio computations meaningless. Given
// that users clearly want to have zero delay, but also want to
// be able to position the source in space, this is regarded as
// a practical choice. Choosing 16384 as a maximum corresponds
// to approx. 84dB attenuation and almost one minute of delay.
// It is hard to imagine any user working with larger values.
//
// ------------------------------------------------------------
// tempo                         BPM, beats per minute
// ------------------------------------------------------------
// valid range [1,1000]
// Intended to provide compatibility with DAWs, where the DAW
// sets the tempo. This is furthermore simply a reasonable range
// of possible tempo values.
// ============================================================

// ============================================================
// TEMPO MAPS
// ============================================================

// ============================================================
// TempoMap: Tempo maps may define directed acyclic graphs that
// together act as the framework for all timing of a Score.
//
// ticks_per_beat is an indication of how many subdivisions are
// possible in a beat. Note that if the tempo is 60 then this
// becomes subdivisions of a second, which can be useful when
// the user wishes to express a concept in physcial time e.g. in
// milliseonds.
// ============================================================
CREATE NODE TABLE TempoMap (
    id              SERIAL,
    name            STRING,
    ticks_per_beat  INT32,
    PRIMARY KEY (id)
);

// ============================================================
// TempoMapPoint: An point in a tempo map determines the start
// tempo of the following segment.
//
// hold is a BOOLEAN flag that can be used to indicate a fixed
// tempo over the following segment.
//
// If hold is false, then there is a linear tempo change to the
// next segment start tempo. Linear means that the rate of
// change of BPM is constant.
// ============================================================
CREATE NODE TABLE TempoMapPoint (
    id              SERIAL,
    position        INT64,
    start_tempo     DOUBLE, // BPM
    hold            BOOLEAN, // true means step change, false means linear
    PRIMARY KEY (id)
);

// ============================================================
// TEMPO_MAP_SYNCHRONIZATION_POINT: Defines the relationship in
// time between tempo maps. It is a relationship from child to
// parent. Where a child has more than one parent, the tempos
// between synchronization points are relative not absolute,
// with any segments either before the first synchronization
// point being scaled by the same factor as the first relative
// segment. Equally any segments after the last synchronization
// point being scaled by the same factor as the last relative
// segment.
//
// It is invalid to create synchronization points that result in
// out-of-order playback, this will be detected in the
// compilation to the rendering model (if not earlier) and
// reported as an error.
// ============================================================
CREATE REL TABLE TEMPO_MAP_SYNCHRONIZATION_POINT (
    FROM TempoMap TO TempoMap,  // child to parent
    sync_point_parent INT64, // position in parent where this map is rooted in the other TempoMap, >= 0
    sync_point_child INT64, // offset of where this TempoMap starts, in its local TempoMap i.e. where the alignment point is.
    MANY_MANY
);

// ============================================================
// Label: Defines a label that describes some quality or property
// of related concepts in the score in the user's mental model.
// ============================================================
CREATE NODE TABLE Label (
    id      SERIAL,
    name    STRING,
    PRIMARY KEY(id)
);

// ============================================================
// PartialSetMusicalTimeline: Objects representing a sequence of
// states and transformations for a particular set of partials.
// ============================================================
CREATE NODE TABLE PartialSetMusicalTimeline (
    id      SERIAL,
    PRIMARY KEY (id)
);

// ============================================================
// PartialSetMusicalTimelinePoint: An point within a
// PartialSetMusicalTimeline. Defines the state of the partials
// for that timeline at a given point in time.
//
// Curve types, INT8:
// 0 linear
// 1 step
// 2 accelerate     implemented as sin(3π/2) to sin(2π)
// 3 decelerate     implemented as sin(0) to sin(π/2)
// 4 s-curve        implemented as sin(-π/2) to sin(π/2)
//
// Curve amount, FLOAT (has no effect if type is linear)
// 0 linear
// 0.5 50/50 mix linear/curve
// 1 curve
//
// @canonical
// ============================================================
CREATE NODE TABLE PartialSetMusicalTimelinePoint (
    id                      SERIAL,
    amplitude_curve_type    INT8 DEFAULT 0,
    amplitude_curve_amount  FLOAT DEFAULT 0.0,
    frequency_curve_type    INT8 DEFAULT 0,
    frequency_curve_amount  FLOAT DEFAULT 0.0,
    PRIMARY KEY (id)
);

// ============================================================
// PartialStateSet: container for a set of related partial
// states.
//
// @canonical
// ============================================================
CREATE NODE TABLE PartialStateSet (
    id      SERIAL,
    PRIMARY KEY (id)
);

// ============================================================
// Partial state at a given point in time.
//
// Ranges (compiler-enforced):
//   frequency      Hz,      allowed to be outside of audio range and even be negative, but will then not be rendered.
//   phase          degrees, [-180, 180]
//   phase_free_run Boolean,
//   amplitude      dB,      [-144, 0]      (-144 dB ~ -inf sentinel value)
//
// Three possible valid types of phase node in a PartialState:
// 1. phase is NOT NULL and phase_free_run is false.
// The property phase specifies the target phase at this point.
// 2. phase_free_run is true and phase is NULL.
// In this case the intention is to allow the parial to "run
// freely" to this point, simply accumulating phase without
// needing to hit a particular phase target.
// 3. phase_free_run is NULL and the phase propperty is also
//    NULL
// This is the "default" case. In this case, the intention is
// that this node does not play a role in determining phase. In
// particular, it may be between nodes that do play such a role.
//
// Three possible invalid types of phase node in a PartialState:
// 1. phase_free_run is true and phase is NOT NULL.
// 2. phase_free_run is NULL and phase is NOT NULL
// 3. phase_free_run is false and phase is NULL
// These are all integrity constraint errors.
//
// All values are "nominal" because they can be affected by modulators
// and in the case of frequency also by phase (phase takes priority where
// there is a conflict).
// ============================================================
CREATE NODE TABLE PartialState (
    id              SERIAL,
    frequency       FLOAT,
    phase           FLOAT,
    phase_free_run  BOOLEAN,
    amplitude       FLOAT,
    PRIMARY KEY (id)
);

// ============================================================
// STATE_OF: Determines to which Partial a particular
// PartialState applies. This relationship maintains the
// identity of a partial during its lifetime.
//
// @canonical
// ============================================================
CREATE REL TABLE STATE_OF (
    FROM PartialState TO Partial,
    MANY_ONE
);

// ============================================================
// HAS_LABEL: Various objects can be annotated with labels that
// have a meaning to the user. This can be used to allow the
// user to select certain partials for rendering.
// ============================================================
CREATE REL TABLE HAS_LABEL (
    FROM Partial TO Label,
    FROM Score TO Label,
    MANY_MANY
);

// ============================================================
// SpatialPositionState: Spatial position state at a given point
// in time.
// All numeric ranges enforced by the DSL->canonical compiler,
// not by the database (LadybugDB has no CHECK constraints).
//
// Ranges (compiler-enforced):
//   azimuth      degrees,     [-180,180]      Anticlockwise from front
//   elevation    degrees,     [-90,90]  From horizontal plane
//   distance     metres,      [1,16384]
// ============================================================
CREATE NODE TABLE SpatialPositionState (
    id          SERIAL,
    azimuth      DOUBLE,
    elevation    DOUBLE,
    distance     FLOAT,
    PRIMARY KEY (id)
);

// ============================================================
// MEMBER_OF_SET_MANY_ONE: Defines set membership where a
// partcular class may only be a member of one set.
// ============================================================
CREATE REL TABLE MEMBER_OF_SET_MANY_ONE (
    FROM PartialState TO PartialStateSet,
    MANY_ONE
);

// ============================================================
// SpatialPositionMusicalTimelinePoint: A point within a
// SpatialPositionMusicalTimeline. Defines the spatial position
// of the related partials for that timeline at a given point in
// time.
//
// Curve types, INT8:
// 0 linear
// 1 step
// 2 accelerate     implemented as sin(3π/2) to sin(2π)
// 3 decelerate     implemented as sin(0) to sin(π/2)
// 4 s-curve        implemented as sin(-π/2) to sin(π/2)
//
// Curve amount, FLOAT (has no effect if type is linear)
// 0 linear
// 0.5 50/50 mix linear/curve
// 1 curve
//
// ============================================================
CREATE NODE TABLE SpatialPositionMusicalTimelinePoint (
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
// SpatialPositionMusicalTimeline: The timeline of a SoundEntity
// in space
// ============================================================
CREATE NODE TABLE SpatialPositionMusicalTimeline (
    id SERIAL,
    PRIMARY KEY (id)
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
// but is not subject to the same constraints, could be
// expanded.
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
// but is not subject to the same constraints, could be
// expanded.
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
// AzimuthModulatorState: The state of an AzimuthModulator
// Currently defined as equivalent to FrequencyModulatorState,
// but is not subject to the same constraints, could be
// expanded.
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
// ElevationModulatorState: The state of an ElevationModulator
// Currently defined as equivalent to FrequencyModulatorState,
// but is not subject to the same constraints, could be
// expanded.
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
// but is not subject to the same constraints, could be
// expanded.
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
// POINT_STATE: Defines the state of a given point on a
// timeline.
// ============================================================
CREATE REL TABLE POINT_STATE (
    FROM PartialSetMusicalTimelinePoint TO PartialStateSet,
    FROM SpatialPositionMusicalTimelinePoint TO SpatialPositionState,
    MANY_ONE
);

// ============================================================
// NEXT_POINT: Timelines are linked lists. This REL Defines
// those linked lists.
// ============================================================
CREATE REL TABLE NEXT_POINT (
    // Head of the linked lists
    FROM Score TO TempoMap, // Defines time = zero
    FROM TempoMap TO TempoMapPoint,
    FROM PartialSetMusicalTimeline TO PartialSetMusicalTimelinePoint,
    FROM SpatialPositionMusicalTimeline TO SpatialPositionMusicalTimelinePoint,

    // Further nodes of the linked lists
    FROM TempoMapPoint TO TempoMapPoint,
    FROM PartialSetMusicalTimelinePoint TO PartialSetMusicalTimelinePoint,
    FROM SpatialPositionMusicalTimelinePoint TO SpatialPositionMusicalTimelinePoint,
    ONE_ONE
);

// ============================================================
// HAS_MUSICAL_TIMELINE: Defines the hierarchy of timelines
// within a given SoundSource that are measured in musical time,
// which can vary with tempo, time signature or the number of
// "ticks" in a beat.
//
// @canonical
// ============================================================
CREATE REL TABLE HAS_MUSICAL_TIMELINE (
    FROM SoundSource TO SpatialPositionMusicalTimeline,
    FROM PartialSet TO PartialSetMusicalTimeline,
    ONE_ONE
);

// ============================================================
// HAS_MODULATOR_MUSICAL_TIMELINE: Defines the hierarchy of
// modulator timelines within a given SoundSource, and so also
// determines which objects modulators apply to.
// ============================================================
/* TODO
CREATE REL TABLE HAS_MODULATOR_MUSICAL_TIMELINE (
    FROM SoundSource TO AzimuthModulatorMusicalTimeline,
    FROM SoundSource TO ElevationModulatorMusicalTimeline,
    FROM SoundSource TO DistanceModulatorMusicalTimeline,
    FROM PartialSet TO FrequencyModulatorMusicalTimeline,
    FROM PartialSet TO AmplitudeModulatorMusicalTimeline,
    FROM PartialSet TO PhaseModulatorMusicalTimeline,
    ONE_MANY
);
*/

// ============================================================
// POINT_AT: Defines when a Timeline point occurs within a given
// tempo map.
//
// The offset in physical time allows for e.g. percussive sounds
// to have constant length at different tempi. This can also be
// useful for attack and release portions of other types of
// sounds. It is, after all, how envelopes work within most
// convnetional synthesisers.
//
// @canonical
// ============================================================
CREATE REL TABLE POINT_AT (
    FROM PartialSetMusicalTimelinePoint TO TempoMap,
    FROM SpatialPositionMusicalTimelinePoint TO TempoMap,
/* TODO
    FROM FrequencyModulatorMusicalTimelinePoint TO TempoMap,
    FROM AmplitudeModulatorMusicalTimelinePoint TO TempoMap,
    FROM PhaseModulatorMusicalTimelinePoint TO TempoMap,
    FROM AzimuthModulatorMusicalTimelinePoint TO TempoMap,
    FROM ElevationModulatorMusicalTimelinePoint TO TempoMap,
    FROM DistanceModulatorMusicalTimelinePoint TO TempoMap,
*/
    sync_point INT64, // position in the TempoMap in units of ticks for that timeline, >= 0
    offset_seconds FLOAT,  // offset in physical time, may be positive or negative.
    MANY_ONE
);