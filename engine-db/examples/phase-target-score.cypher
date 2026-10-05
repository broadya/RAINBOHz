// ============================================================
// phase-target-score.cypher
//
// A small score that exercises phase control: partials whose phase
// timelines set targets the rendered phase must meet, NULL points up
// to which a partial runs free, and a NULL start from which the
// initial phase is worked backwards. Mirrored for the oracle in
// phase-target-score.json; keep the two in step.
//
// Units are the rendering model's: time in samples, frequency in
// radians per sample, amplitude in dB, phase in wrapped radians. At
// 48 kHz a frame is 48,000 samples, so frame k covers
// [48000 k, 48000 (k + 1)).
//
// A phase point is a target, not a knot: between two points the
// phase is adjusted by a linear ramp, so that it arrives at the
// target exactly. A NULL point means "free-running up to here"
// (rendering-engine/docs/precomputation.md section 6.8).
// ============================================================

// Configuration parameters
CREATE (engineConfig:EngineConfig {
    sampleRate: 48000,
    channelMode: 0
    }
);

CREATE (score:Score {name:"Phase Target Score"})

CREATE (soundEntity0:SoundEntity {name:"SoundEntity 0"})
CREATE (score)-[:HAS_SOUNDENTITY]->(soundEntity0)

CREATE (soundSource0:SoundSource {name:"SoundSource 0"})
CREATE (soundEntity0)-[:HAS_SOUNDSOURCE]->(soundSource0)

// ------------------------------------------------------------
// Partial set 0, "Targets": the example that motivated phase
// control, with and without a NULL point in the middle.
// ------------------------------------------------------------
CREATE (partialSet0:PartialSet {name:"Targets"})
CREATE (soundSource0)-[:HAS_PARTIALSET]->(partialSet0)

// Partial A: constant 0.1 rad/sample for five frames. Phase 0 at the
// start and π (180 degrees) at 3 s: the ramp runs over [0, 144000)
// and the adjustment then holds.
CREATE (partialA:Partial {name:"A target"})
CREATE (partialSet0)-[:HAS_PARTIAL]->(partialA)
CREATE (partialA)-[:HAS_PHYSICAL_TIMELINE]->(amplitudeA:AmplitudePhysicalTimeline)
CREATE (amplitudeA)-[:NEXT_POINT]->(:AmplitudePhysicalTimelinePoint {time:0.0, amplitude:-12.0})
       -[:NEXT_POINT]->(:AmplitudePhysicalTimelinePoint {time:240000.0, amplitude:-12.0})
CREATE (partialA)-[:HAS_PHYSICAL_TIMELINE]->(frequencyA:FrequencyPhysicalTimeline)
CREATE (frequencyA)-[:NEXT_POINT]->(:FrequencyPhysicalTimelinePoint {time:0.0, frequency:0.1})
       -[:NEXT_POINT]->(:FrequencyPhysicalTimelinePoint {time:240000.0, frequency:0.1})
CREATE (partialA)-[:HAS_PHYSICAL_TIMELINE]->(phaseA:PhasePhysicalTimeline)
CREATE (phaseA)-[:NEXT_POINT]->(:PhasePhysicalTimelinePoint {time:0.0, phase:0.0})
       -[:NEXT_POINT]->(:PhasePhysicalTimelinePoint {time:144000.0, phase:3.141592653589793})

// Partial B: as A at 0.12 rad/sample, with a NULL point at 1.5 s, in
// the middle of frame 1: free-running to there, so the same target
// is met by a steeper ramp over [72000, 144000).
CREATE (partialB:Partial {name:"B target after null"})
CREATE (partialSet0)-[:HAS_PARTIAL]->(partialB)
CREATE (partialB)-[:HAS_PHYSICAL_TIMELINE]->(amplitudeB:AmplitudePhysicalTimeline)
CREATE (amplitudeB)-[:NEXT_POINT]->(:AmplitudePhysicalTimelinePoint {time:0.0, amplitude:-12.0})
       -[:NEXT_POINT]->(:AmplitudePhysicalTimelinePoint {time:240000.0, amplitude:-12.0})
CREATE (partialB)-[:HAS_PHYSICAL_TIMELINE]->(frequencyB:FrequencyPhysicalTimeline)
CREATE (frequencyB)-[:NEXT_POINT]->(:FrequencyPhysicalTimelinePoint {time:0.0, frequency:0.12})
       -[:NEXT_POINT]->(:FrequencyPhysicalTimelinePoint {time:240000.0, frequency:0.12})
CREATE (partialB)-[:HAS_PHYSICAL_TIMELINE]->(phaseB:PhasePhysicalTimeline)
CREATE (phaseB)-[:NEXT_POINT]->(:PhasePhysicalTimelinePoint {time:0.0, phase:0.0})
       -[:NEXT_POINT]->(:PhasePhysicalTimelinePoint {time:72000.0})
       -[:NEXT_POINT]->(:PhasePhysicalTimelinePoint {time:144000.0, phase:3.141592653589793})

