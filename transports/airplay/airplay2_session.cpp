#include "airplay2_session.h"

#include <stdexcept>

namespace multiroom::airplay {

AirPlay2SessionRequirements AirPlay2SessionBootstrap::requirements_for(const OutputDevice& output) {
    AirPlay2SessionRequirements requirements;
    requirements.output_id = output.id;
    requirements.endpoint_host = output.endpoint_host;
    requirements.endpoint_port = output.endpoint_port;
    requirements.pair_verify_required = output.requires_auth || output.needs_auth_key || output.requires_encrypted_stream;
    requirements.encrypted_media_required = output.requires_encrypted_stream;
    requirements.ptp_clock_required = false;
    return requirements;
}

void AirPlay2SessionBootstrap::validate_mvp_ready(const OutputDevice& output) {
    if (!output.supports_airplay2) {
        throw std::runtime_error("AirPlay 2 is required for the multiroom MVP: " + output.id);
    }
    if (output.endpoint_host.empty() || output.endpoint_port == 0) {
        throw std::invalid_argument("AirPlay 2 output has no stream endpoint: " + output.id);
    }

    static_cast<void>(requirements_for(output));
}

}  // namespace multiroom::airplay
