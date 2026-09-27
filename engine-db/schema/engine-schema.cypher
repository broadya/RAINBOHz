// ============================================================
// engine-schema.cypher
// ============================================================
// This is the actual schem for the percomputation and rendering
// engine. Unlike most of the rest of the RAINBOHz source, this
// file is human-maintained and provides additional information
// about design.
// 
// This schema can be seen as a superset or extension of the 
// canonical model, including features to efficiently perform
// the precomputation and rendering processes.
//
// As such it deals in two different sets of units for the same concepts
// at different points. It must achieve two things -
//
// 1) Be a direct physical representation of the canonical model
// This implies units that align with the mental model of he user
// such as Hz, degrees, dB, seconds etc. These are the units used
// the specify the score.
//
// 2) Be performant for the precomputation and rendering steps
// This implies units that align with the internal calculations
// such as radians per sample, radians, dB, samples etc. These
// are the units used to perform the calculations.
//
// This does imply an overhead to make the conversions. This
// overhead is relatively small given that the data models
// differ in other ways too. In particular, a time conversion
// from beats to seconds is necessary. which anyway forces
// a conversion pass through most of the data model.
// ============================================================

// ============================================================
// UNITS FOR SPECIFICATION OF A SCORE
// ============================================================
// These are units that align with the mental model of a
// typical user. These units come from the canonical model.
//
// ------------------------------------------------------------
// frequency, partial nominal        Hz
// ------------------------------------------------------------
// valid range [20,20000]
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
// "DC", 0Hz, is specifically allowed.
//
// ------------------------------------------------------------
// amplitude of modulator for frequency         unitless
// ------------------------------------------------------------
// No system imposed limits on range.
// Modulator values are multipliers on the parameter that
// they operate on. This means that the modulating carrier
// results in a multiplication in Hz, which is typically
// musically useful as it corresponds to musical intervals.
// If there are multiple such modulators, then they multiply together.
//
// ------------------------------------------------------------
// amplitude of modulator for phase     degrees
// ------------------------------------------------------------
// No system imposed limits on range. It is intentionally
// useful to be able to ramp continuously through this
// parameter exactly to create the classic "phaser" effect.
// If there are multiple such modulators, then they add together.
//
// ------------------------------------------------------------
// amplitude of modulator for azimuth, elevation   degrees
// ------------------------------------------------------------
// No system imposed limits on range. It is intentionally
// useful to be able to ramp continuously through this
// parameter exactly to create rotation in space.
// If there are multiple such modulators, then they add together.
//
// ------------------------------------------------------------
// amplitude of modulator for amplitude     dB
// ------------------------------------------------------------
// valid range [-144,144]
// If there are multiple such modulators, then they add together.
//
// ------------------------------------------------------------
// amplitude of modulator for distance     m
// ------------------------------------------------------------
// valid range [-16384,16384]
// If there are multiple such modulators, then they add together.
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
// TEMPO MAPS
// ============================================================
// Tempo maps are unaltered node and relationship
// tables from the canonical schema that are also requried in the 
// engine schema. The engine  converts from tempo maps to
// absolute time.
// ============================================================

