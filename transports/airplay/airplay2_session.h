#pragma once

#include "transport.h"

#include <cstdint>
#include <string>

namespace multiroom::airplay {

struct AirPlay2SessionRequirements {
    std::string output_id;
    std::string endpoint_host;
    uint16_t endpoint_port = 0;
    bool pair_verify_required = true;
    bool encrypted_media_required = true;
    bool ptp_clock_required = true;
};

class AirPlay2SessionBootstrap {
public:
    static AirPlay2SessionRequirements requirements_for(const OutputDevice& output);
    static void validate_mvp_ready(const OutputDevice& output);
};

}  // namespace multiroom::airplay
