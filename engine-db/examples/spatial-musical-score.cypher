// ============================================================
// spatial-musical-score.cypher
//
// spatial-score.cypher with the source's position written in the canonical
// model: a SpatialPositionMusicalTimeline in musical time, degrees and
// metres, which the transform converts (rendering-engine/docs/transform.md
// section 6.8). The partials are the same physical timelines as there, so
// that only the position goes through the transform, and the two scores
// render to the same samples; the rendering-engine tests require it.
//
//   make run-rendering-engine \
//       RE_STATE=engine-db/examples/spatial-musical-score.cypher \
//       RE_REQUEST=rendering-engine/examples/spatial-request.yaml
//
// The tempo map holds 60 BPM, with 960 ticks per beat, so a beat is a second
// and every point falls on a whole second: an exact number of samples at
// 48 kHz. Each property is its own envelope: a state leaves NULL the
// properties that do not change there, except at the first point, which
// must set all three.
// ============================================================

CREATE (:EngineConfig {sampleRate: 48000, channelMode: 1});

CREATE (score:Score {name: "Spatial Musical Score"})
CREATE (tempoMap:TempoMap {name: "Steady", ticks_per_beat: 960})
CREATE (score)-[:NEXT_POINT]->(tempoMap)
CREATE (tempoMap)-[:NEXT_POINT]->(:TempoMapPoint {position: 0, start_tempo: 60.0, hold: true})
CREATE (score)-[:HAS_SOUNDENTITY]->(entity:SoundEntity {name: "Orbit"})
CREATE (entity)-[:HAS_SOUNDSOURCE]->(source:SoundSource {name: "Orbiting source"})

// 440 Hz, the root.
CREATE (source)-[:HAS_PARTIALSET]->(root:PartialSet {name: "Root"})
CREATE (root)-[:HAS_PARTIAL]->(a:Partial {name: "A 440"})
CREATE (a)-[:HAS_PHYSICAL_TIMELINE]->(:FrequencyPhysicalTimeline)
       -[:NEXT_POINT]->(:FrequencyPhysicalTimelinePoint {time: 0.0, frequency: 0.05759586531581287})
       -[:NEXT_POINT]->(:FrequencyPhysicalTimelinePoint {time: 480000.0, frequency: 0.05759586531581287})
CREATE (a)-[:HAS_PHYSICAL_TIMELINE]->(:AmplitudePhysicalTimeline)
       -[:NEXT_POINT]->(:AmplitudePhysicalTimelinePoint {time: 0.0, amplitude: -60.0})
       -[:NEXT_POINT]->(:AmplitudePhysicalTimelinePoint {time: 4800.0, amplitude: -12.0})
       -[:NEXT_POINT]->(:AmplitudePhysicalTimelinePoint {time: 475200.0, amplitude: -12.0})
       -[:NEXT_POINT]->(:AmplitudePhysicalTimelinePoint {time: 480000.0, amplitude: -90.0})
CREATE (a)-[:HAS_PHYSICAL_TIMELINE]->(:PhasePhysicalTimeline)
       -[:NEXT_POINT]->(:PhasePhysicalTimelinePoint {time: 0.0, phase: 0.0})

// 660 Hz, the fifth.
CREATE (source)-[:HAS_PARTIALSET]->(fifth:PartialSet {name: "Fifth"})
CREATE (fifth)-[:HAS_PARTIAL]->(e:Partial {name: "E 660"})
CREATE (e)-[:HAS_PHYSICAL_TIMELINE]->(:FrequencyPhysicalTimeline)
       -[:NEXT_POINT]->(:FrequencyPhysicalTimelinePoint {time: 0.0, frequency: 0.0863937979737193})
       -[:NEXT_POINT]->(:FrequencyPhysicalTimelinePoint {time: 480000.0, frequency: 0.0863937979737193})