// ============================================================
// TempoMap: Tempo maps define directed acyclic graphs that
// together acts as the framework for all timing of a Score.
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
// UNITS FOR PRECOMPUTATION AND RENDERING
// ============================================================
// These are units that align with the optimization of the 
// precomputation and rendering processes.
//
// ============================================================
// Units and limits in the precomputation and rendering schema.
//
// ------------------------------------------------------------
// frequency, partial nominal        radians per sample
// ------------------------------------------------------------
// Type is DOUBLE, as is every quantity that is integrated into
// the phase of a partial (frequency gradients, frequency modulator
// parameters). Phase is accumulated in double precision, and
// precomputation and rendering must integrate identical values.
//
// Valid range in the model, including negative values,
// is not limited. This is to allow for frequency modulation
// that passes through audio range. Frequencies outside
// of audio range will not actually be rendered.
// Rendering frequency limits (equivalent in Hz), assuming
// typical pro audio sampling rates, 44.1kHz or higher.
// MIN_RENDERING_RANGE = 10Hz (1 octave below)
// MIN_AUDIO_RANGE = 20Hz
// MAX_AUDIO_RANGE = 20kHz
// MAX_RENDERING_RANGE = min of (20kHz + Nyquist frequency)/2 or 30kHz (pragmatic),
//
// (-∞,MIN_RENDERING_RANGE) no rendering
// [MIN_RENDERING_RANGE,MAX_AUDIO_RANGE) linear roll-off to 0 amplitude
// [MIN_AUDIO_RANGE,MAX_AUDIO_RANGE] linear rendering
// (MAX_AUDIO_RANGE,MAX_RENDERING_RANGE] linear roll-off to 0 amplitude
// (MAX_RENDERING_RANGE,∞) no rendering
//
// Roll-off is intended to allow smooth pitch bend above and below audio frequency
// e.g. take a bass note and pitch shift smoothly down two octaves.
// If there were no roll-off this would generate discontinuities / clicks.
//
// ------------------------------------------------------------
// amplitude, partial nominal        dB
// ------------------------------------------------------------
// Valid range (-∞,0]
// 0dB = maximum value that can be represented
// (-∞,-144dB] = zero amplitude, will be optimised to silence.
// Corresponds to dynamic range of 24-bit audio
// according to the 6dB per bit commonly used approximation.
// Values above 0dB will be clipped to 0dB
// Rendering engines could be designed to have smooth limiting from 
// e.g. -3dB. This is an implementation choice for a rendering engine.
//
// ------------------------------------------------------------
// frequency, modulator             radians per sample
// ------------------------------------------------------------
// valid range corresponds to [0Hz,20Hz] after conversion.
// This system is not intended to provide frequency modulation in the
// synthesiser meaning of the word. This isn't an "FM synth", it is
// a synth with LFO-style modulation of frequency.
// As such, frequency modulation does take place, but it remains in the
// subsonic range.
// DC, 0Hz, is specifically allowed.
//
// ------------------------------------------------------------
// amplitude, modulator of frequency / spatial position   unitless
// ------------------------------------------------------------
// The amplitude of a modulator.
// No system imposed limits on range.
// Modulator values are multipliers on the parameter that
// they operate on. This does mean that modulating carrier
// results in a multiplication in dB (on a log scale), whereas
// modulating carrier frequency results in a multiplication in Hz
// (on a linear scale).
//
// ------------------------------------------------------------
// amplitude, modulator of amplitude   dB
// ------------------------------------------------------------
// Amplitude is expressed in dB, so modulators add instead of
// multiplying. For this reason, the unit of an amplitude modulator
// is also dB. There is a large possible range. When modulator
// arithmetic results in a value > 0dB for a given partial it will
// be clipped at 0dB by the rendering engine.
// Valid range [-144,144]
//
// ------------------------------------------------------------
// phase, wrapped                  radians
// ------------------------------------------------------------
// Valid range [-π,π]
// May take a NULL value in some cases. This has semantics that means
// "allow phase accumulation to run freely".
//
// ------------------------------------------------------------
// phase, unwrapped                radians
// ------------------------------------------------------------
// No limits on range. There are unusual situations where even
// negative values are possible.
//
// ------------------------------------------------------------
// time                             samples
// ------------------------------------------------------------
// In the rendering model, time is measured based on the
// the sample rate, and uses a mix of continuous and discrete time.
//
// In continuous time type is DOUBLE.
// In discrete time type is INT64.
//
// ------------------------------------------------------------
// azimuth                          radians
// ------------------------------------------------------------
// Valid range [-π,π] 
// Follows conventions from Ambisonics / Ambix.
//
// 0 = directly in front of the listener
// π/2 = left of the listener
// -π/2 = right of the listener
// π = behind the listener
// -π = behind the listener
//
// ------------------------------------------------------------
// elevation                        radians
// ------------------------------------------------------------
// Valid range [-π/2,π/2] 
// Follows conventions from Ambisonics / Ambix.
//
// 0 = directly in front of the listener
// π/2 = directly above the listener
// -π/2 = directly below the listener
//
// For azimuth and elevation, when modulators take values out of
// defined ranges, wraparound takes place. A linear modulator
// can create continuous rotation, which is a typical use case.
//
// ------------------------------------------------------------
// distance                         metres
// ------------------------------------------------------------
// valid range corresponds to [1,16384]
// 1m = zero delay, zero attenuation, it is the "reference point"
// for attenuation.
//
// ------------------------------------------------------------
// tempo                            not present
// ------------------------------------------------------------
// Tempo does not play a role in the rendering model.
// It is a responsibility of the transformation component to 
// transform from tempo maps to sample time.
// It is a responsibility of the 
// playback component to determine the relationship between a 
// tempo map to time in samples. This can occur by matching
// from the canonical model, or from the playback environment.
// ============================================================

