#pragma once

#include <algorithm>
#include <optional>

namespace multiroom {

inline int effective_output_volume_percent(int master_volume, int speaker_volume) {
    const auto clamped_master = std::clamp(master_volume, 0, 100);
    const auto clamped_speaker = std::clamp(speaker_volume, 0, 100);
    return std::clamp((clamped_master * clamped_speaker + 50) / 100, 0, 100);
}

inline std::optional<int> speaker_volume_from_receiver_percent(int master_volume, int receiver_volume) {
    const auto master = std::clamp(master_volume, 0, 100);
    if (master == 0) {
        return std::nullopt;
    }
    const auto receiver = std::clamp(receiver_volume, 0, 100);
    return std::min(100, (receiver * 100 + master / 2) / master);
}

}  // namespace multiroom
