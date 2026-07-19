#include "stdafx.h"
#include "component_version.h"
#include "multiroom_component_state.h"
#include "preferences_resource.h"
#include "product_identity.h"

#include <algorithm>
#include <array>
#include <cwctype>
#include <sstream>
#include <string>
#include <vector>

#ifndef WM_DPICHANGED_AFTERPARENT
#define WM_DPICHANGED_AFTERPARENT 0x02E3
#endif

namespace {

static constexpr GUID guid_preferences = {
    0x1d6710e5, 0xa768, 0x4d1a, {0x9d, 0x77, 0xce, 0xa4, 0x77, 0x7b, 0x27, 0x01}};
static constexpr COLORREF kDarkBackground = RGB(32, 32, 32);
static constexpr COLORREF kDarkEditBackground = RGB(24, 24, 24);
static constexpr COLORREF kDarkText = RGB(232, 232, 232);
static constexpr UINT_PTR kStatusRefreshTimer = 42000;

enum class Page {
    Status = 0,
    Groups,
    About,
    Count,
};

struct FindChildState {
    int id = 0;
    HWND wnd = nullptr;
};

BOOL CALLBACK find_child_by_id(HWND child, LPARAM param) {
    auto* state = reinterpret_cast<FindChildState*>(param);
    if (state != nullptr && ::GetDlgCtrlID(child) == state->id) {
        state->wnd = child;
        return FALSE;
    }
    return TRUE;
}

HWND find_dlg_item(HWND wnd, int id) {
    if (wnd == nullptr) return nullptr;
    if (HWND direct = ::GetDlgItem(wnd, id); direct != nullptr) return direct;

    FindChildState state{id, nullptr};
    ::EnumChildWindows(wnd, find_child_by_id, reinterpret_cast<LPARAM>(&state));
    return state.wnd;
}

std::wstring widen(const char* text) {
    if (text == nullptr || *text == '\0') return {};
    const int required = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (required <= 1) return {};

    std::wstring result(static_cast<size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, result.data(), required);
    result.resize(static_cast<size_t>(required - 1));
    return result;
}

std::wstring widen_utf8(const std::string& text) {
    return widen(text.c_str());
}

std::string narrow_utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int required = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (required <= 1) return {};
    std::string result(static_cast<size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, result.data(), required, nullptr, nullptr);
    result.resize(static_cast<size_t>(required - 1));
    return result;
}

std::wstring endpoint_text(const multiroom::OutputDevice& output) {
    if (output.endpoint_host.empty() || output.endpoint_port == 0) return L"";
    return widen_utf8(output.endpoint_host) + L":" + std::to_wstring(output.endpoint_port);
}

std::wstring speaker_state_text(const multiroom::OutputDevice& output) {
    if (output.selected) return L"Selected";
    if (output.supports_airplay2) return L"Available";
    if (output.supports_legacy_l16) return L"Legacy";
    if (output.requires_auth) return L"Auth required";
    return L"Unsupported";
}

std::string group_member_outputs_signature(const std::vector<multiroom::OutputDevice>& outputs) {
    std::ostringstream result;
    for (const auto& output : outputs) {
        result << output.id << '|' << output.name << '|' << output.supports_airplay2 << '|';
        for (const auto& alias : output.aliases) result << alias << ',';
        result << ';';
    }
    return result.str();
}

std::wstring text_from_item(HWND wnd, int id) {
    HWND item = find_dlg_item(wnd, id);
    if (item == nullptr) return {};

    const int length = ::GetWindowTextLengthW(item);
    if (length <= 0) return {};

    std::wstring result(static_cast<size_t>(length) + 1, L'\0');
    const int copied = ::GetWindowTextW(item, result.data(), length + 1);
    result.resize(static_cast<size_t>(std::max(copied, 0)));
    return result;
}

std::wstring trim(std::wstring text) {
    size_t first = 0;
    while (first < text.size() && std::iswspace(text[first]) != 0) {
        ++first;
    }
    if (first == text.size()) return {};

    size_t last = text.size();
    while (last > first && std::iswspace(text[last - 1]) != 0) {
        --last;
    }
    return text.substr(first, last - first);
}

int max_int(int left, int right) {
    return left > right ? left : right;
}

SIZE dialog_units_to_pixels(HWND dialog, int horizontal, int vertical) {
    RECT rect = {0, 0, horizontal, vertical};
    if (::MapDialogRect(dialog, &rect) == FALSE) {
        return {horizontal, vertical};
    }
    return {rect.right, rect.bottom};
}