// ============================================================
// 1. Precomputation of phase points
//
// Precomputation exists in source time and is
// continuous. This is because of the need to take
// Doppler shift into account. Generally stated,
// the rate at which the source is sampled is variable, depending
// on the velocity. It doesn't have a constant sample rate, from
// the perspective of the rendered audio.
//
// Segment start points can be correlated to the (receiver time) frames
// in which they will be rendered.
//
// The point in that frame must be determined to 
// high precision in continuous time in order to accurately render without
// discontinuities (clicks / glitches). It points to a time that is not
// sample-aligned.
//
// The rendering process then needs to calculate forward to the next
// sample in receiver time. It can use a binary search in continuous time
// on the segment (or in some edge cases a subsequent segment)
// in order to determine where exactly - in continuous time - rendering
// can start based on the various envelopes. This determination requires
// calculation of an additional small integration on the frequency of the 
// partial to determine the actual phase at the point of the sample.
//
// Rendering can then proceed - potentially both forwards and backwards -
// from that point in order to populate the frame.
// ============================================================

// ============================================================
// 2. Doppler shift and time
//
// Source time is time from the perspective of a sound source.
//
// Receiver time is time from the perspective of the receiver, located
// at the centre of spherical coordinates and facing forwards for this
// system.
// 
// For any given partial, there are two signals - the source signal,
// and the receiver signal. The receiver signal is considered to be a
// time-modified (and distance attenuated) source signal.
// 
// There are two time "contexts", or "perspectives", due to the possibility
// of Doppler shift - source time and receiver time.
//
// They do not form two streams of aligned samples.
// A discrete stream of source samples would not map
// directly to a discrete stream of receiver samples at the same
// sample rate. This is one reason why source segments are considered
// to be in continuous time.
// 
// Some parts of the model are in discrete time, some parts are in
// continuous time. The reasoning is described below - note that
// alternative approaches are possible.
//
// All precomputation is in the source signal domain. This results
// in, for each partial, a contiguous set of segments that can
// be integrated, to determine the accumulated phase.
//
// At rendering time, there is a mapping made based on the distance
// parameter. Binary search on the distance parameter path
// allows determination of the correct start point.
//
// The amount of source time increment - and therefore the amount
// of phase to add to the accumulator on the source side per sample
// is modified according to the velocity for that sample - the
// change in distance between that sample and the subsequent
// sample.
//
// There is a guiding principle that audio resolution is 
// based on the sample rate from the receiver time perspective. This
// means that an approach that first generates the source
// signal at the sample rate would not be sufficient. This is because
// the signal can be slowed when the source is moving towards the receiver,
// which could result in an audio signal with insufficient resolution.
// Alias frequencies could leak into the audio range due to Doppler shift
// when the source is moving away from the receiver.
//
// Increasing the sample rate at the source would work, but
// would be computationally more expensive and would involve interpolating
// every sample of the source. It would be a valid approach, in terms of 
// correctness, but it has not been chosen.
//
// The selected approach uses distance in two separate ways. This is a 
// trade-off whereby handling of distance is slightly inaccurate, but
// still arguably within the limits of quantisation at the receiver sample rate.
//
// Distance is quantised at the sample rate with linear interpolation between
// samples. This means that the resulting distance signal
// has lower bandwidth than other parameters when the source is moving away
// from the receiver.
//
// In typical cases, the frequency content of the
// distance signal will be largely sub-audio, so from the Nyquist perspective
// this is reasonable, although it can produce some aliasing of the
// distance signal from a theoretical perspective.
//
// Once this is done, the period between two distance samples has a gradient.
// This gradient determines the ratio source time : receiver time for that
// period. This is considered to have bounds [t1,t2) where t1 and t2 are the
// points in time of the two samples.
// 
// For each sample of receiver time, a corresponding amount of
// source time can be "consumed" by advancing through source time according
// to this ratio, and "consuming" subsequent samples if needed. When these
// samples are generated, source time is regarded as being continuous.
//
// When rendering a frame, which inherently takes place in receiver time,
// it is necessary to locate a point in source time that is inside the frame.
// This does not need to be the start point of the frame, although that is a
// valid approach. Another approach is to render both
// forwards and backwards from an arbitrary point in the frame.
//
// Finding this point is a question of binary search, starting from the same
// time in the source timeline, which also will be a very common case when the
// distance is the "reference point". There is a known limit at the other extreme
// defined by the maximum distance and the speed of sound. Therefore, it is a
// constrained binary search.
//
// There is no concept of a "frame" in source time.
//
// There are expected to be optimisations for the common case that no use
// is made of Doppler shift, although it is also just the first test in the 
// binary search so it will also naturally be fast.
//
// ============================================================

