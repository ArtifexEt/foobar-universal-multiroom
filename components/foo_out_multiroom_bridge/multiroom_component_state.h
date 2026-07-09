#pragma once

#include "core/multiroom_engine.h"
#include "../../transports/airplay/airplay_transport.h"

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class MultiroomComponentState {
public:
    static MultiroomComponentState& instance();
    ~MultiroomComponentState();

    void refresh_outputs();
    bool refresh_in_progress();
    std::vector<multiroom::OutputDevice> outputs();
    void toggle_output(const std::string& id);
    void set_output_volume(const std::string& id, int volume);
    void open_playback_stream(const multiroom::PcmFormat& format);
    void write_playback_pcm(const void* frames, size_t bytes);
    void flush_playback();
    void stop_playback();
    std::wstring selected_label();
    std::wstring status_text();

private:
    MultiroomComponentState() = default;

    void ensure_discovery_started();
    void refresh_outputs_worker();

    std::mutex mutex_;
    std::mutex transport_mutex_;
    multiroom::airplay::AirPlayTransport transport_;
    std::unique_ptr<multiroom::MultiroomEngine> playback_engine_;
    std::thread refresh_thread_;
    std::vector<multiroom::OutputDevice> cached_outputs_;
    bool discovery_started_ = false;
    bool refresh_in_progress_ = false;
    bool refresh_requested_ = false;
    bool playback_open_ = false;
    size_t refresh_count_ = 0;
    std::wstring last_error_;
};
