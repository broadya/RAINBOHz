#ifndef AUDIO_FRAGMENT_PARSER_H
#define AUDIO_FRAGMENT_PARSER_H

#include <yaml-cpp/yaml.h>

#include <string>
#include <vector>

#include "envelope_types.h"  // Provides Envelope, FrequencyEnvelope, AmplitudeEnvelope, PhaseCoordinate, PhaseCoordinates, PartialEnvelopes, etc.
#include "yaml_keys.h"       // Provides YAML key constants

namespace RAINBOHz {

// Represents an audio fragment as described in the YAML file.
struct AudioFragment {
    double start_time;                // Start time in seconds.
    std::vector<std::string> labels;  // Fragment-level labels.

    // A partial with its labels and envelope data.
    struct Partial {
        std::vector<std::string> labels;  // Partial-level labels.
        PartialEnvelopes envelopes;       // Envelopes for amplitude, frequency, and phase.

        // Constructor: a Partial must be constructed with its labels and envelopes.
        Partial(const std::vector<std::string>& labels, const PartialEnvelopes& envelopes)
            : labels(labels), envelopes(envelopes) {}
    };

    std::vector<Partial> partials;
};

// Parser class to read a YAML file and generate an AudioFragment.
class AudioFragmentParser {
   public:
    // Constructor that takes a filename.
    explicit AudioFragmentParser(const std::string& filename);

    // Parse the YAML file and return an AudioFragment.
    AudioFragment parse();

   private:
    YAML::Node root;  // The root YAML node loaded from file.

    // Helper functions for parsing sub-components.
    std::vector<std::string> parseStringVector(const YAML::Node& node);
    std::vector<double> parseDoubleVector(const YAML::Node& node);
    std::vector<EnvelopeCurvePoint> parseCurveVector(const YAML::Node& node);

    FrequencyEnvelope parseFrequencyEnvelope(const YAML::Node& node);
    AmplitudeEnvelope parseAmplitudeEnvelope(const YAML::Node& node);
    PhaseCoordinates parsePhaseCoordinates(const YAML::Node& node);
};

}  // namespace RAINBOHz

#endif  // AUDIO_FRAGMENT_PARSER_H
