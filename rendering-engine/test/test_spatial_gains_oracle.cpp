// Checks positionGains() against the Python oracle, row by row.
//
// The golden data is oracle/spatial_gains.csv, a copy of
// proof-of-concept/spatial-audio/cpp/test/oracle/oracle_gains.csv, written by
// that directory's export_oracle.py from the spatial_poc Python package.
// When the oracle changes, regenerate the original (see the spatial-audio
// README) and copy it here.

#include "rainbohz/rendering_engine/spatial_gains.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace rainbohz::rendering_engine {
namespace {

constexpr double kTol = 1e-12;
constexpr std::size_t kFields = 5 + kMaxChannels;

struct OracleRow {
    std::string line;
    ChannelMode mode = ChannelMode::Mono;
    SpatialPosition position{.azimuth = Azimuth{0.0},
        .elevation = Elevation{0.0},
        .distance = Metres{1.0}};
    std::vector<double> gains;
};

std::vector<std::string> splitCsvLine(const std::string& line) {
    std::vector<std::string> fields;
    std::stringstream stream{line};
    std::string field;
    while (std::getline(stream, field, ',')) {
        fields.push_back(field);
    }
    if (!line.empty() && line.back() == ',') {
        fields.emplace_back();
    }
    return fields;
}

OracleRow parseRow(const std::string& line) {
    const std::vector<std::string> f = splitCsvLine(line);
    if (f.size() != kFields) {
        throw std::runtime_error("expected " + std::to_string(kFields) + " fields: " + line);
    }
    OracleRow row;
    row.line = line;
    row.mode = channelModeFromInt(static_cast<std::int8_t>(std::stoi(f[0])));
    row.position = {.azimuth = Azimuth{std::stod(f[1])},
        .elevation = Elevation{std::stod(f[2])},
        .distance = Metres{std::stod(f[3])}};
    const auto channels = static_cast<std::size_t>(std::stoul(f[4]));
    for (std::size_t c = 0; c < channels; ++c) {
        row.gains.push_back(std::stod(f[5 + c]));
    }
    return row;
}

std::vector<OracleRow> readOracle() {
    std::ifstream csv{RE_SPATIAL_ORACLE_CSV};
    if (!csv) {
        throw std::runtime_error(std::string{"cannot open "} + RE_SPATIAL_ORACLE_CSV);
    }
    std::string line;
    std::getline(csv, line);
    if (line != "mode,azimuth_rad,elevation_rad,distance_m,channels,g0,g1,g2,g3") {
        throw std::runtime_error("unexpected header: " + line);
    }
    std::vector<OracleRow> rows;
    while (std::getline(csv, line)) {
        rows.push_back(parseRow(line));
    }
    return rows;
}

TEST(Oracle, CoversEveryMode) {
    const std::vector<OracleRow> rows = readOracle();
    EXPECT_GT(rows.size(), 2000U);
    std::set<ChannelMode> modes;
    for (const OracleRow& row : rows) {
        modes.insert(row.mode);
    }
    EXPECT_EQ(modes.size(), 8U);
}

void expectRowMatches(const OracleRow& row) {
    const ChannelGains g = positionGains(row.mode, row.position);
    ASSERT_EQ(g.count, row.gains.size()) << row.line;
    for (std::size_t c = 0; c < g.count; ++c) {
        EXPECT_NEAR(g.gains[c].value(), row.gains[c], kTol) << "channel " << c << ": " << row.line;
    }
}

TEST(Oracle, EveryPositionIsValid) {
    for (const OracleRow& row : readOracle()) {
        EXPECT_NO_THROW(checkPosition(row.position)) << row.line;
    }
}

TEST(Oracle, EveryRowMatchesThePythonGains) {
    for (const OracleRow& row : readOracle()) {
        expectRowMatches(row);
    }
}

} // namespace
} // namespace rainbohz::rendering_engine
