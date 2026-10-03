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
// a transformation pass through most of the data model.
// ============================================================

// ============================================================
// Key paths through the data model
// Note the two types of timeline and two relationships
// HAS_MUSICAL_TIMELINE, measured in ticks of a beat
// HAS_PHYSICAL_TIMELINE, measured in physical units - seconds or samples.
//
// 1. The core structure of the model, common for both specification and precomputation / rendering
// Score 1->m SoundEntity 1->m SoundSource 1-->m PartialSet 1->m Partial
// Score is a piece of music.
// SoundEntity is a compositional concept. It could be a whole piece, a movement or section, a sound object, or part of a sound object.
// SoundSource is part of a SoundEntity with a defined spatial position. It could represent a single channel of stereo audio, or a source that moves in space.
// PartialSet is a set of partials that together create part of a sound.
// Partial is a single partial
// Note that higher levels of abstraction may be possible in the HzL language, but any such concepts flatten to this model.
//
// 2. Spatial position
// Canonical, linked to musical time.
// SoundSource -> SpatialPositionMusicalTimeline
// Precomputation / Rendering, linked to physical time.
// SoundSource -> AzimuthPhysicalTimeline
// SoundSource -> ElevationPhysicalTimeline
// SoundSource -> DistancePhysicalTimeline
//
// 3. Spatial position modulators
// Canonical, linked to musical time.
// SoundSource -> AzimuthModulatorMusicalTimeline
// SoundSource -> ElevationModulatorMusicalTimeline
// SoundSource -> DistanceModulatorMusicalTimeline
// Precomputation / Rendering, linked to physical time.
// SoundSource -> AzimuthModulatorPhysicalTimeline
// SoundSource -> ElevationModulatorPhysicalTimeline
// SoundSource -> DistanceModulatorPhysicalTimeline
// 
// 4. Nominal parameters of a partial
// Canonical, linked to beat time. Explicit relationships between Partials in a PartialSet.
// PartialSet -> PartialSetMusicalTimeline -> PartialSetMusicalTimelinePoint -> PartialSetState -> PartialState (freq / ampl / phase) <- Partial
// Precomputation / Rendering, linked to physical time.
// Partials decoupled from sets because curves do not map to the same parameters.
// Partial -> FrequencyPhysicalTimeline
// Partial -> AmplitudePhysicalTimeline
// Partial -> PhasePhysicalTimeline
//
// 5. Partial modulators
// Canonical, linked to beat time.
// PartialSet -> FrequencyModulatorMusicalTimeline
// PartialSet -> AmplitudeModulatorMusicalTimeline
// PartialSet -> PhaseModulatorMusicalTimeline
// PartialSet -> FrequencyModulatorMusicalTimeline
// Precomputation / Rendering, linked to wall clock time.
// Remain with PartialSets because they are intentionally common per PartialSet and modulate relative to the different
// nominal values of the Partials in each PartialSet.
// PartialSet -> FrequencyModulatorPhysicalTimeline
// PartialSet -> AmplitudeModulatorPhysicalTimeline
// PartialSet -> PhaseModulatorPhysicalTimeline
// PartialSet -> FrequencyModulatorPhysicalTimeline
//
// 6. Timing
// Musical timelines link to positions in a TempoMap that are integer counts of ticks.
// The number of ticks per beat depends on the TempoMap. They can also have fixed time
// offets - useful e.g. for fixed percussive attack portions of a sound.
//
// Physical timelines link to "wall clock" time in the conventional sense. They are also linked to Frame through
// FrameEntry. This allows rapid location of the correct start point when rendering a
// given Frame with random access. It is possible to generate a physial Timeline via
// a single pass through a musical Timeline.
//
// This is a different computation than the calculation of PartialIntegrableSegment,
// which can occur on-demand when rendering a Frame of audio, where it is necessary to 
// do so.
// ============================================================

