#include "stdafx.h"
#include "component_version.h"
#include "preferences_resource.h"

#include <array>

#ifndef WM_DPICHANGED_AFTERPARENT
#define WM_DPICHANGED_AFTERPARENT 0x02E3
#endif

namespace {

static constexpr GUID guid_preferences = {
    0x1d6710e5, 0xa768, 0x4d1a, {0x9d, 0x77, 0xce, 0xa4, 0x77, 0x7b, 0x27, 0x01}};
static constexpr COLORREF kDarkBackground = RGB(32, 32, 32);
static constexpr COLORREF kDarkEditBackground = RGB(24, 24, 24);
static constexpr COLORREF kDarkText = RGB(232, 232, 232);

enum class Page {
    Status = 0,
    About,
    Count,
};

HWND find_dlg_item(HWND wnd, int id) {
    return wnd != nullptr ? ::GetDlgItem(wnd, id) : nullptr;
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

class preferences_instance : public preferences_page_instance {
public:
    preferences_instance(preferences_page_callback::ptr callback)
        : callback_(std::move(callback)) {}

    HWND get_wnd() override { return wnd_; }

    HWND create(HWND parent) override {
        INITCOMMONCONTROLSEX cc = {};
        cc.dwSize = sizeof(cc);
        cc.dwICC = ICC_TAB_CLASSES | ICC_WIN95_CLASSES;
        InitCommonControlsEx(&cc);

        wnd_ = CreateDialogParamW(
            core_api::get_my_instance(),
            MAKEINTRESOURCEW(IDD_MULTIROOM_PREFERENCES),
            parent,
            dialog_proc,
            reinterpret_cast<LPARAM>(this));
        return wnd_;
    }

    t_uint32 get_state() override {
        return preferences_state::resettable | preferences_state::dark_mode_supported;
    }

    void apply() override {}
    void reset() override {}

private:
    static INT_PTR CALLBACK dialog_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<preferences_instance*>(::GetWindowLongPtrW(wnd, GWLP_USERDATA));
        if (msg == WM_INITDIALOG) {
            self = reinterpret_cast<preferences_instance*>(lp);
            ::SetWindowLongPtrW(wnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            return self->on_init(wnd);
        }
        if (self == nullptr) return FALSE;

        switch (msg) {
        case WM_ERASEBKGND:
            return self->on_erase(wnd, reinterpret_cast<HDC>(wp));
        case WM_COMMAND:
            return self->on_command(wp);
        case WM_SIZE:
        case WM_DPICHANGED:
        case WM_DPICHANGED_AFTERPARENT:
            self->position_pages();
            return TRUE;
        case WM_THEMECHANGED:
        case WM_SETTINGCHANGE:
            self->redraw();
            return TRUE;
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
        case WM_CTLCOLORDLG:
            return self->on_control_color(reinterpret_cast<HDC>(wp), reinterpret_cast<HWND>(lp), msg);
        case WM_NOTIFY:
            return self->on_notify(reinterpret_cast<NMHDR*>(lp));
        default:
            return FALSE;
        }
    }

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
            HWND summary = find_dlg_item(wnd_, idStatusSummary);
            if (summary != nullptr) {
                ::SetWindowTextW(summary, L"Discovery engine is ready; native network discovery is the next implementation step.");
            }
            return TRUE;
        }

        return FALSE;
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