std::string narrow_pin(const std::wstring& text) {
    std::string pin;
    for (wchar_t ch : text) {
        if (std::iswspace(ch) != 0) {
            continue;
        }
        if (ch < L'0' || ch > L'9') {
            return {};
        }
        pin.push_back(static_cast<char>('0' + (ch - L'0')));
    }
    return pin;
}

class preferences_instance : public CDialogImpl<preferences_instance>, public preferences_page_instance {
public:
    enum { IDD = IDD_MULTIROOM_PREFERENCES };

    preferences_instance(preferences_page_callback::ptr callback)
        : callback_(std::move(callback)) {}

    preferences_instance(const preferences_instance&) = delete;
    preferences_instance& operator=(const preferences_instance&) = delete;

    ~preferences_instance() {
        DeleteObject(background_brush_);
        DeleteObject(edit_brush_);
    }

    BEGIN_MSG_MAP_EX(preferences_instance)
        MESSAGE_HANDLER(WM_INITDIALOG, on_init_dialog_message)
        MESSAGE_HANDLER(WM_ERASEBKGND, on_erase_message)
        MESSAGE_HANDLER(WM_COMMAND, on_command_message)
        MESSAGE_HANDLER(WM_TIMER, on_timer_message)
        MESSAGE_HANDLER(WM_SIZE, on_size_message)
        MESSAGE_HANDLER(WM_DPICHANGED, on_dpi_changed_message)
        MESSAGE_HANDLER(WM_DPICHANGED_AFTERPARENT, on_dpi_changed_message)
        MESSAGE_HANDLER(WM_THEMECHANGED, on_theme_changed_message)
        MESSAGE_HANDLER(WM_SETTINGCHANGE, on_theme_changed_message)
        MESSAGE_HANDLER(WM_CTLCOLOREDIT, on_control_color_message)
        MESSAGE_HANDLER(WM_CTLCOLORSTATIC, on_control_color_message)
        MESSAGE_HANDLER(WM_CTLCOLORBTN, on_control_color_message)
        MESSAGE_HANDLER(WM_CTLCOLORDLG, on_control_color_message)
        MESSAGE_HANDLER(WM_NOTIFY, on_notify_message)
    END_MSG_MAP()

    t_uint32 get_state() override {
        return preferences_state::resettable | preferences_state::dark_mode_supported;
    }

    void apply() override {}
    void reset() override {}

private:
    LRESULT on_init_dialog_message(UINT, WPARAM, LPARAM, BOOL&) {
        INITCOMMONCONTROLSEX cc = {};
        cc.dwSize = sizeof(cc);
        cc.dwICC = ICC_TAB_CLASSES | ICC_WIN95_CLASSES | ICC_LISTVIEW_CLASSES;
        InitCommonControlsEx(&cc);

        return on_init(m_hWnd);
    }

    LRESULT on_erase_message(UINT, WPARAM wp, LPARAM, BOOL&) { return on_erase(m_hWnd, reinterpret_cast<HDC>(wp)); }
    LRESULT on_command_message(UINT, WPARAM wp, LPARAM, BOOL&) { return on_command(wp); }
    LRESULT on_timer_message(UINT, WPARAM wp, LPARAM, BOOL&) { return on_timer(wp); }
    LRESULT on_size_message(UINT, WPARAM, LPARAM, BOOL&) { position_pages(); return TRUE; }
    LRESULT on_dpi_changed_message(UINT, WPARAM, LPARAM, BOOL&) { position_pages(); return TRUE; }
    LRESULT on_theme_changed_message(UINT, WPARAM, LPARAM, BOOL&) { redraw(); return TRUE; }
    LRESULT on_control_color_message(UINT msg, WPARAM wp, LPARAM lp, BOOL&) {
        return on_control_color(reinterpret_cast<HDC>(wp), reinterpret_cast<HWND>(lp), msg);
    }
    LRESULT on_notify_message(UINT, WPARAM, LPARAM lp, BOOL&) { return on_notify(reinterpret_cast<NMHDR*>(lp)); }

