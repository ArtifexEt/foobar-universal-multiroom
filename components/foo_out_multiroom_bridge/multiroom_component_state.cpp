#include "stdafx.h"
#include "multiroom_component_state.h"

#include <sstream>

namespace {

std::wstring widen_utf8(const std::string& text) {
    if (text.empty()) return {};
    const int required = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (required <= 1) return {};

    std::wstring result(static_cast<size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, result.data(), required);
    result.resize(static_cast<size_t>(required - 1));
    return result;
}

std::vector<std::string> selected_ids(const std::vector<multiroom::OutputDevice>& outputs) {
    std::vector<std::string> result;
    for (const auto& output : outputs) {
        if (output.selected) result.push_back(output.id);
    }
    return result;
}

}  // namespace

MultiroomComponentState& MultiroomComponentState::instance() {
    static MultiroomComponentState state;
    return state;
}

void MultiroomComponentState::ensure_discovery_started() {
    if (discovery_started_) return;
    transport_.start_discovery();
    discovery_started_ = true;
}

void MultiroomComponentState::refresh_outputs() {
    std::lock_guard lock(mutex_);
    try {
        ensure_discovery_started();
        cached_outputs_ = transport_.list_outputs();
        last_error_.clear();
    } catch (const std::exception& e) {
        last_error_ = widen_utf8(e.what());
    }
}

std::vector<multiroom::OutputDevice> MultiroomComponentState::outputs() {
    std::lock_guard lock(mutex_);
    return cached_outputs_;
}

void MultiroomComponentState::toggle_output(const std::string& id) {
    std::lock_guard lock(mutex_);
    try {
        ensure_discovery_started();
        auto selected = selected_ids(cached_outputs_);

        const auto it = std::find(selected.begin(), selected.end(), id);
        if (it == selected.end()) {
            selected.push_back(id);
        } else {
            selected.erase(it);
        }

        transport_.set_enabled_outputs(selected);
        cached_outputs_ = transport_.list_outputs();
        last_error_.clear();
    } catch (const std::exception& e) {
        last_error_ = widen_utf8(e.what());
    }
}

std::wstring MultiroomComponentState::selected_label() {
    const auto current_outputs = outputs();

    std::wstring first;
    size_t count = 0;
    for (const auto& output : current_outputs) {
        if (!output.selected) continue;
        if (first.empty()) first = widen_utf8(output.name);
        ++count;
    }

    if (count == 0) return L"No speakers";
    if (count == 1) return first;
    return first + L" +" + std::to_wstring(count - 1);
}

std::wstring MultiroomComponentState::status_text() {
    std::lock_guard lock(mutex_);
    if (!last_error_.empty()) return L"AirPlay discovery error: " + last_error_;
    if (!discovery_started_) return L"AirPlay discovery: not started";

    size_t selected = 0;
    for (const auto& output : cached_outputs_) {
        if (output.selected) ++selected;
    }

    std::wstringstream stream;
    stream << L"AirPlay discovery: " << cached_outputs_.size() << L" speaker";
    if (cached_outputs_.size() != 1) stream << L"s";
    stream << L", " << selected << L" selected";
    return stream.str();
}
