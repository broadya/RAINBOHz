#include "parser_audio_fragment.h"

#include <algorithm>  // for std::none_of and std::ranges::transform
#include <cassert>
#include <iostream>
#include <stdexcept>

#include "yaml_keys.h"  // Include the YAML key constants

using namespace RAINBOHz;

AudioFragmentParser::AudioFragmentParser(const std::string& filename) {
    try {
        root = YAML::LoadFile(filename);
    } catch (const YAML::Exception& ex) {
        throw std::runtime_error("Error loading YAML file: " + std::string(ex.what()));
    }
}

AudioFragment AudioFragmentParser::parse() {
    YAML::Node fragmentNode = root[RAINBOHz::AUDIO_FRAGMENT_KEY.data()];
    if (!fragmentNode) {
        throw std::invalid_argument("Missing '" + std::string(RAINBOHz::AUDIO_FRAGMENT_KEY) +
                                    "' key in YAML file.");
    }

    AudioFragment audioFragment;
    audioFragment.start_time = fragmentNode[RAINBOHz::START_TIME_KEY.data()].as<double>();
    audioFragment.labels = parseStringVector(fragmentNode[RAINBOHz::LABELS_KEY.data()]);

    YAML::Node partialsNode = fragmentNode[RAINBOHz::PARTIALS_KEY.data()];
    if (!partialsNode || !partialsNode.IsSequence()) {
        throw std::invalid_argument("'" + std::string(RAINBOHz::PARTIALS_KEY) +
                                    "' must be a sequence in the YAML file.");
    }

    for (const auto& p : partialsNode) {
        YAML::Node partialNode = p[RAINBOHz::PARTIAL_KEY.data()];
        if (!partialNode) {
            throw std::invalid_argument("Missing '" + std::string(RAINBOHz::PARTIAL_KEY) +
                                        "' key in one of the partial entries.");
        }
        // Parse the partial's labels.
        std::vector<std::string> partialLabels =
            parseStringVector(partialNode[RAINBOHz::LABELS_KEY.data()]);

        // Parse frequency envelope.
        FrequencyEnvelope freqEnv =
            parseFrequencyEnvelope(partialNode[RAINBOHz::FREQ_ENV_KEY.data()]);

        // Parse amplitude envelope.
        AmplitudeEnvelope ampEnv =
            parseAmplitudeEnvelope(partialNode[RAINBOHz::AMP_ENV_KEY.data()]);

        // Parse phase coordinates.
        PhaseCoordinates phaseCoords =
            parsePhaseCoordinates(partialNode[RAINBOHz::PHASE_COORDS_KEY.data()]);

        // Create the PartialEnvelopes.
        PartialEnvelopes envelopes(ampEnv, freqEnv, phaseCoords);

        // Emplace a new Partial into the vector.
        audioFragment.partials.emplace_back(partialLabels, envelopes);
    }
    return audioFragment;
}

// Helper: Parse a YAML sequence of strings.
std::vector<std::string> AudioFragmentParser::parseStringVector(const YAML::Node& node) {
    std::vector<std::string> result;
    if (!node || !node.IsSequence()) {
        return result;
    }
    for (const auto& item : node) {
        result.push_back(item.as<std::string>());
    }
    return result;
}

// Helper: Parse a YAML sequence of doubles.
std::vector<double> AudioFragmentParser::parseDoubleVector(const YAML::Node& node) {
    std::vector<double> result;
    if (!node || !node.IsSequence()) {
        return result;
    }
    for (const auto& item : node) {
        result.push_back(item.as<double>());
    }
    return result;
}

// Helper: Parse the curves vector.
// For now, numeric curve values are handled but only basic mapping is done.
// Future functionality for full curve support can be added here.
std::vector<EnvelopeCurvePoint> AudioFragmentParser::parseCurveVector(const YAML::Node& node) {
    std::vector<EnvelopeCurvePoint> curves;
    if (!node || !node.IsSequence()) return curves;

    for (const auto& item : node) {
        if (item.IsScalar()) {
            try {
                std::string curveStr = item.as<std::string>();
                if (curveStr == "lin") {
                    curves.emplace_back(EnvelopeCurveType::lin);
                } else if (curveStr == "exp") {
                    curves.emplace_back(EnvelopeCurveType::exp);
                } else if (curveStr == "sine") {
                    curves.emplace_back(EnvelopeCurveType::sine);
                } else if (curveStr == "welch") {
                    curves.emplace_back(EnvelopeCurveType::welch);
                } else if (curveStr == "step") {
                    curves.emplace_back(EnvelopeCurveType::step);
                } else {
                    throw std::invalid_argument("Unknown envelope curve type: " + curveStr);
                }
            } catch (const YAML::BadConversion&) {
                // If not a string, treat it as a numeric value.
                double numericVal = item.as<double>();
                curves.emplace_back(numericVal);
            }
        } else if (item.IsNull()) {
            throw std::invalid_argument("Null value encountered in curves array.");
        } else {
            throw std::invalid_argument("Unexpected YAML node type in curves array.");
        }
    }
    return curves;
}