// ------------------------------------------------------------
// Partial set 1, "Glides": targets on gliding partials.
// ------------------------------------------------------------
CREATE (partialSet1:PartialSet {name:"Glides"})
CREATE (soundSource0)-[:HAS_PARTIALSET]->(partialSet1)

// Partial C: glides 0.05 -> 0.08 rad/sample over five frames. Its
// first phase point is NULL, so its initial phase is worked backwards:
// running free from the start, it meets π at 2 s with no adjustment.
CREATE (partialC:Partial {name:"C null start"})
CREATE (partialSet1)-[:HAS_PARTIAL]->(partialC)
CREATE (partialC)-[:HAS_PHYSICAL_TIMELINE]->(amplitudeC:AmplitudePhysicalTimeline)
CREATE (amplitudeC)-[:NEXT_POINT]->(:AmplitudePhysicalTimelinePoint {time:0.0, amplitude:-12.0})
       -[:NEXT_POINT]->(:AmplitudePhysicalTimelinePoint {time:240000.0, amplitude:-12.0})
CREATE (partialC)-[:HAS_PHYSICAL_TIMELINE]->(frequencyC:FrequencyPhysicalTimeline)
CREATE (frequencyC)-[:NEXT_POINT]->(:FrequencyPhysicalTimelinePoint {time:0.0, frequency:0.05})
       -[:NEXT_POINT]->(:FrequencyPhysicalTimelinePoint {time:240000.0, frequency:0.08})
CREATE (partialC)-[:HAS_PHYSICAL_TIMELINE]->(phaseC:PhasePhysicalTimeline)
CREATE (phaseC)-[:NEXT_POINT]->(:PhasePhysicalTimelinePoint {time:0.0})
       -[:NEXT_POINT]->(:PhasePhysicalTimelinePoint {time:96000.0, phase:3.141592653589793})

// Partial D: starts and ends between samples, glides down and back up,
// and has targets between samples, a NULL point, and a target at the
// end of its lifeline. Its adjustments accumulate without wrapping.
CREATE (partialD:Partial {name:"D many targets"})
CREATE (partialSet1)-[:HAS_PARTIAL]->(partialD)
CREATE (partialD)-[:HAS_PHYSICAL_TIMELINE]->(amplitudeD:AmplitudePhysicalTimeline)
CREATE (amplitudeD)-[:NEXT_POINT]->(:AmplitudePhysicalTimelinePoint {time:30000.5, amplitude:-12.0})
       -[:NEXT_POINT]->(:AmplitudePhysicalTimelinePoint {time:200000.5, amplitude:-12.0})
CREATE (partialD)-[:HAS_PHYSICAL_TIMELINE]->(frequencyD:FrequencyPhysicalTimeline)
CREATE (frequencyD)-[:NEXT_POINT]->(:FrequencyPhysicalTimelinePoint {time:30000.5, frequency:0.2})
       -[:NEXT_POINT]->(:FrequencyPhysicalTimelinePoint {time:100000.5, frequency:0.15})
       -[:NEXT_POINT]->(:FrequencyPhysicalTimelinePoint {time:200000.5, frequency:0.2})
CREATE (partialD)-[:HAS_PHYSICAL_TIMELINE]->(phaseD:PhasePhysicalTimeline)
CREATE (phaseD)-[:NEXT_POINT]->(:PhasePhysicalTimelinePoint {time:30000.5, phase:1.0})
       -[:NEXT_POINT]->(:PhasePhysicalTimelinePoint {time:50000.25, phase:-2.5})
       -[:NEXT_POINT]->(:PhasePhysicalTimelinePoint {time:100000.5})
       -[:NEXT_POINT]->(:PhasePhysicalTimelinePoint {time:150000.75, phase:3.0})
       -[:NEXT_POINT]->(:PhasePhysicalTimelinePoint {time:200000.5, phase:-1.0})

// The sound source's position, in the rendering model's physical units, since
// nothing yet produces it from the canonical model: straight ahead, level and
// 1 m away, from 0 to the end of the score. There the mono gain is exactly 1,
// so mono output is unchanged by the spatial stage.
CREATE (soundSource0)-[:HAS_PHYSICAL_TIMELINE]->(:AzimuthPhysicalTimeline)
       -[:NEXT_POINT]->(:AzimuthPhysicalTimelinePoint {time:0.0, azimuth:0.0})
       -[:NEXT_POINT]->(:AzimuthPhysicalTimelinePoint {time:240000.0, azimuth:0.0})
CREATE (soundSource0)-[:HAS_PHYSICAL_TIMELINE]->(:ElevationPhysicalTimeline)
       -[:NEXT_POINT]->(:ElevationPhysicalTimelinePoint {time:0.0, elevation:0.0})
       -[:NEXT_POINT]->(:ElevationPhysicalTimelinePoint {time:240000.0, elevation:0.0})
CREATE (soundSource0)-[:HAS_PHYSICAL_TIMELINE]->(:DistancePhysicalTimeline)
       -[:NEXT_POINT]->(:DistancePhysicalTimelinePoint {time:0.0, distance:1.0})
       -[:NEXT_POINT]->(:DistancePhysicalTimelinePoint {time:240000.0, distance:1.0});
