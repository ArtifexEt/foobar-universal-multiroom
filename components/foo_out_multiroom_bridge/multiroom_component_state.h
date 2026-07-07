#pragma once

#include "../../transports/airplay/airplay_transport.h"

#include <mutex>
#include <string>
#include <vector>

class MultiroomComponentState {
public:
    static MultiroomComponentState& instance();

    void refresh_outputs();
    std::vector<multiroom::OutputDevice> outputs();
    void toggle_output(const std::string& id);
    std::wstring selected_label();
    std::wstring status_text();

private:
    MultiroomComponentState() = default;

    void ensure_discovery_started();

    std::mutex mutex_;
    multiroom::airplay::AirPlayTransport transport_;
    std::vector<multiroom::OutputDevice> cached_outputs_;
    bool discovery_started_ = false;
    std::wstring last_error_;
};