    static INT_PTR CALLBACK page_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<preferences_instance*>(::GetWindowLongPtrW(wnd, GWLP_USERDATA));
        if (msg == WM_INITDIALOG) {
            self = reinterpret_cast<preferences_instance*>(lp);
            ::SetWindowLongPtrW(wnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            return FALSE;
        }
        if (self == nullptr) return FALSE;

        switch (msg) {
        case WM_ERASEBKGND:
            return self->on_erase(wnd, reinterpret_cast<HDC>(wp));
        case WM_COMMAND:
            return self->on_command(wp);
        case WM_SIZE:
            self->layout_status_page();
            return TRUE;
        case WM_NOTIFY:
            return self->on_notify(reinterpret_cast<NMHDR*>(lp));
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
        case WM_CTLCOLORDLG:
            return self->on_control_color(reinterpret_cast<HDC>(wp), reinterpret_cast<HWND>(lp), msg);
        default:
            return FALSE;
        }
    }

    INT_PTR on_init(HWND wnd) {
        wnd_ = wnd;
        dark_.AddDialogWithControls(wnd_);

        HWND tabs = find_dlg_item(wnd_, idTabs);
        add_tab(tabs, 0, L"Status");
        add_tab(tabs, 1, L"Speaker Groups");
        add_tab(tabs, 2, L"About");

        create_page(Page::Status, IDD_MULTIROOM_PAGE_STATUS);
        create_page(Page::Groups, IDD_MULTIROOM_PAGE_GROUPS);
        create_page(Page::About, IDD_MULTIROOM_PAGE_ABOUT);
        populate_status_page();
        populate_groups_page();
        update_status_page();
        populate_about_page();
        position_pages();
        show_selected_page();
        return FALSE;
    }

    void add_tab(HWND tabs, int index, const wchar_t* label) {
        if (tabs == nullptr) return;
        TCITEMW item = {};
        item.mask = TCIF_TEXT;
        item.pszText = const_cast<wchar_t*>(label);
        TabCtrl_InsertItem(tabs, index, &item);
    }

    void create_page(Page page, int resource_id) {
        HWND page_wnd = CreateDialogParamW(
            core_api::get_my_instance(),
            MAKEINTRESOURCEW(resource_id),
            wnd_,
            page_proc,
            reinterpret_cast<LPARAM>(this));
        if (page_wnd == nullptr) throw std::runtime_error("Could not create multiroom preferences page.");

        page_wnds_[static_cast<size_t>(page)] = page_wnd;
        dark_.AddDialogWithControls(page_wnd);
    }

    void populate_about_page() {
        HWND version = find_dlg_item(wnd_, idAboutVersion);
        if (version != nullptr) {
            const auto text = widen(MULTIROOM_COMPONENT_VERSION);
            ::SetWindowTextW(version, text.c_str());
        }
    }

    void populate_status_page() {
        configure_speaker_list();
    }

    void populate_groups_page() {
        HWND members = find_dlg_item(wnd_, idGroupMemberList);
        if (members != nullptr) {
            ListView_SetExtendedListViewStyle(
                members,
                LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER | LVS_EX_CHECKBOXES);
            while (ListView_DeleteColumn(members, 0)) {
            }
            add_speaker_column(members, 0, L"Speaker", 190);
            add_speaker_column(members, 1, L"Availability", 95);
        }
        reload_group_list({});
    }

    int selected_group_index() const {
        HWND list = find_dlg_item(wnd_, idGroupList);
        if (list == nullptr) return -1;
        const auto selected = static_cast<int>(::SendMessageW(list, LB_GETCURSEL, 0, 0));
        return selected >= 0 && selected < static_cast<int>(speaker_groups_.size()) ? selected : -1;
    }

    void reload_group_list(const std::string& preferred_id) {
        HWND list = find_dlg_item(wnd_, idGroupList);
        if (list == nullptr) return;

        speaker_groups_ = MultiroomComponentState::instance().speaker_groups();
        ::SendMessageW(list, LB_RESETCONTENT, 0, 0);
        int selected = -1;
        for (int index = 0; index < static_cast<int>(speaker_groups_.size()); ++index) {
            const auto name = widen_utf8(speaker_groups_[static_cast<size_t>(index)].name);
            ::SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
            if (speaker_groups_[static_cast<size_t>(index)].id == preferred_id) selected = index;
        }
        if (selected < 0 && !speaker_groups_.empty()) selected = 0;
        ::SendMessageW(list, LB_SETCURSEL, selected, 0);
        if (selected >= 0) {
            load_selected_group();
        } else {
            begin_new_group();
        }
    }

