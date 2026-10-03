// ============================================================
// minimum-score.cypher
//
// An example of a very simple score
// ============================================================

// Configuration parameters
CREATE (enngineConfig:EngineConfig {
    sampleRate: 48000,
    channelMode: 0
    }
);

// Score is essentially a singleton
CREATE (score:Score {name:"Minimal Score"})

// A score has many sound entities
CREATE (soundEntity0:SoundEntity {name:"SoundEntity 0"})
CREATE (score)-[:HAS_SOUNDENTITY]->(soundEntity0)

// A sound entity has many sound sources
CREATE (soundSource0:SoundSource {name:"SoundSource 0"})
CREATE (soundEntity0)-[:HAS_SOUNDSOURCE]->(soundSource0)

// A sound source has many partial sets
CREATE (partialSet0:PartialSet {name:"PartialSet 0"})
CREATE (soundSource0)-[:HAS_PARTIALSET]->(partialSet0)

// A partial set has many partials
CREATE (partial0:Partial {name:"Partial 0"})
CREATE (partialSet0)-[:HAS_PARTIAL]->(partial0)

// A partial has an amplitude timeline
CREATE (amplitudePhysicalTimeline0:AmplitudePhysicalTimeline {})
CREATE (partial0)-[:HAS_PHYSICAL_TIMELINE]->(amplitudePhysicalTimeline0)

// This amplitude timeline has two amplitude timeline points, defaulting to a linear curve
CREATE (amplitudePhysicalTimelinePoint0:AmplitudePhysicalTimelinePoint {time:0.0, amplitude:-6.0})
CREATE (amplitudePhysicalTimelinePoint1:AmplitudePhysicalTimelinePoint {time:480000.0, amplitude:-6.0}) // samples, dB
CREATE (amplitudePhysicalTimeline0)-[:NEXT_POINT]->(amplitudePhysicalTimelinePoint0)
CREATE (amplitudePhysicalTimelinePoint0)-[:NEXT_POINT]->(amplitudePhysicalTimelinePoint1)

// A partial has a frequency timeline
CREATE (frequencyPhysicalTimeline0:FrequencyPhysicalTimeline {})
CREATE (partial0)-[:HAS_PHYSICAL_TIMELINE]->(frequencyPhysicalTimeline0)

// This frequency timeline has two frequency timeline points, defaulting to a linear curve
CREATE (frequencyPhysicalTimelinePoint0:FrequencyPhysicalTimelinePoint {time:0.0, frequency:0.1})
CREATE (frequencyPhysicalTimelinePoint1:FrequencyPhysicalTimelinePoint {time:480000.0, frequency:0.1})
CREATE (frequencyPhysicalTimeline0)-[:NEXT_POINT]->(frequencyPhysicalTimelinePoint0)
CREATE (frequencyPhysicalTimelinePoint0)-[:NEXT_POINT]->(frequencyPhysicalTimelinePoint1)

// A partial has a phase timeline, with at least one point: its phase at the
// start of its lifeline, in radians. One point and no more lets the phase run
// free from there.
CREATE (phasePhysicalTimeline0:PhasePhysicalTimeline {})
CREATE (partial0)-[:HAS_PHYSICAL_TIMELINE]->(phasePhysicalTimeline0)
CREATE (phasePhysicalTimeline0)-[:NEXT_POINT]->(:PhasePhysicalTimelinePoint {time:0.0, phase:0.0});