// ============================================================
// tempo-map-score.cypher
//
// A small score in the canonical model: musical time on a tempo
// map, practitioner-facing units. Nothing here is in samples or
// radians; the transform stage (rendering-engine/docs/transform.md)
// converts it before precomputation. Mirrored for the oracle in
// tempo-map-score.json, which holds the physical values the
// transform must produce; keep the two in step.
//
// The tempo map has 960 ticks per beat and two points: 120 BPM at
// tick 0 ramping to 60 BPM at tick 3840 (four beats), which then
// holds. The Score leaves tempo_ramp_physical_time NULL, so the
// ramp is linear in physical time, the default (ADR 0013): beats
// 0..4 take four beats at the mean tempo of 90, 8/3 = 2.6667 s,
// and every beat after that takes one second.
// tempo-map-position-score.cypher is the same score with the ramp
// linear in musical position.
//
// One partial set, "Chord", with two partials, and a timeline of
// four points at beats 0, 2, 4 and 8. Partial A has a state at
// every point. Partial B has no state at beat 2 and a NULL
// frequency at beat 4, so its frequency envelope has two envelope nodes and
// its amplitude envelope three: a property with no value at a point
// simply continues to the next point where it has one.
// ============================================================

CREATE (engineConfig:EngineConfig {sampleRate: 48000, channelMode: 0});

CREATE (score:Score {name: "Tempo Map Score"})
CREATE (tempoMap:TempoMap {name: "Main", ticks_per_beat: 960})
CREATE (score)-[:NEXT_POINT]->(tempoMap)
CREATE (tempoMap)-[:NEXT_POINT]->(:TempoMapPoint {position: 0, start_tempo: 120.0, hold: false})
       -[:NEXT_POINT]->(:TempoMapPoint {position: 3840, start_tempo: 60.0, hold: true})

CREATE (score)-[:HAS_SOUNDENTITY]->(entity:SoundEntity {name: "SoundEntity 0"})
CREATE (entity)-[:HAS_SOUNDSOURCE]->(source:SoundSource {name: "SoundSource 0"})
CREATE (source)-[:HAS_PARTIALSET]->(chord:PartialSet {name: "Chord"})
CREATE (chord)-[:HAS_PARTIAL]->(a:Partial {name: "A"})
CREATE (chord)-[:HAS_PARTIAL]->(b:Partial {name: "B"})

// The timeline: four points, every one on the tempo map with no offset.
CREATE (chord)-[:HAS_MUSICAL_TIMELINE]->(timeline:PartialSetMusicalTimeline)
CREATE (timeline)-[:NEXT_POINT]->(p0:PartialSetMusicalTimelinePoint)
       -[:NEXT_POINT]->(p1:PartialSetMusicalTimelinePoint)
       -[:NEXT_POINT]->(p2:PartialSetMusicalTimelinePoint)
       -[:NEXT_POINT]->(p3:PartialSetMusicalTimelinePoint)
CREATE (p0)-[:POINT_AT {sync_point: 0, offset_seconds: 0.0}]->(tempoMap)
CREATE (p1)-[:POINT_AT {sync_point: 1920, offset_seconds: 0.0}]->(tempoMap)
CREATE (p2)-[:POINT_AT {sync_point: 3840, offset_seconds: 0.0}]->(tempoMap)
CREATE (p3)-[:POINT_AT {sync_point: 7680, offset_seconds: 0.0}]->(tempoMap)

// Beat 0: A at 440 Hz, B at 660 Hz, both at -6 dB.
CREATE (p0)-[:POINT_STATE]->(s0:PartialStateSet)
CREATE (a0:PartialState {frequency: 440.0, amplitude: -6.0})-[:MEMBER_OF_SET_MANY_ONE]->(s0)
CREATE (a0)-[:STATE_OF]->(a)
CREATE (b0:PartialState {frequency: 660.0, amplitude: -6.0})-[:MEMBER_OF_SET_MANY_ONE]->(s0)
CREATE (b0)-[:STATE_OF]->(b)

// Beat 2: A holds; B has no state here.
CREATE (p1)-[:POINT_STATE]->(s1:PartialStateSet)
CREATE (a1:PartialState {frequency: 440.0, amplitude: -6.0})-[:MEMBER_OF_SET_MANY_ONE]->(s1)
CREATE (a1)-[:STATE_OF]->(a)

// Beat 4: A drops an octave; B's amplitude falls, its frequency is NULL.
CREATE (p2)-[:POINT_STATE]->(s2:PartialStateSet)
CREATE (a2:PartialState {frequency: 220.0, amplitude: -12.0})-[:MEMBER_OF_SET_MANY_ONE]->(s2)
CREATE (a2)-[:STATE_OF]->(a)
CREATE (b2:PartialState {amplitude: -24.0})-[:MEMBER_OF_SET_MANY_ONE]->(s2)
CREATE (b2)-[:STATE_OF]->(b)

// Beat 8: both end, faded to silence; B has glided down to 330 Hz.
CREATE (p3)-[:POINT_STATE]->(s3:PartialStateSet)
CREATE (a3:PartialState {frequency: 220.0, amplitude: -144.0})-[:MEMBER_OF_SET_MANY_ONE]->(s3)
CREATE (a3)-[:STATE_OF]->(a)
CREATE (b3:PartialState {frequency: 330.0, amplitude: -144.0})-[:MEMBER_OF_SET_MANY_ONE]->(s3)
CREATE (b3)-[:STATE_OF]->(b)

// The sound source's position, in the canonical model: straight ahead, level
// and 1 m away, stated once at tick 0 (degrees and metres). A position holds
// after its timeline's last point, so one point covers the whole score. There
// the mono gain is exactly 1, so mono output is unchanged by the spatial
// stage.
CREATE (source)-[:HAS_MUSICAL_TIMELINE]->(:SpatialPositionMusicalTimeline)
       -[:NEXT_POINT]->(position:SpatialPositionMusicalTimelinePoint)
       -[:POINT_STATE]->(:SpatialPositionState {azimuth: 0.0, elevation: 0.0, distance: 1.0})
CREATE (position)-[:POINT_AT {sync_point: 0, offset_seconds: 0.0}]->(tempoMap);
