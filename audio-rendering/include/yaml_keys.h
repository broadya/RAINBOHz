#ifndef YAML_KEYS_H
#define YAML_KEYS_H

#include <string_view>

namespace RAINBOHz {
inline constexpr std::string_view AUDIO_FRAGMENT_KEY = "audio_fragment";
inline constexpr std::string_view START_TIME_KEY = "start_time";
inline constexpr std::string_view LABELS_KEY = "labels";
inline constexpr std::string_view PARTIALS_KEY = "partials";
inline constexpr std::string_view PARTIAL_KEY = "partial";
inline constexpr std::string_view FREQ_ENV_KEY = "frequency_envelope";
inline constexpr std::string_view AMP_ENV_KEY = "amplitude_envelope";
inline constexpr std::string_view TIMES_KEY = "times";
inline constexpr std::string_view LEVELS_KEY = "levels";
inline constexpr std::string_view CURVES_KEY = "curves";
inline constexpr std::string_view PHASE_COORDS_KEY = "phase_coordinates";
inline constexpr std::string_view PHASES_KEY = "phases";
}  // namespace RAINBOHz

#endif  // YAML_KEYS_H