    void begin_new_group() {
        if (HWND list = find_dlg_item(wnd_, idGroupList); list != nullptr) {
            ::SendMessageW(list, LB_SETCURSEL, -1, 0);
        }
        editing_group_id_.clear();
        if (HWND name = find_dlg_item(wnd_, idGroupName); name != nullptr) {
            ::SetWindowTextW(name, L"");
        }
        populate_group_members(nullptr);
        set_group_action_state(false);
        if (HWND name = find_dlg_item(wnd_, idGroupName); name != nullptr) ::SetFocus(name);
    }

    void load_selected_group() {
        const int selected = selected_group_index();
        if (selected < 0) {
            begin_new_group();
            return;
        }
        const auto& group = speaker_groups_[static_cast<size_t>(selected)];
        editing_group_id_ = group.id;
        const auto name = widen_utf8(group.name);
        if (HWND edit = find_dlg_item(wnd_, idGroupName); edit != nullptr) {
            ::SetWindowTextW(edit, name.c_str());
        }
        populate_group_members(&group);
        set_group_action_state(true);
    }

    void set_group_action_state(bool existing_group) {
        if (HWND button = find_dlg_item(wnd_, idGroupDeleteButton); button != nullptr) {
            ::EnableWindow(button, existing_group);
        }
        if (HWND button = find_dlg_item(wnd_, idGroupApplyButton); button != nullptr) {
            ::EnableWindow(button, existing_group);
        }
    }

    static bool group_contains_output(
        const multiroom::SpeakerGroup& group,
        const multiroom::OutputDevice& output) {
        return std::any_of(group.output_ids.begin(), group.output_ids.end(), [&](const auto& id) {
            return id == output.id ||
                std::find(output.aliases.begin(), output.aliases.end(), id) != output.aliases.end();
        });
    }

    void populate_group_members(const multiroom::SpeakerGroup* group) {
        HWND list = find_dlg_item(wnd_, idGroupMemberList);
        if (list == nullptr) return;

        ListView_DeleteAllItems(list);
        group_member_output_ids_.clear();
        const auto outputs = MultiroomComponentState::instance().outputs();
        group_member_outputs_signature_ = group_member_outputs_signature(outputs);
        std::vector<std::string> represented_group_ids;

        for (const auto& output : outputs) {
            const bool member = group != nullptr && group_contains_output(*group, output);
            if (member) {
                for (const auto& id : group->output_ids) {
                    if (id == output.id ||
                        std::find(output.aliases.begin(), output.aliases.end(), id) != output.aliases.end()) {
                        represented_group_ids.push_back(id);
                    }
                }
            }
            add_group_member_row(
                output.id,
                widen_utf8(output.name.empty() ? output.id : output.name),
                output.supports_airplay2 ? L"Available" : L"Unsupported",
                member);
        }

        if (group != nullptr) {
            for (const auto& id : group->output_ids) {
                if (std::find(represented_group_ids.begin(), represented_group_ids.end(), id) != represented_group_ids.end()) {
                    continue;
                }
                add_group_member_row(id, widen_utf8(id), L"Unavailable", true);
            }
        }
    }

    std::vector<std::string> checked_group_member_ids() const {
        std::vector<std::string> result;
        HWND list = find_dlg_item(wnd_, idGroupMemberList);
        if (list == nullptr) return result;
        for (int index = 0; index < static_cast<int>(group_member_output_ids_.size()); ++index) {
            if (ListView_GetCheckState(list, index) != FALSE) {
                result.push_back(group_member_output_ids_[static_cast<size_t>(index)]);
            }
        }
        return result;
    }

    void refresh_group_members_if_outputs_changed() {
        const auto outputs = MultiroomComponentState::instance().outputs();
        const auto signature = group_member_outputs_signature(outputs);
        if (signature == group_member_outputs_signature_) return;

        multiroom::SpeakerGroup edited;
        edited.id = editing_group_id_;
        edited.output_ids = checked_group_member_ids();
        populate_group_members(edited.output_ids.empty() ? nullptr : &edited);
    }

    void add_group_member_row(
        const std::string& id,
        const std::wstring& name,
        const wchar_t* availability,
        bool checked) {
        HWND list = find_dlg_item(wnd_, idGroupMemberList);
        if (list == nullptr) return;
        const int index = static_cast<int>(group_member_output_ids_.size());
        group_member_output_ids_.push_back(id);

        LVITEMW item = {};
        item.mask = LVIF_TEXT;
        item.iItem = index;
        item.pszText = const_cast<wchar_t*>(name.c_str());
        ListView_InsertItem(list, &item);
        ListView_SetItemText(list, index, 1, const_cast<wchar_t*>(availability));
        ListView_SetCheckState(list, index, checked ? TRUE : FALSE);
    }