// ============================================================
// CORE STRUCURE, COMMON TO THE ENTIRE MODEL
// ============================================================

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
// HAS_SOUNDENTITY: A Score consists of compositional elements
// that are defined by SoundEntities. It's a decision for users
// what level of detail is meant by each one.
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
// have the same modulators and will often have time synchronised
// changes in frequency, amplitude and phase - although this is 
// not a constraint.
// ============================================================
CREATE REL TABLE HAS_PARTIALSET (
    FROM SoundSource TO PartialSet,
    ONE_MANY
);

// ============================================================
// HAS_PARTIAL: A ParialSet is itself literally a set that
// has members. Each of those members is an individual
// partial. The partials in a PartialSet share common spatial
// position and common modulators through this model.
// ============================================================
CREATE REL TABLE HAS_PARTIAL (
    FROM PartialSet TO Partial,
    ONE_MANY
);

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
// TempoMap: Tempo maps may define directed acyclic graphs that
// together act as the framework for all timing of a Score.
//
// ticks_per_beat is an indication of how many subdivisions are
// possible in a beat. Note that if the tempo is 60 then this
// becomes subdivisions of a second, which can be useful when the
// user wishes to express a concept in physcial time e.g. in
// milliseonds or samples.
// ============================================================
CREATE NODE TABLE TempoMap (
    id              SERIAL,
    name            STRING,
    ticks_per_beat  INT32,
    PRIMARY KEY (id)
);