// ============================================================
// MODEL FOR SCORE DEFINITION
// ============================================================
// Score -> SoundEntity -> SoundSource -> PartialSet -> Partial

// ============================================================
// Score: The entire piece of music to be rendered.
// Expected to be a singleton, but it is possible that 
// some (future) use cases could call for multiple scores.
// ============================================================
CREATE NODE TABLE Score (
    id      SERIAL,
    name    STRING,
    PRIMARY KEY(id)
);

// ============================================================
// Label: Defines a label that can be selected by the user for rendering.
// ============================================================
CREATE NODE TABLE Label (
    id      SERIAL,
    name    STRING,
    PRIMARY KEY(id)
);

// ============================================================
// Frame: An individual portion of time within the Score
// considered from the receiver perspective.
// A Frame object defines the portion of time in the range
// [frame_number * FRAME_SIZE,(frame_number + 1) * FRAME_SIZE)
//
// There is no linked list for frame because of its sequential
// index. This index is also used in other places in the
// data model.
// ============================================================
CREATE NODE TABLE Frame (
    frame_number            INT32, // Allow very long pieces of music
    // Frame start time and end time are directly computable from
    // a FRAME_SIZE constant (typically equal to the sample rate).
    // As such it is not necessary to hold additional information about 
    // frames.
    // It is important that frame number is predictable. It crosses
    // component boundaries (transformation / precomputation /
    // rendering / playback).
    PRIMARY KEY(frame_number)
);

// ============================================================
// A set of Partial.
// A logical grouping of partials that may have various purposes.
// For example, this set is used to determine which partials
// modulators apply to.
// ============================================================
CREATE NODE TABLE PartialSet (
    id          SERIAL,
    name        STRING,
    PRIMARY KEY (id)
);

// ============================================================
// Partial: An individual Partial.
// A partial may only be a member of one PartialSet.
// ============================================================
CREATE NODE TABLE Partial (
    id      SERIAL,
    name    STRING,
    PRIMARY KEY(id)
);

// ============================================================
// HAS_LABEL: Various classes can be annotated with labels
// ============================================================
CREATE REL TABLE HAS_LABEL (
    FROM Partial TO Label,
    FROM Score TO Label,
    MANY_MANY
);

// ============================================================
// SpatialPositionState: Spatial position state at a given point in time.
// All numeric ranges enforced by the DSL->canonical compiler,
// not by the database (LadybugDB has no CHECK constraints).
//
// Ranges (compiler-enforced):
//   azimuth      rad,     [-π,π]      Anticlockwise from front
//   elevation    rad,     [-π/2,π/2]  From horizontal plane
//   distance     metres,  [1,16384]
// ============================================================
CREATE NODE TABLE SpatialPositionState (
    id          SERIAL,
    azimuth      FLOAT,
    elevation    FLOAT,
    distance     FLOAT,
    PRIMARY KEY (id)
);

// ============================================================
// SoundSource: A source, with a point spatial position (albeit a dynamic
// spatial position). Can be used to represent a channel e.g. one
// side of a stereo signal. Can equally be used to represent
// one given source in a complex spatial audio field.
// ============================================================
CREATE NODE TABLE SoundSource (
    id  SERIAL,
    name STRING,
    PRIMARY KEY(id)
);

// ============================================================
// SoundEntity: Objects representing a sound present during a
// particular period of time.
// ============================================================
CREATE NODE TABLE SoundEntity (
    id      SERIAL,
    name    STRING,
    PRIMARY KEY (id)
);

// ============================================================
// PARTIAL_IN_FRAME: A basic relationship to allow
// a rapid test to determine whether a partial is actually
// in scope for a given frame. This is derived by calculating
// the frame for the start and end of the lifeline of the partial
// and simply creating a contiguous series of objects.
// Note that it is accepted to perform the two Doppler calculations at
// the start and end of the partial lifetime, so this can be exact.
// ============================================================
CREATE REL TABLE PARTIAL_IN_FRAME (
    FROM Partial TO Frame,
    MANY_MANY
);

