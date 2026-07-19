#include "stdafx.h"
#include "multiroom_component_state.h"

#include <helpers/album_art_helpers.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace {

std::string metadata_value(const file_info& info, const char* name) {
    pfc::string8 formatted;
    if (!info.meta_format(name, formatted, ", ")) {
        return {};
    }
    return formatted.c_str();
}

uint32_t parse_tag_number(const std::string& value) {
    if (value.empty()) return 0;
    char* end = nullptr;
    const auto parsed = std::strtoul(value.c_str(), &end, 10);
    if (end == value.c_str()) return 0;
    return static_cast<uint32_t>((std::min)(
        parsed,
        static_cast<unsigned long>((std::numeric_limits<uint32_t>::max)())));
}

uint64_t milliseconds_from_seconds(double value) {
    if (!std::isfinite(value) || value <= 0) return 0;
    const auto milliseconds = value * 1000.0;
    if (milliseconds >= static_cast<double>((std::numeric_limits<uint64_t>::max)())) {
        return (std::numeric_limits<uint64_t>::max)();
    }
    return static_cast<uint64_t>(std::llround(milliseconds));
}

uint64_t current_playback_duration_ms() {
    try {
        return milliseconds_from_seconds(playback_control::get()->playback_get_length_ex());
    } catch (const std::exception&) {
        return 0;
    }
}

void apply_info(multiroom::PlaybackMetadata& metadata, const file_info& info, bool replace_empty) {
    const auto assign = [replace_empty](std::string& target, std::string value) {
        if (replace_empty || !value.empty()) {
            target = std::move(value);
        }
    };

    assign(metadata.title, metadata_value(info, "title"));
    assign(metadata.artist, metadata_value(info, "artist"));
    assign(metadata.album, metadata_value(info, "album"));
    assign(metadata.album_artist, metadata_value(info, "album artist"));
    assign(metadata.composer, metadata_value(info, "composer"));
    assign(metadata.genre, metadata_value(info, "genre"));

    const auto assign_number = [replace_empty](uint32_t& target, uint32_t value) {
        if (replace_empty || value != 0) {
            target = value;
        }
    };
    assign_number(metadata.track_number, parse_tag_number(metadata_value(info, "tracknumber")));
    assign_number(metadata.disc_number, parse_tag_number(metadata_value(info, "discnumber")));
    assign_number(metadata.year, parse_tag_number(metadata_value(info, "date")));

    const auto duration = milliseconds_from_seconds(info.get_length());
    if (replace_empty || duration != 0) {
        metadata.duration_ms = duration;
    }
}

std::string fallback_title(const metadb_handle_ptr& track) {
    if (track.is_empty()) return {};
    pfc::string8 formatted;
    if (!track->format_title_legacy(nullptr, formatted, "$if2(%title%,%filename%)", nullptr)) {
        return {};
    }
    return formatted.c_str();
}