    void configure_speaker_list() {
        HWND list = find_dlg_item(wnd_, idSpeakerList);
        if (list == nullptr) return;

        ListView_SetExtendedListViewStyle(
            list,
            LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER | LVS_EX_CHECKBOXES);

        while (ListView_DeleteColumn(list, 0)) {
        }

        add_speaker_column(list, 0, L"Name", 156);
        add_speaker_column(list, 1, L"State", 74);
        add_speaker_column(list, 2, L"Endpoint", 110);
        add_speaker_column(list, 3, L"Format", 92);
        size_speaker_columns(list);
    }

    void add_speaker_column(HWND list, int index, const wchar_t* label, int width) {
        LVCOLUMNW column = {};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        column.pszText = const_cast<wchar_t*>(label);
        column.cx = width;
        column.iSubItem = index;
        ListView_InsertColumn(list, index, &column);
    }

    void size_speaker_columns(HWND list) {
        RECT client = {};
        ::GetClientRect(list, &client);
        const int usable_width = max_int(
            0,
            static_cast<int>(client.right - client.left) - ::GetSystemMetrics(SM_CXVSCROLL) - 2);
        if (usable_width == 0) return;

        const int name_width = max_int(140, usable_width * 27 / 100);
        const int state_width = max_int(90, usable_width * 16 / 100);
        const int endpoint_width = max_int(170, usable_width * 36 / 100);
        const int format_width = max_int(85, usable_width - name_width - state_width - endpoint_width);

        ListView_SetColumnWidth(list, 0, name_width);
        ListView_SetColumnWidth(list, 1, state_width);
        ListView_SetColumnWidth(list, 2, endpoint_width);
        ListView_SetColumnWidth(list, 3, format_width);
    }

    void position_pages() {
        HWND tabs = find_dlg_item(wnd_, idTabs);
        if (tabs == nullptr) return;

        RECT client_rect = {};
        ::GetClientRect(wnd_, &client_rect);
        const auto tab_margin = dialog_units_to_pixels(wnd_, 5, 5);
        ::SetWindowPos(
            tabs,
            nullptr,
            tab_margin.cx,
            tab_margin.cy,
            max_int(0, static_cast<int>(client_rect.right) - (tab_margin.cx * 2)),
            max_int(0, static_cast<int>(client_rect.bottom) - (tab_margin.cy * 2)),
            SWP_NOZORDER | SWP_NOACTIVATE);

        RECT tab_rect = {};
        ::GetWindowRect(tabs, &tab_rect);
        ::MapWindowPoints(nullptr, wnd_, reinterpret_cast<POINT*>(&tab_rect), 2);

        RECT page_rect = {0, 0, tab_rect.right - tab_rect.left, tab_rect.bottom - tab_rect.top};
        TabCtrl_AdjustRect(tabs, FALSE, &page_rect);

        const int x = tab_rect.left + page_rect.left;
        const int y = tab_rect.top + page_rect.top;
        const int width = page_rect.right - page_rect.left;
        const int height = page_rect.bottom - page_rect.top;

        for (HWND page_wnd : page_wnds_) {
            if (page_wnd != nullptr && ::IsWindow(page_wnd)) {
                ::SetWindowPos(page_wnd, HWND_TOP, x, y, width, height, SWP_NOACTIVATE);
            }
        }
        layout_status_page();
    }

    void show_selected_page() {
        for (size_t page = 0; page < page_wnds_.size(); ++page) {
            HWND page_wnd = page_wnds_[page];
            if (page_wnd != nullptr) {
                ::ShowWindow(page_wnd, page == static_cast<size_t>(selected_page_) ? SW_SHOW : SW_HIDE);
            }
        }
    }