// ============================================================
// AGGREGATE_OF_ONE_MANY: General object-oriented design concept of 
// aggregation.
// ============================================================
CREATE REL TABLE AGGREGATE_OF_ONE_MANY (
    FROM Score TO SoundEntity,
    FROM SoundEntity TO SoundSource,
    FROM SoundSource TO PartialSet,
    FROM PartialSet TO Partial,
    ONE_MANY
);

// ============================================================
// PartialIntegrableSegment: An integrable segment of the
// phase and frequency for a parial. Note that phase modulators
// and Doppler shift compensation do not need to be applied during
// the precomputation phase. The precomputation phase is concerned
// with the source frequency and phase from a spatial audio perspective.
//
// It is permitted to have a segment with any length > 0.
// The range described by a segment is continuous time in the range
// [start_time, end_time)
//
// start_time     The start point in source time
// end_time       The end point in source time, equal to the value of the
//                start_time of the subsequent segment.
//
// start_phase    The initial unwrapped phase for this sample.
//
// start_phase does not include compensations necessary due to phase
// compensation regions. However, if a segment is the first phase in a 
// phase compensation region that starts with an absolute phase value,
// then it takes on that absolute phase value.
//
// Phase compensation regions result in additional phase envelopes.
// ============================================================
CREATE NODE TABLE PartialIntegrableSegment (
    id SERIAL,
    start_time        DOUBLE,  // in source time
    end_time          DOUBLE,  // in source time
    start_phase       DOUBLE,
    PRIMARY KEY (id)
);

// ============================================================
// PartialPhaseCompensationRegion: Specifications can include the
// possibility that partials have an exact phase at certain
// points in their timeline. This can require compensation
// to be applied in certain zones, which are contiguous sequences
// of PartialIntegrableSegment. It is inherent that these
// regions, where present, define boundaries of integrable segments.
// ============================================================
CREATE NODE TABLE PartialPhaseCompensationRegion (
    id SERIAL,
    start_phase         DOUBLE,
    end_phase           DOUBLE,
    PRIMARY KEY (id)
);

// ============================================================
// REGION_TO_SEGMENT: A PartialPhaseCompensationRegion has
// a contiguous sequence of PartialIntegrableSegment
// associated with it.
// ============================================================
CREATE REL TABLE REGION_TO_SEGMENT (
    FROM PartialPhaseCompensationRegion TO PartialIntegrableSegment,
    ONE_MANY
);

// ============================================================
// PartialSegmentNominalFrequency: Defines the nominal frequency
// path for the segment in question.
//
// Note that start_nominal_frequency
// must be computed including in the sum any linear and/or curve
// prior to this segment and/or including compensation due to the
// type of curve (e.g. S-curve starts at -1, so the start_nominal_frequency
// for the first sample with the curve will typically include a compensation 
// equal to curve_scaling_factor).
//
// Note that exact same algorithms, maths conventions etc. must be used
// for precomputation and for rendering.
//
// Corresponds to the PartialSetTimeline in the canonical model.
//
// start_nominal_frequency The initial frequency of the partial, radians per sample.
// linear_component        Flag to indicate there is a linear component present.
// linear_gradient         radians per sample²
// curve_component         Flag to indicate there is a curve component present. 
// curve_start_phase       Start phase of the curve component, radians.
// curve_frequency         radians per sample.
// curve_scaling_factor      unitless
// ============================================================
CREATE NODE TABLE PartialSegmentNominalFrequency (
    id SERIAL,
    start_nominal_frequency         DOUBLE,
    linear_component                BOOLEAN,
    linear_gradient                 DOUBLE,
    curve_component                 BOOLEAN,
    curve_start_phase               DOUBLE,
    curve_frequency                 DOUBLE,
    curve_scaling_factor            DOUBLE,
    PRIMARY KEY (id)
);

// ============================================================
// SEGMENT_TO_FREQUENCY_NOMINAL: A PartialIntegrableSegment includes
// a specification of the nominal frequency path during that segment.
// This is a separate node only to provide clarity in the model.
// ============================================================
CREATE REL TABLE SEGMENT_TO_FREQUENCY_NOMINAL (
    FROM PartialIntegrableSegment TO PartialSegmentNominalFrequency,
    ONE_ONE
);

