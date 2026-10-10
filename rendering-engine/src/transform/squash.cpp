// SPDX-License-Identifier: Apache-2.0

#include "rainbohz/rendering_engine/transform/squash.hpp"

#include <cstddef>
#include <vector>

/// @file
/// @brief The squash rule: semantic order over physical time.
///
/// Implements docs/transform.md section 6.3. The input times are copied so
/// that the compression step can rewrite the stretch after a collision; the
/// loop then continues over the rewritten times, which is what makes a
/// collision inside a compressed stretch fall under the same rule.
///
/// @see squash.hpp

namespace rainbohz::rendering_engine::transform {

std::vector<Placement> squash(std::span<const SampleTime> times) {
    std::vector<SampleTime> t(times.begin(), times.end());
    std::vector<Placement> placed;
    placed.reserve(t.size());
    const std::size_t n = t.size();

    std::size_t i = 0;
    while (i < n) {
        const bool atFloor = placed.empty();
        const SampleTime tail = atFloor ? SampleTime{0.0} : placed.back().time;

        // 1. In order: later than the tail, or at or after the floor when
        //    nothing has been placed yet.
        if (atFloor ? t[i] >= tail : t[i] > tail) {
            placed.push_back(Placement{.index = i, .time = t[i]});
            ++i;
            continue;
        }

        // 2. A collision. The run that keeps stepping backwards collapses to
        //    its last member, which takes the tail's time and place.
        std::size_t j = i;
        while (j + 1 < n && t[j + 1] <= t[j]) {
            ++j;
        }
        if (atFloor) {
            placed.push_back(Placement{.index = j, .time = tail});
        } else {
            placed.back() = Placement{.index = j, .time = tail};
        }

        // 3. Compress the stretch up to the next point later than the tail
        //    into the room left: [t[j], t[m]] maps linearly onto [tail, t[m]].
        //    With no such point nothing is compressed, and every remaining
        //    point collides in turn on the next iterations.
        std::size_t m = j + 1;
        while (m < n && t[m] <= tail) {
            ++m;
        }
        if (m < n && m > j + 1) {
            const double from = t[j].value();
            const double scale = (t[m] - tail) / (t[m] - t[j]);
            for (std::size_t p = j + 1; p < m; ++p) {
                t[p] = tail + SampleDuration{(t[p].value() - from) * scale};
            }
        }
        i = j + 1;
    }
    return placed;
}

} // namespace rainbohz::rendering_engine::transform
