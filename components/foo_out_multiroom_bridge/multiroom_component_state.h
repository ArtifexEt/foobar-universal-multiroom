#pragma once

#include "core/multiroom_engine.h"
#include "../../transports/airplay/airplay_transport.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

class MultiroomComponentState {
public:
    static MultiroomComponentState& instance();
    ~MultiroomComponentState();

    void refresh_outputs();
    bool refresh_in_progress();
    bool control_in_progress();
    std::vector<multiroom::OutputDevice> outputs();
    void pair_output(const std::string& id, const std::string& pin);
    bool pairing_in_progress();
    void toggle_output(const std::string& id);
    void set_output_volume(const std::string& id, int volume);
    void set_master_volume_percent(int volume);
    void set_playback_metadata(const multiroom::PlaybackMetadata& metadata);
    void update_playback_position(uint64_t position_ms);
    void clear_playback_metadata();
    void cancel_pending_playback_open();
    void open_playback_stream(const multiroom::PcmFormat& format);
    void write_playback_pcm(const void* frames, size_t bytes);
    void flush_playback();
    void stop_playback();
    void report_playback_failure(const std::string& message);
    std::wstring playback_destination_label();
    std::wstring status_text();

private:
    MultiroomComponentState();

    void ensure_discovery_started();
    void refresh_outputs_worker();
    void refresh_outputs_for_playback();
    void schedule_control_update();
    void schedule_volume_update(const std::vector<std::string>& ids);
    void schedule_metadata_update();
    void control_update_worker();
    void pairing_worker(std::string id, std::string pin);
    void handle_remote_command(
        const std::string& output_id,
        const multiroom::airplay::AirPlayRemoteCommandEvent& event);

    std::mutex mutex_;
    std::mutex transport_mutex_;
    multiroom::airplay::AirPlayTransport transport_;
    std::unique_ptr<multiroom::MultiroomEngine> playback_engine_;
    multiroom::PcmFormat playback_format_;
    std::thread refresh_thread_;
    std::thread control_thread_;
    std::thread pairing_thread_;
    std::vector<multiroom::OutputDevice> cached_outputs_;
    std::vector<std::wstring> active_output_names_;
    bool discovery_started_ = false;
    bool refresh_in_progress_ = false;
    bool refresh_requested_ = false;
    bool control_in_progress_ = false;
    bool control_full_update_requested_ = false;
    std::set<std::string> control_volume_update_ids_;
    multiroom::PlaybackMetadata playback_metadata_;
    bool playback_metadata_active_ = false;
    bool control_metadata_update_requested_ = false;
    bool pairing_in_progress_ = false;
    bool playback_format_valid_ = false;
    bool playback_connecting_ = false;
    int master_volume_percent_ = 100;
    std::atomic_bool playback_open_ = false;
    std::atomic_bool playback_open_cancel_requested_ = false;
    std::atomic_bool playback_failure_stop_queued_ = false;
    std::atomic_bool shutting_down_ = false;
    std::deque<std::string> recent_remote_command_ids_;
    size_t refresh_count_ = 0;
    std::wstring last_error_;
};
