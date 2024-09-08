#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

#include "AudioTypes.h"
#include "MultiPaxelGenerator.h"
#include "PartialGenerator.h"
#include "PaxelGenerator.h"
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

/*
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
*/

/*
int main(int argc, char* argv[]) {
    RAINBOHz::PaxelSpecification paxelSpecification{
        1000.0, 97.654, 0.5, 0.8, RAINBOHz::HALF_PI, RAINBOHz::ONE_AND_HALF_PI, 300000, 0, 299999};

    // Generate paxel test
    RAINBOHz::PaxelGenerator generator(paxelSpecification);
    auto samples = generator.generatePaxel();

    // Write samples to WAV file
    RAINBOHz::WavWriter writer(RAINBOHz::kSampleRate);
    if (writer.writeToFile("paxeltest.wav", samples)) {
        std::cout << "WAV file generated successfully: " << std::endl;
    } else {
        std::cerr << "Failed to write WAV file.\n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
*/

int testMultiPaxel() {
    RAINBOHz::PaxelSpecification paxelSpecification1{
        1000.0, 97.654, 0.5, 0.8, RAINBOHz::HALF_PI, RAINBOHz::ONE_AND_HALF_PI, 300000, 0, 100000};
    RAINBOHz::PaxelSpecification paxelSpecification2{
        97.654, 2000.0, 0.8, 0.2, RAINBOHz::ONE_AND_HALF_PI, 0.0, 300000, 100001, 299999};

    std::vector<RAINBOHz::PaxelSpecification> multiPaxelSpecification{paxelSpecification1,
                                                                      paxelSpecification2};

    // Generate paxel test
    RAINBOHz::MultiPaxelGenerator generator(multiPaxelSpecification);
    auto samples = generator.generatePaxel();

    // Write samples to WAV file
    RAINBOHz::WavWriter writer(RAINBOHz::kSampleRate);
    if (writer.writeToFile("paxeltest.wav", samples)) {
        std::cout << "WAV file generated successfully: " << std::endl;
    } else {
        std::cerr << "Failed to write WAV file.\n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

int testPartial() {
    RAINBOHz::PaxelSpecification paxelSpecification0{
        20.0, 1000.0, 0.5, 0.5, RAINBOHz::ZERO_PI, RAINBOHz::HALF_PI, 300000, 0, 299999};
    RAINBOHz::PaxelSpecification paxelSpecification1{
        1000.0, 97.654, 0.5, 0.8, RAINBOHz::HALF_PI, RAINBOHz::ONE_AND_HALF_PI, 300000, 0, 100000};
    RAINBOHz::PaxelSpecification paxelSpecification2{
        97.654, 2000.0, 0.8,   0.2, RAINBOHz::ONE_AND_HALF_PI, RAINBOHz::ZERO_PI,
        300000, 100001, 299999};
    RAINBOHz::PaxelSpecification paxelSpecification3{
        2000.0, 2000.0, 0.2, 0.5, RAINBOHz::ZERO_PI, RAINBOHz::ZERO_PI, 300000, 0, 299999};

    std::vector<RAINBOHz::PaxelSpecification> multiPaxelSpecification0{paxelSpecification0};
    std::vector<RAINBOHz::PaxelSpecification> multiPaxelSpecification1{paxelSpecification1,
                                                                       paxelSpecification2};
    std::vector<RAINBOHz::PaxelSpecification> multiPaxelSpecification2{paxelSpecification3};

    std::vector<RAINBOHz::MultiPaxelSpecification> partialSpecification{
        multiPaxelSpecification0, multiPaxelSpecification1, multiPaxelSpecification2};
    std::vector<std::string> labels{"Label0", "Label1"};

    // Generate paxel test
    RAINBOHz::PartialGenerator generator(partialSpecification, labels);
    auto samples = generator.generatePartial();

    // Write samples to WAV file
    RAINBOHz::WavWriter writer(RAINBOHz::kSampleRate);
    if (writer.writeToFile("paxeltest.wav", samples)) {
        std::cout << "WAV file generated successfully: " << std::endl;
    } else {
        std::cerr << "Failed to write WAV file.\n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

int main(int argc, char* argv[]) { return testPartial(); }
