#include <cstdlib>
#include <iostream>
#include <string>

#include "AudioTypes.h"
#include "SineWaveGenerator.h"
#include "WavWriter.h"

void printUsage(const std::string& programName) {
    std::cout << "Usage: " << programName << " [options]\n"
              << "Options:\n"
              << "  -f --frequency <frequency in Hz>    (default: 440.0)\n"
              << "  -a --amplitude <amplitude 0.0-1.0> (default: 0.5)\n"
              << "  -d --duration <duration in seconds> (default: 2.0)\n"
              << "  -o --output <output filename>       (default: output.wav)\n"
              << "  -s --samplerate <samples per second> (default: 44100)\n";
}

int main(int argc, char* argv[]) {
    double frequency = 660.0;
    double amplitude = 0.5;
    double duration = 2.0;
    std::string outputFilename = "output.wav";
    uint32_t sampleRate = RAINBOHz::kSampleRate;

    // Simple command-line parsing
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "-f" || arg == "--frequency") && i + 1 < argc) {
            frequency = std::stod(argv[++i]);
        } else if ((arg == "-a" || arg == "--amplitude") && i + 1 < argc) {
            amplitude = std::stod(argv[++i]);
        } else if ((arg == "-d" || arg == "--duration") && i + 1 < argc) {
            duration = std::stod(argv[++i]);
        } else if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
            outputFilename = argv[++i];
        } else if ((arg == "-s" || arg == "--samplerate") && i + 1 < argc) {
            sampleRate = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else {
            printUsage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    // Validate inputs
    if (amplitude < 0.0 || amplitude > 1.0) {
        std::cerr << "Amplitude must be between 0.0 and 1.0\n";
        return EXIT_FAILURE;
    }
    if (frequency <= 0.0) {
        std::cerr << "Frequency must be positive\n";
        return EXIT_FAILURE;
    }
    if (duration <= 0.0) {
        std::cerr << "Duration must be positive\n";
        return EXIT_FAILURE;
    }

    // Generate sine wave samples
    RAINBOHz::SineWaveGenerator generator(frequency, amplitude, duration, sampleRate);
    auto samples = generator.generateWave();

    // Write samples to WAV file
    RAINBOHz::WavWriter writer(sampleRate);
    if (writer.writeToFile(outputFilename, samples)) {
        std::cout << "WAV file generated successfully: " << outputFilename << std::endl;
    } else {
        std::cerr << "Failed to write WAV file.\n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
