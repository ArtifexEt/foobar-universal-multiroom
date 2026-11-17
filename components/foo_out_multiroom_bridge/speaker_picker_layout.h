#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace multiroom::ui {

struct SpeakerPickerLayout {
    static constexpr int margin_dip = 8;

    int scale_percent = 100;
    int work_width = 0;
    int work_height = 0;

    constexpr int px(int dip) const {
        const auto scaled = static_cast<std::int64_t>(dip) * scale_percent;
        return static_cast<int>((scaled + (scaled < 0 ? -50 : 50)) / 100);
    }

    constexpr int width() const {
        return (std::max)(0, (std::min)(px(640), work_width - 2 * px(margin_dip)));
    }

    constexpr bool stack_controls(int client_width) const {
        return client_width < px(515);
    }

    constexpr int row_height(int client_width) const {
        return px(stack_controls(client_width) ? 154 : 116);
    }

    constexpr std::size_t visible_rows(
        std::size_t count,
        int fixed_height,
        int client_width,
        int row_extra = 0) const {
        const auto available = static_cast<std::int64_t>(work_height) -
            2 * px(margin_dip) - (std::max)(0, fixed_height);
        const auto row = static_cast<std::int64_t>(row_height(client_width)) +
            (std::max)(0, row_extra);
        if (available <= 0 || row <= 0) return 0;
        return (std::min)(count, static_cast<std::size_t>(available / row));
    }
};

}  // namespace multiroom::ui
