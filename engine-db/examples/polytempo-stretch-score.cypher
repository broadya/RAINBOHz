// ============================================================
// polytempo-stretch-score.cypher
//
// A child tempo map with three synchronisation points, so two
// regions stretched by different factors, compensated where the
// ramps meet (rendering-engine/docs/polytempo.md sections 5.4
// and 5.6). Mirrored for the oracle in
// polytempo-stretch-score.json, which holds the physical values
// the transform must produce; keep the two in step.
//
// The Score's map "Main" holds 120 BPM, 960 ticks per beat. The
// child map "Voice" is written flat at 120 BPM, but with ramps
// (hold false) at beats 0, 2, 4 and 6 and a hold from beat 8.
// Its beats 0, 4 and 8 are tied to beats 0, 4 and 6 of Main: to
// 0 s, 2 s and 3 s. Nominally each region takes 2 s, so the
// first fits (k = 1) and the second must play twice as fast
// (k = 2): uncompensated, a step from 120 to 240 BPM at beat 4.
//
// Ramps meet at beat 4, so the shared tempo there is the
// geometric mean 120 * sqrt(1 * 2) = 169.71 BPM, and each region
// is re-solved to hit its anchor exactly: region 1 plays flat at
// 106.19 BPM to beat 2 then ramps to 169.71 at beat 4 (2.0 s);
// region 2 ramps from 169.71 to 266.66 BPM at beat 6 then holds
// it to beat 8 (3.0 s). After the last synchronisation point the
// map is nominal, so beat 10 is one second after beat 8.
//
// One partial set on the child map, with points at beats 0, 2,
// 4, 6, 8 and 10, lands at 0, 1.1301, 2.0, 2.5500, 3.0 and 4.0 s.
// Ramps are linear in physical time, the default.
// ============================================================

CREATE (engineConfig:EngineConfig {sampleRate: 48000, channelMode: 0});

CREATE (score:Score {name: "Polytempo Stretch Score"})
CREATE (main:TempoMap {name: "Main", ticks_per_beat: 960})
CREATE (score)-[:NEXT_POINT]->(main)
CREATE (main)-[:NEXT_POINT]->(:TempoMapPoint {position: 0, start_tempo: 120.0, hold: true})

// The child map: flat at 120 with ramps, so that compensation may bend it.
CREATE (voice:TempoMap {name: "Voice", ticks_per_beat: 960})
CREATE (voice)-[:NEXT_POINT]->(:TempoMapPoint {position: 0, start_tempo: 120.0, hold: false})
       -[:NEXT_POINT]->(:TempoMapPoint {position: 1920, start_tempo: 120.0, hold: false})
       -[:NEXT_POINT]->(:TempoMapPoint {position: 3840, start_tempo: 120.0, hold: false})
       -[:NEXT_POINT]->(:TempoMapPoint {position: 5760, start_tempo: 120.0, hold: false})
       -[:NEXT_POINT]->(:TempoMapPoint {position: 7680, start_tempo: 120.0, hold: true})

// Three synchronisation points, each from the child to the parent.
CREATE (voice)-[:TEMPO_MAP_SYNCHRONIZATION_POINT {sync_point_parent: 0, sync_point_child: 0}]->(main)
CREATE (voice)-[:TEMPO_MAP_SYNCHRONIZATION_POINT {sync_point_parent: 3840, sync_point_child: 3840}]->(main)
CREATE (voice)-[:TEMPO_MAP_SYNCHRONIZATION_POINT {sync_point_parent: 5760, sync_point_child: 7680}]->(main)

CREATE (score)-[:HAS_SOUNDENTITY]->(entity:SoundEntity {name: "SoundEntity 0"})
CREATE (entity)-[:HAS_SOUNDSOURCE]->(source:SoundSource {name: "SoundSource 0"})