// ============================================================
// PartialSegmentFrequencyModulator: Defines the path of the
// frequency modulator for a given integrable segment of a partial.
//
// Note that offset is the total offset from all modulator
// components, both linear and curve.
// ============================================================
CREATE NODE TABLE PartialSegmentFrequencyModulator (
    id              SERIAL,

    // Offset is sum of all offsets from component parts of
    // the modulator for this segment.
    offset                          DOUBLE,

    // Linear component
    linear_gradient                 DOUBLE,

    // Summed with a set of sinusoidal curve components
    // Empty array if only linear modulation is present.
    curve_components  STRUCT(
        curve_start_phase           DOUBLE,
        curve_frequency             DOUBLE,
        curve_scaling_factor        DOUBLE
    )[],

    PRIMARY KEY (id)
);

// ============================================================
// SEGMENT_TO_FREQUENCY_MODULATOR: A PartialIntegrableSegment includes
// a specification of frequency modulation path during that segment.
// A single segment for a single partial may have many modulators.
// A modulator may affect many partials, which can be used to
// reduce the work needed to compute those partials.
// ============================================================
CREATE REL TABLE SEGMENT_TO_FREQUENCY_MODULATOR (
    FROM PartialIntegrableSegment TO PartialSegmentFrequencyModulator,
    MANY_MANY
);

// ============================================================
// MEMBER_OF_SET_MANY_MANY: Defines set membership where a 
// particular class may be a member of many sets.
//
// Currently not in use.
// ============================================================
//CREATE REL TABLE MEMBER_OF_SET_MANY_MANY (
//    FROM Partial TO PartialSet,
//    MANY_MANY
//);

// ============================================================
// MEMBER_OF_SET_MANY_ONE: Defines set membership where a 
// particular class may only be a member of one set.
//
// Currently not in use.
// ============================================================
// CREATE REL TABLE MEMBER_OF_SET_MANY_ONE (
//     FROM X TO Y,
//    MANY_ONE
// );

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
    time                    DOUBLE,
    azimuth_curve_type      INT8 DEFAULT 0,
    azimuth_curve_amount    FLOAT DEFAULT 0.0,
    elevation_curve_type    INT8 DEFAULT 0,
    elevation_curve_amount  FLOAT DEFAULT 0.0,
    distance_curve_type     INT8 DEFAULT 0,
    distance_curve_amount   FLOAT DEFAULT 0.0,
    PRIMARY KEY (id)
);

// ============================================================
// AmplitudeTimelinePoint: A point within an AmplitudeTimeline.
// Defines the amplitude of a single partial for that timeline at a given
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
// amplitude     dB,   Valid range (-∞,0]
// ============================================================
CREATE NODE TABLE AmplitudeTimelinePoint (
    id                          SERIAL,
    time                        DOUBLE,
    amplitude_curve_type        INT8 DEFAULT 0,
    amplitude_curve_amount      FLOAT DEFAULT 0.0,
    amplitude                   FLOAT,
    PRIMARY KEY (id)
);

// ============================================================
// FrequencyTimelinePoint: A point within a FrequencyTimeline.
// Defines the frequency of a single partial for that timeline at a given
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
CREATE NODE TABLE FrequencyTimelinePoint (
    id                          SERIAL,
    time                        DOUBLE,
    frequency_curve_type        INT8 DEFAULT 0,
    frequency_curve_amount      FLOAT DEFAULT 0.0,
    frequency                   DOUBLE,
    PRIMARY KEY (id)
);

// ============================================================
// PhaseTimelinePoint: A point within a PhaseTimeline.
// Defines the absolute phase of a single partial for that timeline at a given
// point in time.
//
// This is always linear. It does not need to have that constraint
// from a theoretical or maths perspective, but the benefit of
// curves is unclear. The user will generally not know
// how much phase will be added / subtracted and so cannot
// make a judgement as to the effect of the curve.
// ============================================================
CREATE NODE TABLE PhaseTimelinePoint (
    id                          SERIAL,
    time                        DOUBLE,
    phase                       FLOAT,
    PRIMARY KEY (id)
);

// ============================================================
// AmplitudeModulatorTimelinePoint: An point within an AmplitudeModulatorTimeline.
// Defines the modulator state for that timeline at a given
// point in time.
// ============================================================
CREATE NODE TABLE AmplitudeModulatorTimelinePoint (
    id  SERIAL,
    time DOUBLE,
    PRIMARY KEY (id)
);

// ============================================================
// PhaseModulatorTimelinePoint: An point within a PhaseModulatorTimeline.
// Defines the modulator state for that timeline at a given
// point in time.
// ============================================================
CREATE NODE TABLE PhaseModulatorTimelinePoint (
    id  SERIAL,
    time DOUBLE,
    PRIMARY KEY (id)
);

