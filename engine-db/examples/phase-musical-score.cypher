// ============================================================
// phase-musical-score.cypher
//
// Phase targets in the canonical model: PartialState.phase in
// degrees, on a tempo map, which the transform turns into each
// partial's PhasePhysicalTimeline in radians
// (rendering-engine/docs/transform.md §6.6). Mirrored for the oracle
// in phase-musical-score.json, which holds the physical values the
// transform must produce; keep the two in step.
//
// The tempo map is tempo-map-score's: 960 ticks per beat, 120 BPM
// at tick 0 ramping linearly in musical position to 60 BPM at tick
// 3840, then held.
//
// One partial set, "Phased", with two partials, and a timeline of
// seven points: beats 0, 1, 2, 3, 4, then beat 4 again with an
// offset of -0.25 s, then beat 8.
//
// Partial A has a state at every point but the sixth. Its phase is
// NULL at beats 0 to 3, so precomputation works its initial phase
// backwards to meet the first target, 90 degrees at beat 4; the
// run of four NULL points is collapsed to its first and last. It
// then ramps to -180 degrees at beat 8.
//
// Partial B starts at phase 0 and has states at beat 4 and at the
// point just after it, which the offset pulls back before it. The
// later state wins the collision (ADR 0014): its frequency and its
// -60 degree target are placed at beat 4's time, and the 45 degree
// target there is discarded. It ends at 120 degrees.
// ============================================================

CREATE (engineConfig:EngineConfig {sampleRate: 48000, channelMode: 0});

CREATE (score:Score {name: "Phase Musical Score"})
CREATE (tempoMap:TempoMap {name: "Main", ticks_per_beat: 960})
CREATE (score)-[:NEXT_POINT]->(tempoMap)
CREATE (tempoMap)-[:NEXT_POINT]->(:TempoMapPoint {position: 0, start_tempo: 120.0, hold: false})
       -[:NEXT_POINT]->(:TempoMapPoint {position: 3840, start_tempo: 60.0, hold: true})

CREATE (score)-[:HAS_SOUNDENTITY]->(entity:SoundEntity {name: "SoundEntity 0"})
CREATE (entity)-[:HAS_SOUNDSOURCE]->(source:SoundSource {name: "SoundSource 0"})
CREATE (source)-[:HAS_PARTIALSET]->(phased:PartialSet {name: "Phased"})
CREATE (phased)-[:HAS_PARTIAL]->(a:Partial {name: "A"})
CREATE (phased)-[:HAS_PARTIAL]->(b:Partial {name: "B"})

CREATE (phased)-[:HAS_MUSICAL_TIMELINE]->(timeline:PartialSetMusicalTimeline)
CREATE (timeline)-[:NEXT_POINT]->(p0:PartialSetMusicalTimelinePoint)
       -[:NEXT_POINT]->(p1:PartialSetMusicalTimelinePoint)
       -[:NEXT_POINT]->(p2:PartialSetMusicalTimelinePoint)
       -[:NEXT_POINT]->(p3:PartialSetMusicalTimelinePoint)
       -[:NEXT_POINT]->(p4:PartialSetMusicalTimelinePoint)
       -[:NEXT_POINT]->(p5:PartialSetMusicalTimelinePoint)
       -[:NEXT_POINT]->(p6:PartialSetMusicalTimelinePoint)
CREATE (p0)-[:POINT_AT {sync_point: 0, offset_seconds: 0.0}]->(tempoMap)
CREATE (p1)-[:POINT_AT {sync_point: 960, offset_seconds: 0.0}]->(tempoMap)
CREATE (p2)-[:POINT_AT {sync_point: 1920, offset_seconds: 0.0}]->(tempoMap)
CREATE (p3)-[:POINT_AT {sync_point: 2880, offset_seconds: 0.0}]->(tempoMap)
CREATE (p4)-[:POINT_AT {sync_point: 3840, offset_seconds: 0.0}]->(tempoMap)
CREATE (p5)-[:POINT_AT {sync_point: 3840, offset_seconds: -0.25}]->(tempoMap)
CREATE (p6)-[:POINT_AT {sync_point: 7680, offset_seconds: 0.0}]->(tempoMap)

// Beat 0: A free-running at 440 Hz; B at 550 Hz, phase 0.
CREATE (p0)-[:POINT_STATE]->(s0:PartialStateSet)
CREATE (a0:PartialState {frequency: 440.0, amplitude: -12.0})-[:MEMBER_OF_SET_MANY_ONE]->(s0)
CREATE (a0)-[:STATE_OF]->(a)
CREATE (b0:PartialState {frequency: 550.0, amplitude: -12.0, phase: 0.0})-[:MEMBER_OF_SET_MANY_ONE]->(s0)
CREATE (b0)-[:STATE_OF]->(b)

// Beats 1 to 3: A's amplitude only, phase NULL; B has no state.
CREATE (p1)-[:POINT_STATE]->(s1:PartialStateSet)
CREATE (a1:PartialState {amplitude: -12.0})-[:MEMBER_OF_SET_MANY_ONE]->(s1)
CREATE (a1)-[:STATE_OF]->(a)
CREATE (p2)-[:POINT_STATE]->(s2:PartialStateSet)
CREATE (a2:PartialState {amplitude: -12.0})-[:MEMBER_OF_SET_MANY_ONE]->(s2)
CREATE (a2)-[:STATE_OF]->(a)
CREATE (p3)-[:POINT_STATE]->(s3:PartialStateSet)
CREATE (a3:PartialState {amplitude: -12.0})-[:MEMBER_OF_SET_MANY_ONE]->(s3)
CREATE (a3)-[:STATE_OF]->(a)

// Beat 4: A has dropped to 330 Hz and meets 90 degrees; B targets 45
// degrees, but is overtaken by the next point.
CREATE (p4)-[:POINT_STATE]->(s4:PartialStateSet)
CREATE (a4:PartialState {frequency: 330.0, amplitude: -12.0, phase: 90.0})-[:MEMBER_OF_SET_MANY_ONE]->(s4)
CREATE (a4)-[:STATE_OF]->(a)
CREATE (b4:PartialState {frequency: 550.0, amplitude: -12.0, phase: 45.0})-[:MEMBER_OF_SET_MANY_ONE]->(s4)
CREATE (b4)-[:STATE_OF]->(b)

// Beat 4 less 0.25 s: B only, at 600 Hz and -60 degrees.
CREATE (p5)-[:POINT_STATE]->(s5:PartialStateSet)
CREATE (b5:PartialState {frequency: 600.0, amplitude: -12.0, phase: -60.0})-[:MEMBER_OF_SET_MANY_ONE]->(s5)
CREATE (b5)-[:STATE_OF]->(b)

// Beat 8: both end, faded to silence, on their last targets.
CREATE (p6)-[:POINT_STATE]->(s6:PartialStateSet)
CREATE (a6:PartialState {frequency: 330.0, amplitude: -144.0, phase: -180.0})-[:MEMBER_OF_SET_MANY_ONE]->(s6)
CREATE (a6)-[:STATE_OF]->(a)
CREATE (b6:PartialState {frequency: 600.0, amplitude: -144.0, phase: 120.0})-[:MEMBER_OF_SET_MANY_ONE]->(s6)
CREATE (b6)-[:STATE_OF]->(b);
