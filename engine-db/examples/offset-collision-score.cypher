// ============================================================
// offset-collision-score.cypher
//
// A score in the canonical model whose physical offsets make points
// collide, exercising the transform's squash rule
// (rendering-engine/docs/transform.md section 6.3, ADR 0014).
// Mirrored for the oracle in offset-collision-score.json; keep the
// two in step.
//
// The tempo map holds 480 BPM, so a beat is 0.125 s. One partial,
// "Hit", plays three percussive hits on beats 0, 1 and 2. Each hit
// is three points at the same tick with fixed offsets: the start,
// the peak 1/128 s (375 samples) later, and the end of the decay
// 3/16 s (9000 samples) later. The offsets are exact in the FLOAT
// column. A hit lasts 0.1875 s but the next beat comes after
// 0.125 s, so the start of hits 2 and 3 computes earlier than the
// end of the hit before.
//
// The rule keeps the user's order and lets time yield: the start of
// hit 2 replaces the end of hit 1 at its time (9000 samples), and
// hit 2's peak is compressed in proportion into the room left
// before its own end (at 9250 rather than 9375). Hit 3 does the
// same at 15000 and 15250. The frequency is 1000 Hz throughout.
// ============================================================

CREATE (engineConfig:EngineConfig {sampleRate: 48000, channelMode: 0});

CREATE (score:Score {name: "Offset Collision Score"})
CREATE (tempoMap:TempoMap {name: "Fast", ticks_per_beat: 960})
CREATE (score)-[:NEXT_POINT]->(tempoMap)
CREATE (tempoMap)-[:NEXT_POINT]->(:TempoMapPoint {position: 0, start_tempo: 480.0, hold: true})

CREATE (score)-[:HAS_SOUNDENTITY]->(entity:SoundEntity {name: "SoundEntity 0"})
CREATE (entity)-[:HAS_SOUNDSOURCE]->(source:SoundSource {name: "SoundSource 0"})
CREATE (source)-[:HAS_PARTIALSET]->(hits:PartialSet {name: "Hits"})
CREATE (hits)-[:HAS_PARTIAL]->(hit:Partial {name: "Hit"})

CREATE (hits)-[:HAS_MUSICAL_TIMELINE]->(timeline:PartialSetMusicalTimeline)
CREATE (timeline)-[:NEXT_POINT]->(p0:PartialSetMusicalTimelinePoint)
       -[:NEXT_POINT]->(p1:PartialSetMusicalTimelinePoint)
       -[:NEXT_POINT]->(p2:PartialSetMusicalTimelinePoint)
       -[:NEXT_POINT]->(p3:PartialSetMusicalTimelinePoint)
       -[:NEXT_POINT]->(p4:PartialSetMusicalTimelinePoint)
       -[:NEXT_POINT]->(p5:PartialSetMusicalTimelinePoint)
       -[:NEXT_POINT]->(p6:PartialSetMusicalTimelinePoint)
       -[:NEXT_POINT]->(p7:PartialSetMusicalTimelinePoint)
       -[:NEXT_POINT]->(p8:PartialSetMusicalTimelinePoint)

// Hit 1 on beat 0.
CREATE (p0)-[:POINT_AT {sync_point: 0, offset_seconds: 0.0}]->(tempoMap)
CREATE (p1)-[:POINT_AT {sync_point: 0, offset_seconds: 0.0078125}]->(tempoMap)
CREATE (p2)-[:POINT_AT {sync_point: 0, offset_seconds: 0.1875}]->(tempoMap)
// Hit 2 on beat 1.
CREATE (p3)-[:POINT_AT {sync_point: 960, offset_seconds: 0.0}]->(tempoMap)
CREATE (p4)-[:POINT_AT {sync_point: 960, offset_seconds: 0.0078125}]->(tempoMap)
CREATE (p5)-[:POINT_AT {sync_point: 960, offset_seconds: 0.1875}]->(tempoMap)
// Hit 3 on beat 2.
CREATE (p6)-[:POINT_AT {sync_point: 1920, offset_seconds: 0.0}]->(tempoMap)
CREATE (p7)-[:POINT_AT {sync_point: 1920, offset_seconds: 0.0078125}]->(tempoMap)
CREATE (p8)-[:POINT_AT {sync_point: 1920, offset_seconds: 0.1875}]->(tempoMap)