// One partial on the child map: a fifth up at beat 4 and back at beat 8,
// fading by 3 dB a point, then to silence at beat 10.
CREATE (source)-[:HAS_PARTIALSET]->(set:PartialSet {name: "Voice"})
CREATE (set)-[:HAS_PARTIAL]->(a:Partial {name: "A"})
CREATE (set)-[:HAS_MUSICAL_TIMELINE]->(timeline:PartialSetMusicalTimeline)
CREATE (timeline)-[:NEXT_POINT]->(p0:PartialSetMusicalTimelinePoint)
       -[:NEXT_POINT]->(p1:PartialSetMusicalTimelinePoint)
       -[:NEXT_POINT]->(p2:PartialSetMusicalTimelinePoint)
       -[:NEXT_POINT]->(p3:PartialSetMusicalTimelinePoint)
       -[:NEXT_POINT]->(p4:PartialSetMusicalTimelinePoint)
       -[:NEXT_POINT]->(p5:PartialSetMusicalTimelinePoint)
CREATE (p0)-[:POINT_AT {sync_point: 0, offset_seconds: 0.0}]->(voice)
CREATE (p1)-[:POINT_AT {sync_point: 1920, offset_seconds: 0.0}]->(voice)
CREATE (p2)-[:POINT_AT {sync_point: 3840, offset_seconds: 0.0}]->(voice)
CREATE (p3)-[:POINT_AT {sync_point: 5760, offset_seconds: 0.0}]->(voice)
CREATE (p4)-[:POINT_AT {sync_point: 7680, offset_seconds: 0.0}]->(voice)
CREATE (p5)-[:POINT_AT {sync_point: 9600, offset_seconds: 0.0}]->(voice)

CREATE (p0)-[:POINT_STATE]->(s0:PartialStateSet)
CREATE (a0:PartialState {frequency: 440.0, amplitude: -6.0})-[:MEMBER_OF_SET_MANY_ONE]->(s0)
CREATE (a0)-[:STATE_OF]->(a)
CREATE (p1)-[:POINT_STATE]->(s1:PartialStateSet)
CREATE (a1:PartialState {frequency: 440.0, amplitude: -9.0})-[:MEMBER_OF_SET_MANY_ONE]->(s1)
CREATE (a1)-[:STATE_OF]->(a)
CREATE (p2)-[:POINT_STATE]->(s2:PartialStateSet)
CREATE (a2:PartialState {frequency: 660.0, amplitude: -12.0})-[:MEMBER_OF_SET_MANY_ONE]->(s2)
CREATE (a2)-[:STATE_OF]->(a)
CREATE (p3)-[:POINT_STATE]->(s3:PartialStateSet)
CREATE (a3:PartialState {frequency: 660.0, amplitude: -15.0})-[:MEMBER_OF_SET_MANY_ONE]->(s3)
CREATE (a3)-[:STATE_OF]->(a)
CREATE (p4)-[:POINT_STATE]->(s4:PartialStateSet)
CREATE (a4:PartialState {frequency: 440.0, amplitude: -18.0})-[:MEMBER_OF_SET_MANY_ONE]->(s4)
CREATE (a4)-[:STATE_OF]->(a)
CREATE (p5)-[:POINT_STATE]->(s5:PartialStateSet)
CREATE (a5:PartialState {frequency: 440.0, amplitude: -144.0})-[:MEMBER_OF_SET_MANY_ONE]->(s5)
CREATE (a5)-[:STATE_OF]->(a)

// The sound source's position: straight ahead, level and 1 m away, stated
// once at tick 0 of Main. There the mono gain is exactly 1.
CREATE (source)-[:HAS_MUSICAL_TIMELINE]->(:SpatialPositionMusicalTimeline)
       -[:NEXT_POINT]->(position:SpatialPositionMusicalTimelinePoint)
       -[:POINT_STATE]->(:SpatialPositionState {azimuth: 0.0, elevation: 0.0, distance: 1.0})
CREATE (position)-[:POINT_AT {sync_point: 0, offset_seconds: 0.0}]->(main);
