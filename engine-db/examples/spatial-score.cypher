// ============================================================
// spatial-score.cypher
//
// A sound source moving around the listener, for listening to the spatial
// stage by hand (rendering-engine/docs/rendering.md section 8.5):
//
//   make run-rendering-engine RE_STATE=engine-db/examples/spatial-score.cypher \
//       RE_REQUEST=rendering-engine/examples/spatial-request.yaml
//
// Two partials, a fifth apart, in two partial sets of one source, so that
// they share its position. Over ten seconds the source goes from straight
// ahead to the left, round behind the listener through ±π, to the right and
// back to the front, moving away to 4 m and back while it is behind and
// rising above the horizon at the back. Channel mode 1: stereo, decoded from
// an M/S pair.
//
// Physical units throughout, as the rendering model has them: times in
// samples at 48 kHz, frequencies in radians per sample, amplitudes in dB,
// angles in radians, distances in metres.
// ============================================================

CREATE (:EngineConfig {sampleRate: 48000, channelMode: 1});

CREATE (score:Score {name: "Spatial Score"})
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

// The orbit. From 3.0 to -3.0 rad the azimuth takes the shorter way, through
// ±π behind the listener, not back round through the front.
CREATE (source)-[:HAS_PHYSICAL_TIMELINE]->(:AzimuthPhysicalTimeline)
       -[:NEXT_POINT]->(:AzimuthPhysicalTimelinePoint {time: 0.0, azimuth: 0.0})
       -[:NEXT_POINT]->(:AzimuthPhysicalTimelinePoint {time: 96000.0, azimuth: 1.5707963267948966})
       -[:NEXT_POINT]->(:AzimuthPhysicalTimelinePoint {time: 192000.0, azimuth: 3.0})
       -[:NEXT_POINT]->(:AzimuthPhysicalTimelinePoint {time: 288000.0, azimuth: -3.0})
       -[:NEXT_POINT]->(:AzimuthPhysicalTimelinePoint {time: 384000.0, azimuth: -1.5707963267948966})
       -[:NEXT_POINT]->(:AzimuthPhysicalTimelinePoint {time: 480000.0, azimuth: 0.0})
CREATE (source)-[:HAS_PHYSICAL_TIMELINE]->(:ElevationPhysicalTimeline)
       -[:NEXT_POINT]->(:ElevationPhysicalTimelinePoint {time: 0.0, elevation: 0.0})
       -[:NEXT_POINT]->(:ElevationPhysicalTimelinePoint {time: 192000.0, elevation: 0.0})
       -[:NEXT_POINT]->(:ElevationPhysicalTimelinePoint {time: 240000.0, elevation: 0.7853981633974483})
       -[:NEXT_POINT]->(:ElevationPhysicalTimelinePoint {time: 288000.0, elevation: 0.0})
       -[:NEXT_POINT]->(:ElevationPhysicalTimelinePoint {time: 480000.0, elevation: 0.0})
CREATE (source)-[:HAS_PHYSICAL_TIMELINE]->(:DistancePhysicalTimeline)
       -[:NEXT_POINT]->(:DistancePhysicalTimelinePoint {time: 0.0, distance: 1.0})
       -[:NEXT_POINT]->(:DistancePhysicalTimelinePoint {time: 144000.0, distance: 1.0})
       -[:NEXT_POINT]->(:DistancePhysicalTimelinePoint {time: 240000.0, distance: 4.0})
       -[:NEXT_POINT]->(:DistancePhysicalTimelinePoint {time: 336000.0, distance: 1.0})
       -[:NEXT_POINT]->(:DistancePhysicalTimelinePoint {time: 480000.0, distance: 1.0});