CREATE (e)-[:HAS_PHYSICAL_TIMELINE]->(:AmplitudePhysicalTimeline)
       -[:NEXT_POINT]->(:AmplitudePhysicalTimelinePoint {time: 0.0, amplitude: -60.0})
       -[:NEXT_POINT]->(:AmplitudePhysicalTimelinePoint {time: 4800.0, amplitude: -18.0})
       -[:NEXT_POINT]->(:AmplitudePhysicalTimelinePoint {time: 475200.0, amplitude: -18.0})
       -[:NEXT_POINT]->(:AmplitudePhysicalTimelinePoint {time: 480000.0, amplitude: -90.0})
CREATE (e)-[:HAS_PHYSICAL_TIMELINE]->(:PhasePhysicalTimeline)
       -[:NEXT_POINT]->(:PhasePhysicalTimelinePoint {time: 0.0, phase: 0.0})

// The orbit, as in spatial-score.cypher: ahead, left, round behind through
// ±180° (172° to -172° takes the shorter way), right and back to the front;
// away to 4 m and back while behind, and up to 45° at the back.
CREATE (source)-[:HAS_MUSICAL_TIMELINE]->(:SpatialPositionMusicalTimeline)
       -[:NEXT_POINT]->(p0:SpatialPositionMusicalTimelinePoint)
       -[:NEXT_POINT]->(p1:SpatialPositionMusicalTimelinePoint)
       -[:NEXT_POINT]->(p2:SpatialPositionMusicalTimelinePoint)
       -[:NEXT_POINT]->(p3:SpatialPositionMusicalTimelinePoint)
       -[:NEXT_POINT]->(p4:SpatialPositionMusicalTimelinePoint)
       -[:NEXT_POINT]->(p5:SpatialPositionMusicalTimelinePoint)
       -[:NEXT_POINT]->(p6:SpatialPositionMusicalTimelinePoint)
       -[:NEXT_POINT]->(p7:SpatialPositionMusicalTimelinePoint)
       -[:NEXT_POINT]->(p8:SpatialPositionMusicalTimelinePoint)
CREATE (p0)-[:POINT_AT {sync_point: 0, offset_seconds: 0.0}]->(tempoMap)
CREATE (p1)-[:POINT_AT {sync_point: 1920, offset_seconds: 0.0}]->(tempoMap)
CREATE (p2)-[:POINT_AT {sync_point: 2880, offset_seconds: 0.0}]->(tempoMap)
CREATE (p3)-[:POINT_AT {sync_point: 3840, offset_seconds: 0.0}]->(tempoMap)
CREATE (p4)-[:POINT_AT {sync_point: 4800, offset_seconds: 0.0}]->(tempoMap)
CREATE (p5)-[:POINT_AT {sync_point: 5760, offset_seconds: 0.0}]->(tempoMap)
CREATE (p6)-[:POINT_AT {sync_point: 6720, offset_seconds: 0.0}]->(tempoMap)
CREATE (p7)-[:POINT_AT {sync_point: 7680, offset_seconds: 0.0}]->(tempoMap)
CREATE (p8)-[:POINT_AT {sync_point: 9600, offset_seconds: 0.0}]->(tempoMap)
CREATE (p0)-[:POINT_STATE]->(:SpatialPositionState {azimuth: 0.0, elevation: 0.0, distance: 1.0})
CREATE (p1)-[:POINT_STATE]->(:SpatialPositionState {azimuth: 90.0})
CREATE (p2)-[:POINT_STATE]->(:SpatialPositionState {distance: 1.0})
CREATE (p3)-[:POINT_STATE]->(:SpatialPositionState {azimuth: 172.0, elevation: 0.0})
CREATE (p4)-[:POINT_STATE]->(:SpatialPositionState {elevation: 45.0, distance: 4.0})
CREATE (p5)-[:POINT_STATE]->(:SpatialPositionState {azimuth: -172.0, elevation: 0.0})
CREATE (p6)-[:POINT_STATE]->(:SpatialPositionState {distance: 1.0})
CREATE (p7)-[:POINT_STATE]->(:SpatialPositionState {azimuth: -90.0})
CREATE (p8)-[:POINT_STATE]->(:SpatialPositionState {azimuth: 0.0, elevation: 0.0, distance: 1.0});
