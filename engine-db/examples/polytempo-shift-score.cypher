// ============================================================
// polytempo-shift-score.cypher
//
// Two tempo maps in one Score, the simplest synchronisation: a
// child map with one synchronisation point, which only shifts
// it in time (rendering-engine/docs/polytempo.md section 5.2).
// Mirrored for the oracle in polytempo-shift-score.json, which
// holds the physical values the transform must produce; keep
// the two in step.
//
// The Score's map "Main" holds 120 BPM, 960 ticks per beat. The
// child map "Voice" holds 90 BPM, and its tick 0 is tied to tick
// 7680 of Main, beat 8, which is 4 s. So Voice plays at its own
// 90 BPM from 4 s: its beat 3 falls at 6 s.
//
// Two partial sets under one source: "Chord" keeps time by Main
// (beats 0 and 4, so 0 s and 2 s), "Voice" by the child map
// (beats 0 and 3, so 4 s and 6 s). A POINT_AT may lead to any
// map of the Score's graph, not only the Score's own map.
// ============================================================

CREATE (engineConfig:EngineConfig {sampleRate: 48000, channelMode: 0});

CREATE (score:Score {name: "Polytempo Shift Score"})
CREATE (main:TempoMap {name: "Main", ticks_per_beat: 960})
CREATE (score)-[:NEXT_POINT]->(main)
CREATE (main)-[:NEXT_POINT]->(:TempoMapPoint {position: 0, start_tempo: 120.0, hold: true})

// The child map, and the edge that synchronises it: from child to parent,
// child tick 0 at parent tick 7680.
CREATE (voice:TempoMap {name: "Voice", ticks_per_beat: 960})
CREATE (voice)-[:NEXT_POINT]->(:TempoMapPoint {position: 0, start_tempo: 90.0, hold: true})
CREATE (voice)-[:TEMPO_MAP_SYNCHRONIZATION_POINT {sync_point_parent: 7680, sync_point_child: 0}]->(main)

CREATE (score)-[:HAS_SOUNDENTITY]->(entity:SoundEntity {name: "SoundEntity 0"})
CREATE (entity)-[:HAS_SOUNDSOURCE]->(source:SoundSource {name: "SoundSource 0"})

// "Chord" on Main: one partial, 440 Hz, fading from -6 dB to -12 dB over
// beats 0 to 4.
CREATE (source)-[:HAS_PARTIALSET]->(chord:PartialSet {name: "Chord"})
CREATE (chord)-[:HAS_PARTIAL]->(a:Partial {name: "A"})
CREATE (chord)-[:HAS_MUSICAL_TIMELINE]->(chordTimeline:PartialSetMusicalTimeline)
CREATE (chordTimeline)-[:NEXT_POINT]->(c0:PartialSetMusicalTimelinePoint)
       -[:NEXT_POINT]->(c1:PartialSetMusicalTimelinePoint)
CREATE (c0)-[:POINT_AT {sync_point: 0, offset_seconds: 0.0}]->(main)
CREATE (c1)-[:POINT_AT {sync_point: 3840, offset_seconds: 0.0}]->(main)
CREATE (c0)-[:POINT_STATE]->(cs0:PartialStateSet)
CREATE (a0:PartialState {frequency: 440.0, amplitude: -6.0})-[:MEMBER_OF_SET_MANY_ONE]->(cs0)
CREATE (a0)-[:STATE_OF]->(a)
CREATE (c1)-[:POINT_STATE]->(cs1:PartialStateSet)
CREATE (a1:PartialState {frequency: 440.0, amplitude: -12.0})-[:MEMBER_OF_SET_MANY_ONE]->(cs1)
CREATE (a1)-[:STATE_OF]->(a)

// "Voice" on the child map: one partial, 330 Hz, fading from -6 dB to
// -12 dB over the child's beats 0 to 3 (ticks 0 to 2880).
CREATE (source)-[:HAS_PARTIALSET]->(voiceSet:PartialSet {name: "Voice"})
CREATE (voiceSet)-[:HAS_PARTIAL]->(v:Partial {name: "V"})
CREATE (voiceSet)-[:HAS_MUSICAL_TIMELINE]->(voiceTimeline:PartialSetMusicalTimeline)
CREATE (voiceTimeline)-[:NEXT_POINT]->(v0:PartialSetMusicalTimelinePoint)
       -[:NEXT_POINT]->(v1:PartialSetMusicalTimelinePoint)
CREATE (v0)-[:POINT_AT {sync_point: 0, offset_seconds: 0.0}]->(voice)
CREATE (v1)-[:POINT_AT {sync_point: 2880, offset_seconds: 0.0}]->(voice)
CREATE (v0)-[:POINT_STATE]->(vs0:PartialStateSet)
CREATE (vv0:PartialState {frequency: 330.0, amplitude: -6.0})-[:MEMBER_OF_SET_MANY_ONE]->(vs0)
CREATE (vv0)-[:STATE_OF]->(v)
CREATE (v1)-[:POINT_STATE]->(vs1:PartialStateSet)
CREATE (vv1:PartialState {frequency: 330.0, amplitude: -12.0})-[:MEMBER_OF_SET_MANY_ONE]->(vs1)
CREATE (vv1)-[:STATE_OF]->(v)

// The sound source's position: straight ahead, level and 1 m away, stated
// once at tick 0 of Main. There the mono gain is exactly 1.
CREATE (source)-[:HAS_MUSICAL_TIMELINE]->(:SpatialPositionMusicalTimeline)
       -[:NEXT_POINT]->(position:SpatialPositionMusicalTimelinePoint)
       -[:POINT_STATE]->(:SpatialPositionState {azimuth: 0.0, elevation: 0.0, distance: 1.0})
CREATE (position)-[:POINT_AT {sync_point: 0, offset_seconds: 0.0}]->(main);