// The same three states for every hit: quiet, loud, quiet.
CREATE (p0)-[:POINT_STATE]->(s0:PartialStateSet)
CREATE (h0:PartialState {frequency: 1000.0, amplitude: -60.0})-[:MEMBER_OF_SET_MANY_ONE]->(s0)
CREATE (h0)-[:STATE_OF]->(hit)
CREATE (p1)-[:POINT_STATE]->(s1:PartialStateSet)
CREATE (h1:PartialState {frequency: 1000.0, amplitude: -6.0})-[:MEMBER_OF_SET_MANY_ONE]->(s1)
CREATE (h1)-[:STATE_OF]->(hit)
CREATE (p2)-[:POINT_STATE]->(s2:PartialStateSet)
CREATE (h2:PartialState {frequency: 1000.0, amplitude: -60.0})-[:MEMBER_OF_SET_MANY_ONE]->(s2)
CREATE (h2)-[:STATE_OF]->(hit)
CREATE (p3)-[:POINT_STATE]->(s3:PartialStateSet)
CREATE (h3:PartialState {frequency: 1000.0, amplitude: -60.0})-[:MEMBER_OF_SET_MANY_ONE]->(s3)
CREATE (h3)-[:STATE_OF]->(hit)
CREATE (p4)-[:POINT_STATE]->(s4:PartialStateSet)
CREATE (h4:PartialState {frequency: 1000.0, amplitude: -6.0})-[:MEMBER_OF_SET_MANY_ONE]->(s4)
CREATE (h4)-[:STATE_OF]->(hit)
CREATE (p5)-[:POINT_STATE]->(s5:PartialStateSet)
CREATE (h5:PartialState {frequency: 1000.0, amplitude: -60.0})-[:MEMBER_OF_SET_MANY_ONE]->(s5)
CREATE (h5)-[:STATE_OF]->(hit)
CREATE (p6)-[:POINT_STATE]->(s6:PartialStateSet)
CREATE (h6:PartialState {frequency: 1000.0, amplitude: -60.0})-[:MEMBER_OF_SET_MANY_ONE]->(s6)
CREATE (h6)-[:STATE_OF]->(hit)
CREATE (p7)-[:POINT_STATE]->(s7:PartialStateSet)
CREATE (h7:PartialState {frequency: 1000.0, amplitude: -6.0})-[:MEMBER_OF_SET_MANY_ONE]->(s7)
CREATE (h7)-[:STATE_OF]->(hit)
CREATE (p8)-[:POINT_STATE]->(s8:PartialStateSet)
CREATE (h8:PartialState {frequency: 1000.0, amplitude: -60.0})-[:MEMBER_OF_SET_MANY_ONE]->(s8)
CREATE (h8)-[:STATE_OF]->(hit)

// The sound source's position, in the rendering model's physical units, since
// nothing yet produces it from the canonical model: straight ahead, level and
// 1 m away, from 0 to well past the end of the score, whose length in samples
// the transform decides. There the mono gain is exactly 1, so mono output is
// unchanged by the spatial stage.
CREATE (source)-[:HAS_PHYSICAL_TIMELINE]->(:AzimuthPhysicalTimeline)
       -[:NEXT_POINT]->(:AzimuthPhysicalTimelinePoint {time:0.0, azimuth:0.0})
       -[:NEXT_POINT]->(:AzimuthPhysicalTimelinePoint {time:1000000000.0, azimuth:0.0})
CREATE (source)-[:HAS_PHYSICAL_TIMELINE]->(:ElevationPhysicalTimeline)
       -[:NEXT_POINT]->(:ElevationPhysicalTimelinePoint {time:0.0, elevation:0.0})
       -[:NEXT_POINT]->(:ElevationPhysicalTimelinePoint {time:1000000000.0, elevation:0.0})
CREATE (source)-[:HAS_PHYSICAL_TIMELINE]->(:DistancePhysicalTimeline)
       -[:NEXT_POINT]->(:DistancePhysicalTimelinePoint {time:0.0, distance:1.0})
       -[:NEXT_POINT]->(:DistancePhysicalTimelinePoint {time:1000000000.0, distance:1.0});