    void layout_status_page() {
        HWND page = page_wnds_[static_cast<size_t>(Page::Status)];
        if (page == nullptr || !::IsWindow(page)) return;

        RECT rc = {};
        ::GetClientRect(page, &rc);
        const int client_width = static_cast<int>(rc.right - rc.left);
        const int client_height = static_cast<int>(rc.bottom - rc.top);

        const auto margin = dialog_units_to_pixels(page, 11, 11);
        const auto summary_position = dialog_units_to_pixels(page, 109, 11);
        const auto list_position = dialog_units_to_pixels(page, 11, 83);
        const auto pair_label_position = dialog_units_to_pixels(page, 11, 2);
        const auto pin_position = dialog_units_to_pixels(page, 39, 0);
        const auto pair_position = dialog_units_to_pixels(page, 101, 0);
        const auto pin_size = dialog_units_to_pixels(page, 52, 14);
        const auto pair_size = dialog_units_to_pixels(page, 110, 14);
        const auto label_size = dialog_units_to_pixels(page, 24, 13);
        const auto pair_gap = dialog_units_to_pixels(page, 0, 9).cy;
        const auto pair_bottom_margin = dialog_units_to_pixels(page, 0, 20).cy;

        const int width = max_int(0, client_width - (margin.cx * 2));
        const int pair_top = max_int(0, client_height - pair_bottom_margin - pair_size.cy);
        const int list_height = max_int(0, pair_top - pair_gap - list_position.cy);

        if (HWND summary = find_dlg_item(page, idStatusSummary); summary != nullptr) {
            const int summary_width = max_int(0, client_width - summary_position.cx - margin.cx);
            ::SetWindowPos(
                summary,
                nullptr,
                summary_position.cx,
                summary_position.cy,
                summary_width,
                label_size.cy,
                SWP_NOZORDER | SWP_NOACTIVATE);
        }
        if (HWND list = find_dlg_item(page, idSpeakerList); list != nullptr) {
            ::SetWindowPos(
                list,
                nullptr,
                list_position.cx,
                list_position.cy,
                width,
                list_height,
                SWP_NOZORDER | SWP_NOACTIVATE);
            size_speaker_columns(list);
        }
        if (HWND pin_label = find_dlg_item(page, idPairPinLabel); pin_label != nullptr) {
            ::SetWindowPos(
                pin_label,
                nullptr,
                pair_label_position.cx,
                pair_top + pair_label_position.cy,
                label_size.cx,
                label_size.cy,
                SWP_NOZORDER | SWP_NOACTIVATE);
        }
        if (HWND pin = find_dlg_item(page, idPairPin); pin != nullptr) {
            ::SetWindowPos(
                pin,
                nullptr,
                pin_position.cx,
                pair_top,
                pin_size.cx,
                pin_size.cy,
                SWP_NOZORDER | SWP_NOACTIVATE);
        }
        if (HWND pair = find_dlg_item(page, idPairButton); pair != nullptr) {
            ::SetWindowPos(
                pair,
                nullptr,
                pair_position.cx,
                pair_top,
                pair_size.cx,
                pair_size.cy,
                SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }

    INT_PTR on_command(WPARAM wp) {
        const WORD id = LOWORD(wp);
        const WORD code = HIWORD(wp);

        if (id == idGroupList && code == LBN_SELCHANGE) {
            load_selected_group();
            return TRUE;
        }
        if (code != BN_CLICKED) return FALSE;

        if (id == idSupportButton) {
            ::ShellExecuteW(wnd_, L"open", L"https://buymeacoffee.com/szymonrybka", nullptr, nullptr, SW_SHOWNORMAL);
            return TRUE;
        }
        if (id == idGitHubButton) {
            ::ShellExecuteW(wnd_, L"open", L"https://github.com/ArtifexEt/foobar-universal-multiroom", nullptr, nullptr, SW_SHOWNORMAL);
            return TRUE;
        }
        if (id == idRefreshButton) {
            MultiroomComponentState::instance().refresh_outputs();
            update_status_page();
            ::SetTimer(wnd_, kStatusRefreshTimer, 250, nullptr);
            return TRUE;
        }
        if (id == idPairButton) {
            return pair_selected_airplay_output();
        }
        if (id == idGroupNewButton) {
            begin_new_group();
            return TRUE;
        }
        if (id == idGroupSaveButton) {
            return save_group_editor();
        }
        if (id == idGroupDeleteButton) {
            return delete_selected_group();
        }
        if (id == idGroupApplyButton) {
            return apply_selected_group();
        }

        return FALSE;
    }

    INT_PTR save_group_editor() {
        try {
            const auto name = narrow_utf8(trim(text_from_item(wnd_, idGroupName)));
            std::vector<std::string> members;
            HWND list = find_dlg_item(wnd_, idGroupMemberList);
            if (list != nullptr) {
                for (int index = 0; index < static_cast<int>(group_member_output_ids_.size()); ++index) {
                    if (ListView_GetCheckState(list, index) != FALSE) {
                        members.push_back(group_member_output_ids_[static_cast<size_t>(index)]);
                    }
                }
            }
            const auto saved_id = MultiroomComponentState::instance().save_speaker_group(
                editing_group_id_, name, members);
            reload_group_list(saved_id);
            return TRUE;
        } catch (const std::exception& e) {
            const auto message = widen_utf8(e.what());
            ::MessageBoxW(wnd_, message.c_str(), MULTIROOM_PRODUCT_NAME_WIDE, MB_OK | MB_ICONWARNING);
            return TRUE;
        }
    }

    INT_PTR delete_selected_group() {
        const int selected = selected_group_index();
        if (selected < 0) return TRUE;
        const auto& group = speaker_groups_[static_cast<size_t>(selected)];
        const auto prompt = L"Delete the speaker group \"" + widen_utf8(group.name) + L"\"?";
        if (::MessageBoxW(wnd_, prompt.c_str(), MULTIROOM_PRODUCT_NAME_WIDE, MB_YESNO | MB_ICONQUESTION) != IDYES) {
            return TRUE;
        }
        MultiroomComponentState::instance().delete_speaker_group(group.id);
        reload_group_list({});
        return TRUE;
    }

    INT_PTR apply_selected_group() {
        const int selected = selected_group_index();
        if (selected < 0) return TRUE;
        MultiroomComponentState::instance().activate_speaker_group(
            speaker_groups_[static_cast<size_t>(selected)].id);
        update_status_page();
        ::SetTimer(wnd_, kStatusRefreshTimer, 250, nullptr);
        return TRUE;
    }

    INT_PTR pair_selected_airplay_output() {
        HWND list = find_dlg_item(wnd_, idSpeakerList);
        if (list == nullptr) return TRUE;

        const int selected = ListView_GetNextItem(list, -1, LVNI_SELECTED);
        if (selected < 0 || selected >= static_cast<int>(speaker_list_output_ids_.size())) {
            ::MessageBoxW(wnd_, L"Select a speaker from the list.", MULTIROOM_PRODUCT_NAME_WIDE, MB_OK | MB_ICONWARNING);
            return TRUE;
        }

        const auto pin = narrow_pin(trim(text_from_item(wnd_, idPairPin)));
        if (pin.empty()) {
            ::MessageBoxW(wnd_, L"Enter the PIN shown by the speaker or TV.", MULTIROOM_PRODUCT_NAME_WIDE, MB_OK | MB_ICONWARNING);
            if (HWND pin_control = find_dlg_item(wnd_, idPairPin); pin_control != nullptr) {
                ::SetFocus(pin_control);
                ::SendMessageW(pin_control, EM_SETSEL, 0, -1);
            }
            return TRUE;
        }

        MultiroomComponentState::instance().pair_output(
            speaker_list_output_ids_[static_cast<size_t>(selected)],
            pin);
        update_status_page();
        ::SetTimer(wnd_, kStatusRefreshTimer, 250, nullptr);
        return TRUE;
    }

    INT_PTR on_timer(WPARAM wp) {
        if (wp != kStatusRefreshTimer) return FALSE;

        update_status_page();
        refresh_group_members_if_outputs_changed();
        if (!MultiroomComponentState::instance().refresh_in_progress() &&
            !MultiroomComponentState::instance().pairing_in_progress() &&
            !MultiroomComponentState::instance().control_in_progress()) {
            ::KillTimer(wnd_, kStatusRefreshTimer);
        }
        return TRUE;
    }

    INT_PTR on_notify(NMHDR* header) {
        if (header != nullptr && header->idFrom == idSpeakerList &&
            header->code == LVN_ITEMCHANGED && !updating_speaker_list_) {
            const auto* change = reinterpret_cast<const NMLISTVIEW*>(header);
            const UINT changed_state = (change->uOldState ^ change->uNewState) & LVIS_STATEIMAGEMASK;
            if ((change->uChanged & LVIF_STATE) != 0 && changed_state != 0 && change->iItem >= 0) {
                if (change->iItem < static_cast<int>(speaker_list_output_ids_.size())) {
                    const bool visible = ListView_GetCheckState(
                        find_dlg_item(wnd_, idSpeakerList),
                        change->iItem) != FALSE;
                    MultiroomComponentState::instance().set_output_dropdown_visibility(
                        speaker_list_output_ids_[static_cast<size_t>(change->iItem)],
                        visible);
                }
            }
            return TRUE;
        }
        if (header != nullptr && header->idFrom == idTabs && header->code == TCN_SELCHANGE) {
            HWND tabs = find_dlg_item(wnd_, idTabs);
            const int selected = tabs != nullptr ? TabCtrl_GetCurSel(tabs) : -1;
            if (selected >= 0 && selected < static_cast<int>(Page::Count)) {
                selected_page_ = selected;
                show_selected_page();
            }
        }
        return FALSE;
    }

    INT_PTR on_erase(HWND target, HDC dc) {
        RECT rc = {};
        ::GetClientRect(target, &rc);
        FillRect(dc, &rc, background_brush());
        return TRUE;
    }

    INT_PTR on_control_color(HDC dc, HWND control, UINT msg) {
        if (!dark_) return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_WINDOW));

        SetTextColor(dc, kDarkText);
        SetBkColor(dc, kDarkBackground);
        SetBkMode(dc, TRANSPARENT);
        if (msg == WM_CTLCOLOREDIT || is_edit_control(control)) {
            SetBkMode(dc, OPAQUE);
            SetBkColor(dc, kDarkEditBackground);
            return reinterpret_cast<INT_PTR>(edit_brush_);
        }

        return reinterpret_cast<INT_PTR>(background_brush());
    }

