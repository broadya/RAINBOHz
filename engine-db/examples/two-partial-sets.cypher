// ============================================================
// two-partial-sets.cypher
//
// A small score that exercises what rendering has to get right and
// minimum-score.cypher cannot show: two partial sets, a partial that
// starts and ends between samples in the middle of a frame, glides,
// fades, and partials that leave the audio range through both ends of
// the frequency-range roll-off. Mirrored for the oracle in
// two-partial-sets.json; keep the two in step.
//
// Units are the rendering model's: time in samples, frequency in
// radians per sample, amplitude in dB. At 48 kHz a frame is 48,000
// samples, so frame k covers [48000 k, 48000 (k + 1)).
// ============================================================

// Configuration parameters
CREATE (engineConfig:EngineConfig {
    sampleRate: 48000,
    channelMode: 0
    }
);

CREATE (score:Score {name:"Two Partial Sets"})

CREATE (soundEntity0:SoundEntity {name:"SoundEntity 0"})
CREATE (score)-[:HAS_SOUNDENTITY]->(soundEntity0)

CREATE (soundSource0:SoundSource {name:"SoundSource 0"})
CREATE (soundEntity0)-[:HAS_SOUNDSOURCE]->(soundSource0)

// ------------------------------------------------------------
// Partial set 0, "Sustain": a constant partial for the whole
// score, and one that enters mid-frame with a fade.
// ------------------------------------------------------------
CREATE (partialSet0:PartialSet {name:"Sustain"})
CREATE (soundSource0)-[:HAS_PARTIALSET]->(partialSet0)

// Partial A: 0.1 rad/sample (763.9 Hz) at -6 dB for ten frames.
CREATE (partialA:Partial {name:"A constant"})
CREATE (partialSet0)-[:HAS_PARTIAL]->(partialA)
CREATE (partialA)-[:HAS_PHYSICAL_TIMELINE]->(amplitudeA:AmplitudePhysicalTimeline)
CREATE (amplitudeA)-[:NEXT_POINT]->(:AmplitudePhysicalTimelinePoint {time:0.0, amplitude:-6.0})
       -[:NEXT_POINT]->(:AmplitudePhysicalTimelinePoint {time:480000.0, amplitude:-6.0})
CREATE (partialA)-[:HAS_PHYSICAL_TIMELINE]->(frequencyA:FrequencyPhysicalTimeline)
CREATE (frequencyA)-[:NEXT_POINT]->(:FrequencyPhysicalTimelinePoint {time:0.0, frequency:0.1})
       -[:NEXT_POINT]->(:FrequencyPhysicalTimelinePoint {time:480000.0, frequency:0.1})

// Partial B: the second partial of rendering.md section 12. Starts at
// 144100.5, half a sample into frame 3; glides 0.05 -> 0.06 over 2,000
// samples then holds; fades in from -60 dB over 1,000 samples, holds
// at -6 dB, fades out to -90 dB over the last 10,000 samples; ends at
// 300000.5, inside frame 6.
CREATE (partialB:Partial {name:"B mid-frame"})
CREATE (partialSet0)-[:HAS_PARTIAL]->(partialB)
CREATE (partialB)-[:HAS_PHYSICAL_TIMELINE]->(amplitudeB:AmplitudePhysicalTimeline)
CREATE (amplitudeB)-[:NEXT_POINT]->(:AmplitudePhysicalTimelinePoint {time:144100.5, amplitude:-60.0})
       -[:NEXT_POINT]->(:AmplitudePhysicalTimelinePoint {time:145100.5, amplitude:-6.0})
       -[:NEXT_POINT]->(:AmplitudePhysicalTimelinePoint {time:290000.5, amplitude:-6.0})
       -[:NEXT_POINT]->(:AmplitudePhysicalTimelinePoint {time:300000.5, amplitude:-90.0})
CREATE (partialB)-[:HAS_PHYSICAL_TIMELINE]->(frequencyB:FrequencyPhysicalTimeline)
CREATE (frequencyB)-[:NEXT_POINT]->(:FrequencyPhysicalTimelinePoint {time:144100.5, frequency:0.05})
       -[:NEXT_POINT]->(:FrequencyPhysicalTimelinePoint {time:146100.5, frequency:0.06})
       -[:NEXT_POINT]->(:FrequencyPhysicalTimelinePoint {time:300000.5, frequency:0.06})