class PlaybackMetadataBridge final
    : public play_callback
    , public now_playing_album_art_notify {
public:
    static constexpr unsigned callback_flags =
        play_callback::flag_on_playback_starting |
        play_callback::flag_on_playback_new_track |
        play_callback::flag_on_playback_stop |
        play_callback::flag_on_playback_seek |
        play_callback::flag_on_playback_pause |
        play_callback::flag_on_playback_edited |
        play_callback::flag_on_playback_dynamic_info_track |
        play_callback::flag_on_playback_time;

    void on_playback_starting(play_control::t_track_command, bool) override {
        active_ = false;
        metadata_ = {};
        MultiroomComponentState::instance().clear_playback_metadata();
    }

    void on_playback_new_track(metadb_handle_ptr track) override {
        active_ = true;
        metadata_ = {};

        if (track.is_valid()) {
            file_info_impl info;
            if (track->get_browse_info_merged(info)) {
                apply_info(metadata_, info, true);
            }
            if (metadata_.duration_ms == 0) {
                metadata_.duration_ms = milliseconds_from_seconds(track->get_length());
            }
            if (metadata_.title.empty()) {
                metadata_.title = fallback_title(track);
            }
        }

        // playback_get_length_ex() also consults dynamic playback information.
        // This covers inputs whose metadb/file_info length is not populated at
        // the first new-track callback even though foobar knows the duration.
        const auto playback_duration = current_playback_duration_ms();
        if (playback_duration != 0) {
            metadata_.duration_ms = playback_duration;
        }

        MultiroomComponentState::instance().set_playback_metadata(metadata_);
    }

    void on_playback_stop(play_control::t_stop_reason) override {
        active_ = false;
        metadata_ = {};
        MultiroomComponentState::instance().clear_playback_metadata();
    }

    void on_playback_seek(double time) override {
        if (!active_) return;
        metadata_.position_ms = milliseconds_from_seconds(time);
        MultiroomComponentState::instance().set_playback_metadata(metadata_);
    }

    void on_playback_pause(bool paused) override {
        if (!active_) return;
        metadata_.paused = paused;
        MultiroomComponentState::instance().set_playback_metadata(metadata_);
    }

    void on_playback_edited(metadb_handle_ptr track) override {
        if (!active_ || track.is_empty()) return;

        file_info_impl info;
        if (track->get_browse_info_merged(info)) {
            const auto artwork = metadata_.artwork;
            const auto artwork_mime = metadata_.artwork_mime;
            const auto duration_ms = metadata_.duration_ms;
            const auto position_ms = metadata_.position_ms;
            const auto paused = metadata_.paused;
            apply_info(metadata_, info, true);
            if (metadata_.duration_ms == 0) {
                metadata_.duration_ms = current_playback_duration_ms();
            }
            if (metadata_.duration_ms == 0) {
                metadata_.duration_ms = duration_ms;
            }
            metadata_.artwork = artwork;
            metadata_.artwork_mime = artwork_mime;
            metadata_.position_ms = position_ms;
            metadata_.paused = paused;
            MultiroomComponentState::instance().set_playback_metadata(metadata_);
        }
    }

    void on_playback_dynamic_info(const file_info&) override {}

    void on_playback_dynamic_info_track(const file_info& info) override {
        if (!active_) return;
        apply_info(metadata_, info, false);
        MultiroomComponentState::instance().set_playback_metadata(metadata_);
    }

    void on_playback_time(double time) override {
        if (!active_) return;
        metadata_.position_ms = milliseconds_from_seconds(time);
        const auto playback_duration = current_playback_duration_ms();
        if (playback_duration != 0 && playback_duration != metadata_.duration_ms) {
            // Some decoders expose their length only after playback has begun.
            // Resend the complete snapshot once so the receiver obtains a real
            // progress end timestamp instead of current/current.
            metadata_.duration_ms = playback_duration;
            MultiroomComponentState::instance().set_playback_metadata(metadata_);
        } else {
            MultiroomComponentState::instance().update_playback_position(metadata_.position_ms);
        }
    }

    void on_volume_change(float) override {}

    void on_album_art(album_art_data::ptr data) override {
        if (!active_) return;

        metadata_.artwork.clear();
        metadata_.artwork_mime.clear();
        if (data.is_valid() && data->data() != nullptr && data->size() != 0) {
            if (album_art_helpers::isWebP(data)) {
                try {
                    data = album_art_helpers::encodeJPEG(data, 88);
                } catch (const std::exception&) {
                    data.release();
                }
            }

            if (data.is_valid() && album_art_helpers::isJPEG(data)) {
                metadata_.artwork_mime = "image/jpeg";
            } else if (data.is_valid() && album_art_helpers::isPNG(data)) {
                metadata_.artwork_mime = "image/png";
            }

            if (!metadata_.artwork_mime.empty()) {
                const auto* begin = static_cast<const uint8_t*>(data->data());
                metadata_.artwork.assign(begin, begin + data->size());
            }
        }

        MultiroomComponentState::instance().set_playback_metadata(metadata_);
    }

private:
    multiroom::PlaybackMetadata metadata_;
    bool active_ = false;
};

class PlaybackMetadataLifecycle : public initquit {
public:
    void on_init() override {
        bridge_ = std::make_unique<PlaybackMetadataBridge>();
        auto artwork_manager = now_playing_album_art_notify_manager::get();
        artwork_manager->add(bridge_.get());
        play_callback_manager::get()->register_callback(
            bridge_.get(),
            PlaybackMetadataBridge::callback_flags,
            true);

        const auto artwork = artwork_manager->current();
        if (artwork.is_valid()) {
            bridge_->on_album_art(artwork);
        }
    }

    void on_quit() override {
        if (!bridge_) return;
        play_callback_manager::get()->unregister_callback(bridge_.get());
        now_playing_album_art_notify_manager::get()->remove(bridge_.get());
        bridge_.reset();
    }

private:
    std::unique_ptr<PlaybackMetadataBridge> bridge_;
};

static initquit_factory_t<PlaybackMetadataLifecycle> g_playback_metadata_lifecycle_factory;

}  // namespace
