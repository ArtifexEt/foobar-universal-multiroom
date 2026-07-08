#include "stdafx.h"
#include "component_version.h"
#include "multiroom_component_state.h"
#include "preferences_resource.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cwctype>
#include <cstdint>
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

bool parse_port(const std::wstring& text, std::uint16_t& port) {
    if (text.empty()) return false;

    wchar_t* end = nullptr;
    errno = 0;
    const unsigned long value = std::wcstoul(text.c_str(), &end, 10);
    while (end != nullptr && std::iswspace(*end) != 0) {
        ++end;
    }

    if (errno == ERANGE || end == text.c_str() || (end != nullptr && *end != L'\0') || value == 0 || value > 65535) {
        return false;
    }

    port = static_cast<std::uint16_t>(value);
    return true;
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
        add_tab(tabs, 1, L"About");

        create_page(Page::Status, IDD_MULTIROOM_PAGE_STATUS);
        create_page(Page::About, IDD_MULTIROOM_PAGE_ABOUT);
        populate_status_page();
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
        HWND port = find_dlg_item(wnd_, idManualPort);
        if (port != nullptr && ::GetWindowTextLengthW(port) == 0) {
            ::SetWindowTextW(port, L"7000");
        }
        configure_speaker_list();
    }

    void configure_speaker_list() {
        HWND list = find_dlg_item(wnd_, idSpeakerList);
        if (list == nullptr) return;

        ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

        while (ListView_DeleteColumn(list, 0)) {
        }

        add_speaker_column(list, 0, L"Name", 156);
        add_speaker_column(list, 1, L"State", 74);
        add_speaker_column(list, 2, L"Endpoint", 110);
        add_speaker_column(list, 3, L"Format", 92);
    }

    void add_speaker_column(HWND list, int index, const wchar_t* label, int width) {
        LVCOLUMNW column = {};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        column.pszText = const_cast<wchar_t*>(label);
        column.cx = width;
        column.iSubItem = index;
        ListView_InsertColumn(list, index, &column);
    }

    void position_pages() {
        HWND tabs = find_dlg_item(wnd_, idTabs);
        if (tabs == nullptr) return;

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
    }

    void show_selected_page() {
        for (size_t page = 0; page < page_wnds_.size(); ++page) {
            HWND page_wnd = page_wnds_[page];
            if (page_wnd != nullptr) {
                ::ShowWindow(page_wnd, page == static_cast<size_t>(selected_page_) ? SW_SHOW : SW_HIDE);
            }
        }
    }

    INT_PTR on_command(WPARAM wp) {
        const WORD id = LOWORD(wp);
        const WORD code = HIWORD(wp);
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
        if (id == idManualAddButton) {
            return add_manual_airplay_output();
        }
        if (id == idPairButton) {
            return pair_selected_airplay_output();
        }

        return FALSE;
    }

    INT_PTR add_manual_airplay_output() {
        auto name = trim(text_from_item(wnd_, idManualName));
        auto host = trim(text_from_item(wnd_, idManualHost));
        auto port_text = trim(text_from_item(wnd_, idManualPort));

        if (host.empty()) {
            ::MessageBoxW(wnd_, L"Enter a host or IP address.", L"Universal Multiroom Bridge", MB_OK | MB_ICONWARNING);
            if (HWND host_control = find_dlg_item(wnd_, idManualHost); host_control != nullptr) {
                ::SetFocus(host_control);
            }
            return TRUE;
        }

        std::uint16_t port = 0;
        if (!parse_port(port_text, port)) {
            ::MessageBoxW(wnd_, L"Enter a port from 1 to 65535.", L"Universal Multiroom Bridge", MB_OK | MB_ICONWARNING);
            if (HWND port_control = find_dlg_item(wnd_, idManualPort); port_control != nullptr) {
                ::SetFocus(port_control);
                ::SendMessageW(port_control, EM_SETSEL, 0, -1);
            }
            return TRUE;
        }

        const bool added = MultiroomComponentState::instance().add_manual_airplay_output(name, host, port);
        update_status_page();
        if (added) {
            if (HWND name_control = find_dlg_item(wnd_, idManualName); name_control != nullptr) {
                ::SetWindowTextW(name_control, L"");
            }
            if (HWND host_control = find_dlg_item(wnd_, idManualHost); host_control != nullptr) {
                ::SetWindowTextW(host_control, L"");
                ::SetFocus(host_control);
            }
        }
        return TRUE;
    }

    INT_PTR pair_selected_airplay_output() {
        HWND list = find_dlg_item(wnd_, idSpeakerList);
        if (list == nullptr) return TRUE;

        const int selected = ListView_GetNextItem(list, -1, LVNI_SELECTED);
        const auto outputs = MultiroomComponentState::instance().outputs();
        if (selected < 0 || selected >= static_cast<int>(outputs.size())) {
            ::MessageBoxW(wnd_, L"Select an AirPlay speaker from the list.", L"Universal Multiroom Bridge", MB_OK | MB_ICONWARNING);
            return TRUE;
        }

        const auto pin = narrow_pin(trim(text_from_item(wnd_, idPairPin)));
        if (pin.empty()) {
            ::MessageBoxW(wnd_, L"Enter the AirPlay PIN shown by the speaker or TV.", L"Universal Multiroom Bridge", MB_OK | MB_ICONWARNING);
            if (HWND pin_control = find_dlg_item(wnd_, idPairPin); pin_control != nullptr) {
                ::SetFocus(pin_control);
                ::SendMessageW(pin_control, EM_SETSEL, 0, -1);
            }
            return TRUE;
        }

        MultiroomComponentState::instance().pair_output(outputs[static_cast<size_t>(selected)].id, pin);
        update_status_page();
        ::SetTimer(wnd_, kStatusRefreshTimer, 250, nullptr);
        return TRUE;
    }

    INT_PTR on_timer(WPARAM wp) {
        if (wp != kStatusRefreshTimer) return FALSE;

        update_status_page();
        if (!MultiroomComponentState::instance().refresh_in_progress() &&
            !MultiroomComponentState::instance().pairing_in_progress()) {
            ::KillTimer(wnd_, kStatusRefreshTimer);
        }
        return TRUE;
    }

    INT_PTR on_notify(NMHDR* header) {
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
        ListView_DeleteAllItems(list);

        for (int index = 0; index < static_cast<int>(outputs.size()); ++index) {
            const auto& output = outputs[static_cast<size_t>(index)];
            auto name = widen_utf8(output.name.empty() ? output.id : output.name);
            auto state = speaker_state_text(output);
            auto endpoint = endpoint_text(output);
            auto format = widen_utf8(output.format);

            LVITEMW item = {};
            item.mask = LVIF_TEXT;
            item.iItem = index;
            item.iSubItem = 0;
            item.pszText = name.data();
            ListView_InsertItem(list, &item);
            ListView_SetItemText(list, index, 1, state.data());
            ListView_SetItemText(list, index, 2, endpoint.data());
            ListView_SetItemText(list, index, 3, format.data());
        }
    }

    preferences_page_callback::ptr callback_;
    HWND wnd_ = nullptr;
    fb2k::CCoreDarkModeHooks dark_;
    std::array<HWND, static_cast<size_t>(Page::Count)> page_wnds_ = {};
    int selected_page_ = 0;
    HBRUSH background_brush_ = CreateSolidBrush(kDarkBackground);
    HBRUSH edit_brush_ = CreateSolidBrush(kDarkEditBackground);
};

class preferences_page_multiroom : public preferences_page_impl<preferences_instance> {
public:
    const char* get_name() override { return "Universal Multiroom Bridge"; }
    GUID get_guid() override { return guid_preferences; }
    GUID get_parent_guid() override { return preferences_page::guid_output; }
};

static preferences_page_factory_t<preferences_page_multiroom> g_preferences_factory;

}  // namespace