    HBRUSH background_brush() const {
        return dark_ && background_brush_ != nullptr ? background_brush_ : GetSysColorBrush(COLOR_WINDOW);
    }

    static bool is_edit_control(HWND control) {
        wchar_t class_name[16] = {};
        GetClassNameW(control, class_name, static_cast<int>(_countof(class_name)));
        return _wcsicmp(class_name, L"Edit") == 0;
    }

    void redraw() {
        ::RedrawWindow(wnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    }

    void update_status_page() {
        HWND summary = find_dlg_item(wnd_, idStatusSummary);
        const auto status = MultiroomComponentState::instance().status_text();
        if (summary != nullptr) {
            ::SetWindowTextW(summary, status.c_str());
        }
        update_speaker_list();
    }

    void update_speaker_list() {
        HWND list = find_dlg_item(wnd_, idSpeakerList);
        if (list == nullptr) return;

        const auto outputs = MultiroomComponentState::instance().outputs();
        updating_speaker_list_ = true;
        ListView_DeleteAllItems(list);
        speaker_list_output_ids_.clear();
        speaker_list_output_ids_.reserve(outputs.size());

        for (int index = 0; index < static_cast<int>(outputs.size()); ++index) {
            const auto& output = outputs[static_cast<size_t>(index)];
            speaker_list_output_ids_.push_back(output.id);
            const std::array<std::wstring, 4> cells = {
                widen_utf8(output.name.empty() ? output.id : output.name),
                speaker_state_text(output),
                endpoint_text(output),
                widen_utf8(output.format),
            };

            for (int column = 0; column < static_cast<int>(cells.size()); ++column) {
                LVITEMW item = {};
                item.mask = LVIF_TEXT;
                item.iItem = index;
                item.iSubItem = column;
                item.pszText = const_cast<wchar_t*>(cells[static_cast<size_t>(column)].c_str());
                if (column == 0) {
                    ::SendMessageW(list, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));
                } else {
                    ::SendMessageW(list, LVM_SETITEMW, 0, reinterpret_cast<LPARAM>(&item));
                }
            }
            ListView_SetCheckState(list, index, output.visible_in_dropdown ? TRUE : FALSE);
        }
        updating_speaker_list_ = false;
    }

    preferences_page_callback::ptr callback_;
    HWND wnd_ = nullptr;
    fb2k::CCoreDarkModeHooks dark_;
    std::array<HWND, static_cast<size_t>(Page::Count)> page_wnds_ = {};
    std::vector<std::string> speaker_list_output_ids_;
    std::vector<multiroom::SpeakerGroup> speaker_groups_;
    std::vector<std::string> group_member_output_ids_;
    std::string editing_group_id_;
    std::string group_member_outputs_signature_;
    int selected_page_ = 0;
    bool updating_speaker_list_ = false;
    HBRUSH background_brush_ = CreateSolidBrush(kDarkBackground);
    HBRUSH edit_brush_ = CreateSolidBrush(kDarkEditBackground);
};

class preferences_page_multiroom : public preferences_page_impl<preferences_instance> {
public:
    const char* get_name() override { return MULTIROOM_PRODUCT_NAME; }
    GUID get_guid() override { return guid_preferences; }
    GUID get_parent_guid() override { return preferences_page::guid_output; }
};

static preferences_page_factory_t<preferences_page_multiroom> g_preferences_factory;

}  // namespace