// ============================================================
// TempoMapPoint: An point in a tempo map determines
// the start tempo of the following segment.
//
// hold is a BOOLEAN flag that can be used to indicate a fixed
// tempo over the next tempo.
//
// If hold is false, then there is a linear tempo change to
// the next segment start tempo.
//
// ============================================================
CREATE NODE TABLE TempoMapPoint (
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
    sync_point_parent INT64, // position in parent where this map is rooted in the other TempoMap, >= 0 
    sync_point_child INT64, // offset of where this TempoMap starts, in its local TempoMap i.e. where the alignment point is. 
    MANY_MANY
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
// PartialSetMusicalTimeline: Objects representing a sequence of states
// and transformations for a particular set of partials.
//
// @canonical
// ============================================================
CREATE NODE TABLE PartialSetMusicalTimeline (
    id      SERIAL,
    PRIMARY KEY (id)
);

// ============================================================
// PartialSetMusicalTimelinePoint: An point within a PartialSetMusicalTimeline.
// Defines the state of the partials for that timeline at a given
// point in time.
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
// PartialStateSet: container for a set of related partial states.
//
// @canonical
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
//   frequency      Hz,      allowed to be outside of audio range and even be negative, but will not be rendered.
//   phase          degrees, [-180, 180]
//   phase_free_run Boolean,
//   amplitude      dB,      [-144, 0]      (-144 dB ~ -inf sentinel value)
//
// Three possible valid types of phase node in a PartialState:
// 1. phase is NOT NULL and phase_free_run is false.
// The property phase specifies the target phase at this point.
// 2. phase_free_run is true and phase is NULL.
// In this case the intention is to allow the parial to "run freely" to this
// point, simply accumulating phase without needing to hit a particular phase
// target.
// 3. phase_free_run is NULL and the phase propperty is also NULL
// This is the "default" case.
// In this case, the intention is that this node does not play a role in 
// determining phase. In particular, it may be between nodes that do play
// such a role.
//
// Three possible invalid types of phase node in a PartialState:
// 1. phase_free_run is true and phase is NOT NULL.
// 2. phase_free_run is NULL and phase is NOT NULL
// 3. phase_free_run is false and phase is NULL
// These are all integrity constraint errors.
//
//  All values are "nominal" because they can be affected by modulators
//  and in the case of frequency also by phase (phase takes priority where
//  there is a conflict).
//
// @canonical
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
// STATE_OF: Determines to which Partial a particular PartialState applies.
// This relationship maintains the identity of a partial during its lifetime.
//
// @canonical
// ============================================================
CREATE REL TABLE STATE_OF (
    FROM PartialState TO Partial,
    MANY_ONE
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
// PartialIntegrableSegment: An integrable segment of the
// phase and frequency for a parial. Note that phase modulators
// and Doppler shift compensation do not need to be applied during
// the precomputation phase. The precomputation phase is concerned
// with the source frequency and phase, from a spatial audio perspective.
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
// compensation segments. The actual phase rendered will include this
// just as it will phase modulation. This type of node is only
// concerned with the phase that arises from frequency itself.
// ============================================================
CREATE NODE TABLE PartialIntegrableSegment (
    id SERIAL,
    start_time        DOUBLE,  // in source time
    end_time          DOUBLE,  // in source time
    start_phase       DOUBLE,
    PRIMARY KEY (id)
);

// ============================================================
// PartialPhaseCompensationSegment: Specifications can include the
// possibility that partials have an exact phase at certain
// points in their timeline. This can require compensation
// to be applied, defined by a contiguous sequence
// of nodes of this type.
// ============================================================
CREATE NODE TABLE PartialPhaseCompensationSegment (
    id SERIAL,
    start_time                DOUBLE,
    end_time                  DOUBLE,
    start_phase_delta         DOUBLE,
    end_phase_delta           DOUBLE,
    PRIMARY KEY (id)
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
// Corresponds to the PartialSetMusicalTimeline in the canonical model.
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
// partcular class may only be a member of one set.
// ============================================================
CREATE REL TABLE MEMBER_OF_SET_MANY_ONE (
    // @canonical
    FROM PartialState TO PartialStateSet,
    MANY_ONE
);

// ============================================================
// SpatialPositionMusicalTimelinePoint: A point within a SpatialPositionMusicalTimeline.
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
// AmplitudePhysicalTimelinePoint: A point within an AmplitudePhysicalTimeline.
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
CREATE NODE TABLE AmplitudePhysicalTimelinePoint (
    id                          SERIAL,
    time                        DOUBLE,
    amplitude_curve_type        INT8 DEFAULT 0,
    amplitude_curve_amount      FLOAT DEFAULT 0.0,
    amplitude                   FLOAT,
    PRIMARY KEY (id)
);

// ============================================================
// FrequencyPhysicalTimelinePoint: A point within a FrequencyPhysicalTimeline.
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
CREATE NODE TABLE FrequencyPhysicalTimelinePoint (
    id                          SERIAL,
    time                        DOUBLE,
    frequency_curve_type        INT8 DEFAULT 0,
    frequency_curve_amount      FLOAT DEFAULT 0.0,
    frequency                   DOUBLE,
    PRIMARY KEY (id)
);

// ============================================================
// PhasePhysicalTimelinePoint: A point within a PhasePhysicalTimeline.
// Defines the absolute phase of a single partial for that timeline at a given
// point in time.
//
// This is always linear. It does not need to have that constraint
// from a theoretical or maths perspective, but the benefit of
// curves is unclear. The user will generally not know
// how much phase will be added / subtracted and so cannot
// make a judgement as to the effect of the curve.
// ============================================================
CREATE NODE TABLE PhasePhysicalTimelinePoint (
    id                          SERIAL,
    time                        DOUBLE,
    phase                       DOUBLE,
    PRIMARY KEY (id)
);

// ============================================================
// FrequencyModulatorPhysicalTimelinePoint: An point within a FrequencyModulatorPhysicalTimeline.
// Defines the modulator state for that timeline at a given
// point in time.
// ============================================================
CREATE NODE TABLE FrequencyModulatorPhysicalTimelinePoint (
    id  SERIAL,
    PRIMARY KEY (id)
);

// ============================================================
// AmplitudeModulatorPhysicalTimelinePoint: An point within an AmplitudeModulatorPhysicalTimeline.
// Defines the modulator state for that timeline at a given
// point in time.
// ============================================================
CREATE NODE TABLE AmplitudeModulatorPhysicalTimelinePoint (
    id  SERIAL,
    time DOUBLE,
    PRIMARY KEY (id)
);

// ============================================================
// PhaseModulatorPhysicalTimelinePoint: An point within a PhaseModulatorPhysicalTimeline.
// Defines the modulator state for that timeline at a given
// point in time.
// ============================================================
CREATE NODE TABLE PhaseModulatorPhysicalTimelinePoint (
    id  SERIAL,
    time DOUBLE,
    PRIMARY KEY (id)
);

// ============================================================
// AzimuthModulatorPhysicalTimelinePoint: An point within an AzimuthModulatorPhysicalTimeline.
// Defines the modulator state for that timeline at a given
// point in time.
// ============================================================
CREATE NODE TABLE AzimuthModulatorPhysicalTimelinePoint (
    id SERIAL,
    time DOUBLE,
    PRIMARY KEY (id)
);

// ============================================================
// ElevationModulatorPhysicalTimelinePoint: An point within an ElevationModulatorPhysicalTimeline.
// Defines the modulator state for that timeline at a given
// point in time.
// ============================================================
CREATE NODE TABLE ElevationModulatorPhysicalTimelinePoint (
    id SERIAL,
    time DOUBLE,
    PRIMARY KEY (id)
);

// ============================================================
// DistanceModulatorPhysicalTimelinePoint: An point within a DistanceModulatorPhysicalTimeline.
// Defines the modulator state for that timeline at a given
// point in time.
// ============================================================
CREATE NODE TABLE DistanceModulatorPhysicalTimelinePoint (
    id SERIAL,
    time DOUBLE,
    PRIMARY KEY (id)
);

// ============================================================
// SpatialPositionMusicalTimeline: The timeline of a SoundEntity in space
// ============================================================
CREATE NODE TABLE SpatialPositionMusicalTimeline (
    id SERIAL,
    PRIMARY KEY (id)
);

// ============================================================
// AmplitudePhysicalTimeline: The timeline of the amplitude of a Partial
// ============================================================
CREATE NODE TABLE AmplitudePhysicalTimeline (
    id SERIAL,
    PRIMARY KEY (id)
);

// ============================================================
// FrequencyPhysicalTimeline: The timeline of the frequency of a Partial
// ============================================================
CREATE NODE TABLE FrequencyPhysicalTimeline (
    id SERIAL,
    PRIMARY KEY (id)
);

// ============================================================
// PhasePhysicalTimeline: The timeline of the phase of a Partial
// ============================================================
CREATE NODE TABLE PhasePhysicalTimeline (
    id SERIAL,
    PRIMARY KEY (id)
);

// ============================================================
// PartialSegmentPhysicalTimeline: The timeline of the frequency of a Partial
// ============================================================
CREATE NODE TABLE PartialSegmentPhysicalTimeline (
    id SERIAL,
    PRIMARY KEY (id)
);

// ============================================================
// FrequencyModulatorPhysicalTimeline: The timeline of a FrequencyModulator
// ============================================================
CREATE NODE TABLE FrequencyModulatorPhysicalTimeline (
    id SERIAL,
    PRIMARY KEY (id)
);

// ============================================================
// AmplitudeModulatorPhysicalTimeline: The timeline of an AmplitudeModulator
// ============================================================
CREATE NODE TABLE AmplitudeModulatorPhysicalTimeline (
    id SERIAL,
    PRIMARY KEY (id)
);

// ============================================================
// PhaseModulatorPhysicalTimeline: The timeline of an PhaseModulator
// ============================================================
CREATE NODE TABLE PhaseModulatorPhysicalTimeline (
    id SERIAL,
    PRIMARY KEY (id)
);

// ============================================================
// AzimuthModulatorPhysicalTimeline: The timeline of an AzimuthModulator
// ============================================================
CREATE NODE TABLE AzimuthModulatorPhysicalTimeline (
    id SERIAL,
    PRIMARY KEY (id)
);

// ============================================================
// ElevationModulatorPhysicalTimeline: The timeline of an ElevationModulator
// ============================================================
CREATE NODE TABLE ElevationModulatorPhysicalTimeline (
    id SERIAL,
    PRIMARY KEY (id)
);

// ============================================================
// DistanceModulatorPhysicalTimeline: The timeline of an DistanceModulator
// ============================================================
CREATE NODE TABLE DistanceModulatorPhysicalTimeline (
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
    FROM FrequencyModulatorPhysicalTimelinePoint TO FrequencyModulatorState,
    FROM AmplitudeModulatorPhysicalTimelinePoint TO AmplitudeModulatorState,
    FROM PhaseModulatorPhysicalTimelinePoint TO PhaseModulatorState,
    FROM AzimuthModulatorPhysicalTimelinePoint TO AzimuthModulatorState,
    FROM ElevationModulatorPhysicalTimelinePoint TO ElevationModulatorState,
    FROM DistanceModulatorPhysicalTimelinePoint TO DistanceModulatorState,

    // @canonical
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
    FROM PartialSegmentPhysicalTimeline TO PartialIntegrableSegment,
    FROM PartialSegmentPhysicalTimeline TO PartialPhaseCompensationSegment,
    FROM SpatialPositionMusicalTimeline TO SpatialPositionMusicalTimelinePoint,
    FROM AmplitudePhysicalTimeline TO AmplitudePhysicalTimelinePoint,
    FROM FrequencyPhysicalTimeline TO FrequencyPhysicalTimelinePoint,
    FROM PhasePhysicalTimeline TO PhasePhysicalTimelinePoint,
    FROM FrequencyModulatorPhysicalTimeline TO FrequencyModulatorPhysicalTimelinePoint,
    FROM AmplitudeModulatorPhysicalTimeline TO AmplitudeModulatorPhysicalTimelinePoint,
    FROM PhaseModulatorPhysicalTimeline TO PhaseModulatorPhysicalTimelinePoint,
    FROM AzimuthModulatorPhysicalTimeline TO AzimuthModulatorPhysicalTimelinePoint,
    FROM ElevationModulatorPhysicalTimeline TO ElevationModulatorPhysicalTimelinePoint,
    FROM DistanceModulatorPhysicalTimeline TO DistanceModulatorPhysicalTimelinePoint,

    // @canonical
    FROM TempoMap TO TempoMapPoint,
    FROM PartialSetMusicalTimeline TO PartialSetMusicalTimelinePoint,
    FROM Score TO TempoMap, // Defines time = zero

    // Further nodes of the linked lists
    FROM PartialIntegrableSegment TO PartialIntegrableSegment,
    FROM PartialPhaseCompensationSegment TO PartialPhaseCompensationSegment,
    FROM SpatialPositionMusicalTimelinePoint TO SpatialPositionMusicalTimelinePoint,
    FROM AmplitudePhysicalTimelinePoint TO AmplitudePhysicalTimelinePoint,
    FROM FrequencyPhysicalTimelinePoint TO FrequencyPhysicalTimelinePoint,
    FROM PhasePhysicalTimelinePoint TO PhasePhysicalTimelinePoint,
    FROM FrequencyModulatorPhysicalTimelinePoint TO FrequencyModulatorPhysicalTimelinePoint,
    FROM AmplitudeModulatorPhysicalTimelinePoint TO AmplitudeModulatorPhysicalTimelinePoint,
    FROM PhaseModulatorPhysicalTimelinePoint TO PhaseModulatorPhysicalTimelinePoint,
    FROM AzimuthModulatorPhysicalTimelinePoint TO AzimuthModulatorPhysicalTimelinePoint,
    FROM ElevationModulatorPhysicalTimelinePoint TO ElevationModulatorPhysicalTimelinePoint,
    FROM DistanceModulatorPhysicalTimelinePoint TO DistanceModulatorPhysicalTimelinePoint,

    // @canonical
    FROM TempoMapPoint TO TempoMapPoint,
    FROM PartialSetMusicalTimelinePoint TO PartialSetMusicalTimelinePoint,
    ONE_ONE
);

// ============================================================
// HAS_PHYSICAL_TIMELINE: Defines the hierarchy of timelines within a
// given SoundSource that are measured in linear time, independent
// of tempo, time signature or other concepts from music.
// ============================================================
CREATE REL TABLE HAS_PHYSICAL_TIMELINE (
    // Optimisations for precomputation and rendeering
    FROM Partial TO AmplitudePhysicalTimeline,
    FROM Partial TO FrequencyPhysicalTimeline,
    FROM Partial TO PhasePhysicalTimeline,
    FROM Partial TO PartialSegmentPhysicalTimeline,
    ONE_ONE
);

// ============================================================
// HAS_MUSICAL_TIMELINE: Defines the hierarchy of timelines within a
// given SoundSource that are measured in beat time, which can
// vary with tempo, time signature or the number of "ticks" in
// a beat.
//
// @canonical
// ============================================================
CREATE REL TABLE HAS_MUSICAL_TIMELINE (
    FROM SoundSource TO SpatialPositionMusicalTimeline,
    FROM PartialSet TO PartialSetMusicalTimeline,
    ONE_ONE
);

// ============================================================
// HAS_MODULATOR_PHYSICAL_TIMELINE: Defines the hierarchy of modulator
// timelines within a given SoundSource, and so also determines which objects 
// modulators apply to.
// ============================================================
CREATE REL TABLE HAS_MODULATOR_PHYSICAL_TIMELINE (
    FROM SoundSource TO AzimuthModulatorPhysicalTimeline,
    FROM SoundSource TO ElevationModulatorPhysicalTimeline,
    FROM SoundSource TO DistanceModulatorPhysicalTimeline,
    FROM PartialSet TO FrequencyModulatorPhysicalTimeline,
    FROM PartialSet TO AmplitudeModulatorPhysicalTimeline,
    FROM PartialSet TO PhaseModulatorPhysicalTimeline,
    ONE_MANY
);

// ============================================================
// HAS_MODULATOR_MUSICAL_TIMELINE: Defines the hierarchy of modulator
// timelines within a given SoundSource, and so also determines which objects 
// modulators apply to.
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
// OPTIMISATION FOR RENDERING PERFORMANCE
// ============================================================

// ============================================================
// Optimised lookup from frame to timelines
// Rendering a frame immediately finds the Frame node.
// From Frame node via FRAME_ENTRY to FrameEntry.
// From FrameEntry via FRAME_ENTRY_PARTIAL to the partial
// in question.
// From FrameEntry via FRAME_ENTRY_PHYSICAL_TIMELINE to the timelines
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
// FRAME_ENTRY_PHYSICAL_TIMELINE: For each frame a partial is in scope for, the
// first node of each of its lists that the frame can need.
// Under Doppler this points at the node covering the latest
// source time the frame start can map to (delay zero), so the renderer walks
// backwards from it and never forward. With no distance delay
// it is already exact.
// ============================================================
CREATE REL TABLE FRAME_ENTRY_PHYSICAL_TIMELINE (
    FROM FrameEntry TO PartialIntegrableSegment,
    FROM FrameEntry TO PartialPhaseCompensationSegment,
    FROM FrameEntry TO AmplitudePhysicalTimelinePoint,
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
// 1 stereo (derived from an "M/S recording" then decoded to stero channels)
// 2 M/S (actually closer to native for this system, but less useful as typical output)
// 3 FOA A-format
// 4 FOA B-format
// ============================================================
CREATE NODE TABLE EngineConfig (
    id          SERIAL,
    frameSize   INT32, // If NULL this is equal to the sample rate.
    sampleRate  INT32,
    channelMode  INT8,
    PRIMARY KEY(id)
);