// Helper: Parse a FrequencyEnvelope node.
FrequencyEnvelope AudioFragmentParser::parseFrequencyEnvelope(const YAML::Node& node) {
    if (!node) {
        throw std::invalid_argument("Missing '" + std::string(RAINBOHz::FREQ_ENV_KEY) + "' node.");
    }
    std::vector<double> levels = parseDoubleVector(node[RAINBOHz::LEVELS_KEY.data()]);
    std::vector<double> times = parseDoubleVector(node[RAINBOHz::TIMES_KEY.data()]);
    std::vector<EnvelopeCurvePoint> curves = parseCurveVector(node[RAINBOHz::CURVES_KEY.data()]);

    // Enforce that times vector has at least levels.size()-1 elements.
    if (times.size() < levels.size() - 1) {
        throw std::invalid_argument("Frequency envelope '" + std::string(RAINBOHz::TIMES_KEY) +
                                    "' array has insufficient elements.");
    }
    return FrequencyEnvelope(levels, times, curves);
}

// Helper: Parse an AmplitudeEnvelope node.
AmplitudeEnvelope AudioFragmentParser::parseAmplitudeEnvelope(const YAML::Node& node) {
    if (!node) {
        throw std::invalid_argument("Missing '" + std::string(RAINBOHz::AMP_ENV_KEY) + "' node.");
    }
    std::vector<double> levels = parseDoubleVector(node[RAINBOHz::LEVELS_KEY.data()]);
    std::vector<double> times = parseDoubleVector(node[RAINBOHz::TIMES_KEY.data()]);
    std::vector<EnvelopeCurvePoint> curves = parseCurveVector(node[RAINBOHz::CURVES_KEY.data()]);

    if (times.size() < levels.size() - 1) {
        throw std::invalid_argument("Amplitude envelope '" + std::string(RAINBOHz::TIMES_KEY) +
                                    "' array has insufficient elements.");
    }
    return AmplitudeEnvelope(levels, times, curves);
}

// Helper: Parse the phase_coordinates node.
PhaseCoordinates AudioFragmentParser::parsePhaseCoordinates(const YAML::Node& node) {
    if (!node) {
        throw std::invalid_argument("Missing '" + std::string(RAINBOHz::PHASE_COORDS_KEY) +
                                    "' node.");
    }
    std::vector<double> times = parseDoubleVector(node[RAINBOHz::TIMES_KEY.data()]);
    YAML::Node phasesNode = node[RAINBOHz::PHASES_KEY.data()];
    if (!phasesNode || !phasesNode.IsSequence()) {
        throw std::invalid_argument("Phase coordinates '" + std::string(RAINBOHz::PHASES_KEY) +
                                    "' must be a sequence.");
    }
    if (phasesNode.size() != times.size()) {
        throw std::invalid_argument("The '" + std::string(RAINBOHz::TIMES_KEY) + "' and '" +
                                    std::string(RAINBOHz::PHASES_KEY) +
                                    "' arrays in phase_coordinates must have the same length.");
    }

    std::vector<PhaseCoordinate> coordinates;
    constexpr double TWO_PI = 6.283185307179586;
    for (std::size_t i = 0; i < times.size(); ++i) {
        double t = times[i];
        const YAML::Node phaseNode = phasesNode[i];
        if (phaseNode.IsNull()) {
            // Use the natural phase constructor.
            coordinates.push_back(PhaseCoordinate(t));
        } else {
            double phaseVal = phaseNode.as<double>();
            if (phaseVal < 0.0 || phaseVal > TWO_PI) {
                throw std::invalid_argument("Phase value " + std::to_string(phaseVal) +
                                            " at time " + std::to_string(t) +
                                            " is out of range [0,2π].");
            }
            coordinates.push_back(PhaseCoordinate(t, phaseVal));
        }
    }
    // Construct PhaseCoordinates (its constructor enforces ordering and invariants).
    return PhaseCoordinates(coordinates);
}

// --- Optional Main Function for Testing ---
// Uncomment the following main() function if you wish to test the parser directly.
/*
#include <iostream>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: audio_parser <filename.yaml>\n";
        return 1;
    }

    try {
        AudioFragmentParser parser(argv[1]);
        AudioFragment fragment = parser.parse();

        std::cout << "Audio fragment start time: " << fragment.start_time << " seconds\n";
        std::cout << "Number of partials: " << fragment.partials.size() << "\n";
    } catch (const std::exception& ex) {
        std::cerr << "Parsing error: " << ex.what() << "\n";
        return 1;
    }

    return 0;
}
*/
