#pragma once

#include <algorithm>

namespace multiroom {

inline int effective_output_volume_percent(int master_volume, int speaker_volume) {
    const auto clamped_master = std::clamp(master_volume, 0, 100);
    const auto clamped_speaker = std::clamp(speaker_volume, 0, 100);
    return std::clamp((clamped_master * clamped_speaker + 50) / 100, 0, 100);
}

}  // namespace multiroom