// ============================================================
// AzimuthModulatorTimelinePoint: An point within an AzimuthModulatorTimeline.
// Defines the modulator state for that timeline at a given
// point in time.
// ============================================================
CREATE NODE TABLE AzimuthModulatorTimelinePoint (
    id SERIAL,
    time DOUBLE,
    PRIMARY KEY (id)
);

// ============================================================
// ElevationModulatorTimelinePoint: An point within an ElevationModulatorTimeline.
// Defines the modulator state for that timeline at a given
// point in time.
// ============================================================
CREATE NODE TABLE ElevationModulatorTimelinePoint (
    id SERIAL,
    time DOUBLE,
    PRIMARY KEY (id)
);

// ============================================================
// DistanceModulatorTimelinePoint: An point within a DistanceModulatorTimeline.
// Defines the modulator state for that timeline at a given
// point in time.
// ============================================================
CREATE NODE TABLE DistanceModulatorTimelinePoint (
    id SERIAL,
    time DOUBLE,
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
// AmplitudeTimeline: The timeline of the amplitude of a Partial
// ============================================================
CREATE NODE TABLE AmplitudeTimeline (
    id SERIAL,
    PRIMARY KEY (id)
);

// ============================================================
// FrequencyTimeline: The timeline of the frequency of a Partial
// ============================================================
CREATE NODE TABLE FrequencyTimeline (
    id SERIAL,
    PRIMARY KEY (id)
);

// ============================================================
// PhaseTimeline: The timeline of the phase of a Partial
// ============================================================
CREATE NODE TABLE PhaseTimeline (
    id SERIAL,
    PRIMARY KEY (id)
);

// ============================================================
// PartialSegmentTimeline: The timeline of the frequency of a Partial
// ============================================================
CREATE NODE TABLE PartialSegmentTimeline (
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
// PhaseModulatorTimeline: The timeline of an PhaseModulator
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
    FROM SpatialPositionTimelinePoint TO SpatialPositionState, // remove later
    FROM AmplitudeModulatorTimelinePoint TO AmplitudeModulatorState,
    FROM PhaseModulatorTimelinePoint TO PhaseModulatorState,
    FROM AzimuthModulatorTimelinePoint TO AzimuthModulatorState,
    FROM ElevationModulatorTimelinePoint TO ElevationModulatorState,
    FROM DistanceModulatorTimelinePoint TO DistanceModulatorState,
    MANY_ONE
);

// ============================================================
// NEXT_POINT: Timelines are linked lists. This REL Defines
// those linked lists.
// ============================================================
CREATE REL TABLE NEXT_POINT (
    // Head of the linked lists
    FROM PartialSegmentTimeline TO PartialIntegrableSegment,
    FROM SpatialPositionTimeline TO SpatialPositionTimelinePoint,
    FROM AmplitudeTimeline TO AmplitudeTimelinePoint,
    FROM FrequencyTimeline TO FrequencyTimelinePoint,
    FROM PhaseTimeline TO PhaseTimelinePoint,
    FROM AmplitudeModulatorTimeline TO AmplitudeModulatorTimelinePoint,
    FROM PhaseModulatorTimeline TO PhaseModulatorTimelinePoint,
    FROM AzimuthModulatorTimeline TO AzimuthModulatorTimelinePoint,
    FROM ElevationModulatorTimeline TO ElevationModulatorTimelinePoint,
    FROM DistanceModulatorTimeline TO DistanceModulatorTimelinePoint,

    // Further nodes of the linked lists
    FROM PartialIntegrableSegment TO PartialIntegrableSegment,
    FROM SpatialPositionTimelinePoint TO SpatialPositionTimelinePoint,
    FROM AmplitudeTimelinePoint TO AmplitudeTimelinePoint,
    FROM FrequencyTimelinePoint TO FrequencyTimelinePoint,
    FROM PhaseTimelinePoint TO PhaseTimelinePoint,
    FROM AmplitudeModulatorTimelinePoint TO AmplitudeModulatorTimelinePoint,
    FROM PhaseModulatorTimelinePoint TO PhaseModulatorTimelinePoint,
    FROM AzimuthModulatorTimelinePoint TO AzimuthModulatorTimelinePoint,
    FROM ElevationModulatorTimelinePoint TO ElevationModulatorTimelinePoint,
    FROM DistanceModulatorTimelinePoint TO DistanceModulatorTimelinePoint,
    ONE_ONE
);

// ============================================================
// HAS_TIMELINE: Defines the hierarchy of timelines within a
// given SoundSource.
// ============================================================
CREATE REL TABLE HAS_TIMELINE (
    FROM SoundSource TO SpatialPositionTimeline,
    FROM Partial TO AmplitudeTimeline,
    FROM Partial TO FrequencyTimeline,
    FROM Partial TO PhaseTimeline,
    FROM Partial TO PartialSegmentTimeline,
    ONE_ONE
);

// ============================================================
// HAS_MODULATOR_TIMELINE: Defines the hierarchy of modulator
// timelines within a given SoundSource, and so also determines which objects 
// modulators apply to.
// ============================================================
CREATE REL TABLE HAS_MODULATOR_TIMELINE (
    FROM SoundSource TO AzimuthModulatorTimeline,
    FROM SoundSource TO ElevationModulatorTimeline,
    FROM SoundSource TO DistanceModulatorTimeline,
    FROM PartialSet TO AmplitudeModulatorTimeline,
    FROM PartialSet TO PhaseModulatorTimeline,
    ONE_MANY
);

// ============================================================
// OPTIMISATION FOR RENDERING PERFORMANCE
// ============================================================

// ============================================================
// Optimised lookup from frame to timelines
// Rendering a frame immediately finds the Frame node.
// From Frame node via FRAME_ENTRY to FrameEntry.
// From FrameEntry via FRAME_ENTRY_PARTIAL to the partial
// in question.
// From FrameEntry via FRAME_ENTRY_TIMELINE to the timelines
// for that partial.
// ============================================================

// ============================================================
// FrameEntry: A junction node that exists only to allow
// for indexed lookup into timelines based on a frame number.
// This node type and those related to it has no real domain semantics,
// it is purely to provide random access within the database.
//
// Note that it is not called "FrameStart" because it is not
// guaranteed to find the start point of a frame, but it is the
// "entry" point into data structures to rapidly find the start.
// ============================================================
CREATE NODE TABLE FrameEntry (
    id              SERIAL,
    PRIMARY KEY (id)
);

// ============================================================
// FRAME_ENTRY: The relationship that allows the FrameEntry
// node to be rappidly found from a Frame.
// ============================================================
CREATE REL TABLE FRAME_ENTRY (
    FROM Frame TO FrameEntry,
    ONE_MANY
);

// ============================================================
// FRAME_ENTRY_TIMELINE: For each frame a partial is in scope for, the
// first node of each of its lists that the frame can need.
// Under Doppler this points at the node covering the latest
// source time the frame start can map to (delay zero), so the renderer walks
// backwards from it and never forward. With no distance delay
// it is already exact.
// ============================================================
CREATE REL TABLE FRAME_ENTRY_TIMELINE (
    FROM FrameEntry TO PartialIntegrableSegment,
    FROM FrameEntry TO AmplitudeTimelinePoint,
    MANY_MANY
);

// ============================================================
// FRAME_ENTRY_PARTIAL: Provides the connection to the actual
// partial.
// ============================================================
CREATE REL TABLE FRAME_ENTRY_PARTIAL (
    FROM FrameEntry TO Partial,
    MANY_ONE
);

// ============================================================
// POINT_AT: Defines when a Timeline point occurs within a 
// given tempo map.
//
// The offset in wall-clock time allows for percussive sounds to have
// constant length at different tempi. This can also be useful for
// attack and release portions of other types of sounds.
//
// @canonical
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
// PartialSetTimeline: Objects representing a sequence of states
// and transformations for a particular set of partials.
//
// @canonical
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
//
// @canonical
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
// MODEL FOR RESULT OF FREQUENCY AND PHASE PRECOMPUTATION
// ============================================================

// Rendering time azimuth

// Rendering time elevation

// Rendering time velocity (rate of change of distance)

// ============================================================
// PARAMETERS OF THE RENDERING ENGINE
// ============================================================

// ============================================================
// EngineConfig: Configuration parameters of the engine 
// itself.
// Singleton.
// Currently works with a number of fixed channel modes. This
// could expand to flexible "virtual mic" positions and types
// in the future.
//
// Channel mode, INT8:
// 0 mono
// 1 stereo (ideal back-to-back)
// 2 FOA B-format
// ============================================================
CREATE NODE TABLE EngineConfig (
    id          SERIAL,
    frameSize   INT32, // If NULL this is equal to the sample rate.
    sampleRate  INT32,
    channelMode  INT8,
    PRIMARY KEY(id)
);


