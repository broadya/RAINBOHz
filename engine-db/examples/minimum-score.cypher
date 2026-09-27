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
CREATE (score)-[:AGGREGATE_OF_ONE_MANY]->(soundEntity0)

// A sound entity has many sound sources
CREATE (soundSource0:SoundSource {name:"SoundSource 0"})
CREATE (soundEntity0)-[:AGGREGATE_OF_ONE_MANY]->(soundSource0)

// A sound source has many partial sets
CREATE (partialSet0:PartialSet {name:"PartialSet 0"})
CREATE (soundSource0)-[:AGGREGATE_OF_ONE_MANY]->(partialSet0)

// A partial set has many partials
CREATE (partial0:Partial {name:"Partial 0"})
CREATE (partialSet0)-[:AGGREGATE_OF_ONE_MANY]->(partial0)

// A partial has an amplitude timeline
CREATE (amplitudeTimeline0:AmplitudeTimeline {})
CREATE (partial0)-[:HAS_TIMELINE]->(amplitudeTimeline0)

// This amplitude timeline has two amplitude timeline points, defaulting to a linear curve
CREATE (amplitudeTimelinePoint0:AmplitudeTimelinePoint {time:0.0, amplitude:-6.0})
CREATE (amplitudeTimelinePoint1:AmplitudeTimelinePoint {time:480000.0, amplitude:-6.0}) // samples, dB
CREATE (amplitudeTimeline0)-[:NEXT_POINT]->(amplitudeTimelinePoint0)
CREATE (amplitudeTimelinePoint0)-[:NEXT_POINT]->(amplitudeTimelinePoint1)

// A partial has a frequency timeline
CREATE (frequencyTimeline0:FrequencyTimeline {})
CREATE (partial0)-[:HAS_TIMELINE]->(frequencyTimeline0)

// This frequency timeline has two frequency timeline points, defaulting to a linear curve
CREATE (frequencyTimelinePoint0:FrequencyTimelinePoint {time:0.0, frequency:0.1})
CREATE (frequencyTimelinePoint1:FrequencyTimelinePoint {time:480000.0, frequency:0.1})
CREATE (frequencyTimeline0)-[:NEXT_POINT]->(frequencyTimelinePoint0)
CREATE (frequencyTimelinePoint0)-[:NEXT_POINT]->(frequencyTimelinePoint1);