// ------------------------------------------------------------
// Partial set 1, "Glides": three partials that leave the audio range.
// ------------------------------------------------------------
CREATE (partialSet1:PartialSet {name:"Glides"})
CREATE (soundSource0)-[:HAS_PARTIALSET]->(partialSet1)

// Partial C: glides down from 0.2 rad/sample (1528 Hz) to 0.0002
// (1.5 Hz) over frames 2 to 4, through the low roll-off band, and
// holds there, silent, until frame 6 starts.
CREATE (partialC:Partial {name:"C glide down"})
CREATE (partialSet1)-[:HAS_PARTIAL]->(partialC)
CREATE (partialC)-[:HAS_PHYSICAL_TIMELINE]->(amplitudeC:AmplitudePhysicalTimeline)
CREATE (amplitudeC)-[:NEXT_POINT]->(:AmplitudePhysicalTimelinePoint {time:96000.0, amplitude:-12.0})
       -[:NEXT_POINT]->(:AmplitudePhysicalTimelinePoint {time:288000.0, amplitude:-12.0})
CREATE (partialC)-[:HAS_PHYSICAL_TIMELINE]->(frequencyC:FrequencyPhysicalTimeline)
CREATE (frequencyC)-[:NEXT_POINT]->(:FrequencyPhysicalTimelinePoint {time:96000.0, frequency:0.2})
       -[:NEXT_POINT]->(:FrequencyPhysicalTimelinePoint {time:240000.0, frequency:0.0002})
       -[:NEXT_POINT]->(:FrequencyPhysicalTimelinePoint {time:288000.0, frequency:0.0002})

// Partial D: glides up from 2.5 rad/sample (19.1 kHz) to 3.2 (24.4 kHz,
// above Nyquist) over frames 1 to 3, through the high roll-off band
// [2.618, 2.880] at 48 kHz, and holds there, silent.
CREATE (partialD:Partial {name:"D glide up"})
CREATE (partialSet1)-[:HAS_PARTIAL]->(partialD)
CREATE (partialD)-[:HAS_PHYSICAL_TIMELINE]->(amplitudeD:AmplitudePhysicalTimeline)
CREATE (amplitudeD)-[:NEXT_POINT]->(:AmplitudePhysicalTimelinePoint {time:48000.0, amplitude:-20.0})
       -[:NEXT_POINT]->(:AmplitudePhysicalTimelinePoint {time:200000.0, amplitude:-20.0})
CREATE (partialD)-[:HAS_PHYSICAL_TIMELINE]->(frequencyD:FrequencyPhysicalTimeline)
CREATE (frequencyD)-[:NEXT_POINT]->(:FrequencyPhysicalTimelinePoint {time:48000.0, frequency:2.5})
       -[:NEXT_POINT]->(:FrequencyPhysicalTimelinePoint {time:192000.0, frequency:3.2})
       -[:NEXT_POINT]->(:FrequencyPhysicalTimelinePoint {time:200000.0, frequency:3.2})

// Partial E: passes through zero frequency during frame 1: from 0.01
// rad/sample (76 Hz) at the start of frame 0 to -0.01 at the end of
// frame 1. It fades out below 20 Hz and is silent while negative.
CREATE (partialE:Partial {name:"E through zero"})
CREATE (partialSet1)-[:HAS_PARTIAL]->(partialE)
CREATE (partialE)-[:HAS_PHYSICAL_TIMELINE]->(amplitudeE:AmplitudePhysicalTimeline)
CREATE (amplitudeE)-[:NEXT_POINT]->(:AmplitudePhysicalTimelinePoint {time:0.0, amplitude:-30.0})
       -[:NEXT_POINT]->(:AmplitudePhysicalTimelinePoint {time:96000.0, amplitude:-30.0})
CREATE (partialE)-[:HAS_PHYSICAL_TIMELINE]->(frequencyE:FrequencyPhysicalTimeline)
CREATE (frequencyE)-[:NEXT_POINT]->(:FrequencyPhysicalTimelinePoint {time:0.0, frequency:0.01})
       -[:NEXT_POINT]->(:FrequencyPhysicalTimelinePoint {time:96000.0, frequency:-0.01});
