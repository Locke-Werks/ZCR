// ZCR, a zero-copy HDR screen recorder for Windows on NVIDIA hardware.
// Copyright (C) 2026 Locke Werks
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version.
//
// This program is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
// PARTICULAR PURPOSE. See the GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along with
// this program. If not, see <https://www.gnu.org/licenses/>.
#include "tray.h"

#include "audio.h"
#include "diag_log.h"
#include "ids.h"
#include "ipc.h"
#include "monitors.h"
#include "recorder.h"
#include "resource.h"
#include "settings.h"
#include "win32.h"

#include <shellapi.h>

#include <algorithm>
#include <cwchar>
#include <memory>
#include <string>
#include <vector>

namespace zcr {
namespace {

constexpr UINT kIconId = 1;

enum class IconState { Idle = 0, Recording = 1, Error = 2 };

/// Line breaks become spaces. A pipe reply is one line by contract, and a
/// tooltip with a stray newline in it wraps mid-word.
std::wstring OneLine(std::wstring text)
{
    for (wchar_t& c : text) {
        if (c == L'\r' || c == L'\n') {
            c = L' ';
        }
    }
    return text;
}

/// A menu item treats '&' as the mnemonic marker, so "AT&T" would render as
/// "ATT" with an underlined T.
std::wstring EscapeMenuText(std::wstring_view text)
{
    std::wstring out;
    out.reserve(text.size());
    for (const wchar_t c : text) {
        out.push_back(c);
        if (c == L'&') {
            out.push_back(L'&');
        }
    }
    return out;
}

std::wstring Clock(double seconds)
{
    const auto total = static_cast<unsigned long long>(seconds > 0.0 ? seconds : 0.0);
    wchar_t buffer[32];
    _snwprintf_s(buffer, _TRUNCATE, L"%02llu:%02llu:%02llu", total / 3600,
                 (total / 60) % 60, total % 60);
    return buffer;
}

/// Digits only. wcstoul alone would accept "60abc" and " -1" (as a huge
/// value), and these arrive from a command line.
bool ParseUint(std::wstring_view text, uint32_t& out)
{
    if (text.empty() || text.size() > 9) {
        return false;
    }
    uint32_t value = 0;
    for (const wchar_t c : text) {
        if (c < L'0' || c > L'9') {
            return false;
        }
        value = value * 10 + static_cast<uint32_t>(c - L'0');
    }
    out = value;
    return true;
}

std::vector<std::wstring> SplitWords(std::wstring_view line)
{
    std::vector<std::wstring> words;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == L' ' || line[i] == L'\t')) {
            ++i;
        }
        const size_t start = i;
        while (i < line.size() && line[i] != L' ' && line[i] != L'\t') {
            ++i;
        }
        if (i > start) {
            words.emplace_back(line.substr(start, i - start));
        }
    }
    return words;
}

std::wstring MonitorMenuLabel(size_t index, const MonitorInfo& monitor)
{
    std::wstring label = std::to_wstring(index + 1) + L": "
                         + EscapeMenuText(monitor.friendly_name) + L" ("
                         + std::to_wstring(Width(monitor.desktop_rect)) + L"x"
                         + std::to_wstring(Height(monitor.desktop_rect)) + L", "
                         + (monitor.hdr ? L"HDR" : L"SDR") + L")";
    if (!monitor.is_nvidia) {
        label += L" (not NVIDIA)";
    }
    return label;
}

// ---------------------------------------------------------------------------
// TrayIcon: the notification-area icon and nothing else.
// ---------------------------------------------------------------------------

class TrayIcon {
public:
    TrayIcon() = default;
    ~TrayIcon()
    {
        Remove();
        FreeIcons();
    }
    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    bool Add(HWND host)
    {
        host_ = host;
        if (!icons_[0]) {
            LoadIcons();
        }

        NOTIFYICONDATAW data = BaseData();
        data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
        data.uCallbackMessage = WM_APP_TRAY;
        data.hIcon = CurrentIcon();
        CopyTip(data);

        if (!Shell_NotifyIconW(NIM_ADD, &data)) {
            return false;
        }

        // Version 4 changes the callback packing: the event arrives in
        // LOWORD(lParam), the anchor point in wParam, a left click as
        // NIN_SELECT and a right click or the menu key as WM_CONTEXTMENU.
        data.uVersion = NOTIFYICON_VERSION_4;
        version4_ = Shell_NotifyIconW(NIM_SETVERSION, &data) != FALSE;
        if (!version4_) {
            log::Write(L"tray: NIM_SETVERSION 4 failed, using legacy callbacks");
        }
        added_ = true;
        return true;
    }

    /// After an explorer restart every icon is gone and must be added again.
    /// The icons are reloaded too, because the restart is often a DPI change.
    void Readd()
    {
        if (!host_) {
            return;
        }
        added_ = false;
        FreeIcons();
        if (Add(host_)) {
            log::Write(L"tray: re-added the icon after an explorer restart");
        } else {
            log::Write(L"tray: NIM_ADD failed after an explorer restart");
        }
    }

    void Remove()
    {
        if (!added_) {
            return;
        }
        NOTIFYICONDATAW data = BaseData();
        Shell_NotifyIconW(NIM_DELETE, &data);
        added_ = false;
    }

    void Update(IconState state, std::wstring tip)
    {
        if (state == state_ && tip == tip_) {
            return;
        }
        state_ = state;
        tip_ = std::move(tip);

        if (!added_) {
            // Explorer was not up when we started (a Run-key launch can beat
            // it), or NIM_ADD failed. Try again rather than wait for a
            // TaskbarCreated that may already have been broadcast.
            if (host_ && Add(host_)) {
                log::Write(L"tray: icon added late");
            }
            return;
        }

        NOTIFYICONDATAW data = BaseData();
        data.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        data.hIcon = CurrentIcon();
        CopyTip(data);
        Shell_NotifyIconW(NIM_MODIFY, &data);
    }

    [[nodiscard]] bool Version4() const { return version4_; }

private:
    [[nodiscard]] NOTIFYICONDATAW BaseData() const
    {
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = host_;
        data.uID = kIconId;
        return data;
    }

    /// szTip holds 127 characters and a terminator. Writing past it corrupts
    /// the struct and the call fails in a way that looks like a shell bug.
    void CopyTip(NOTIFYICONDATAW& data) const
    {
        constexpr size_t capacity = ARRAYSIZE(data.szTip) - 1;
        std::wstring text = tip_;
        if (text.size() > capacity) {
            text.resize(capacity - 3);
            text += L"...";
        }
        std::copy_n(text.begin(), text.size(), data.szTip);
        data.szTip[text.size()] = L'\0';
    }

    [[nodiscard]] HICON CurrentIcon() const
    {
        return icons_[static_cast<int>(state_)];
    }

    void LoadIcons()
    {
        // The small-icon metric at the window's DPI, so the shell gets the
        // right frame out of the multi-size .ico instead of downscaling a
        // larger one, which is visibly soft at 150%.
        UINT dpi = host_ ? GetDpiForWindow(host_) : 0;
        if (dpi == 0) {
            dpi = GetDpiForSystem();
        }
        const int cx = GetSystemMetricsForDpi(SM_CXSMICON, dpi);
        const int cy = GetSystemMetricsForDpi(SM_CYSMICON, dpi);

        const HINSTANCE module = GetModuleHandleW(nullptr);
        constexpr int ids[3] = {IDI_TRAY_IDLE, IDI_TRAY_REC, IDI_TRAY_ERR};
        for (int i = 0; i < 3; ++i) {
            icons_[i] = static_cast<HICON>(LoadImageW(
                module, MAKEINTRESOURCEW(ids[i]), IMAGE_ICON, cx, cy, LR_DEFAULTCOLOR));
            owned_[i] = icons_[i] != nullptr;
            if (icons_[i]) {
                continue;
            }

            log::Writef(L"tray: icon resource %d missing, falling back", ids[i]);
            icons_[i] = static_cast<HICON>(LoadImageW(
                module, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, cx, cy, LR_DEFAULTCOLOR));
            owned_[i] = icons_[i] != nullptr;
            if (!icons_[i]) {
                // Shared: owned by the system, never destroyed by us.
                icons_[i] = static_cast<HICON>(LoadImageW(
                    nullptr, IDI_APPLICATION, IMAGE_ICON, cx, cy, LR_SHARED));
            }
        }
    }

    void FreeIcons()
    {
        for (int i = 0; i < 3; ++i) {
            if (icons_[i] && owned_[i]) {
                DestroyIcon(icons_[i]);
            }
            icons_[i] = nullptr;
            owned_[i] = false;
        }
    }

    HWND host_ = nullptr;
    HICON icons_[3] = {};
    bool owned_[3] = {};
    bool added_ = false;
    bool version4_ = false;
    IconState state_ = IconState::Idle;
    std::wstring tip_ = L"ZCR";
};

// ---------------------------------------------------------------------------
// App: the tray instance.
// ---------------------------------------------------------------------------

class App {
public:
    int Run(HINSTANCE instance);

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);

    [[nodiscard]] bool IsRecording() const { return recorder_ && recorder_->IsRecording(); }

    void Refresh();
    void RefreshMonitorLabel();

    bool StartRecording(std::wstring& path_or_error);
    bool StopRecording(std::wstring& path_or_error);
    void Toggle();

    void OnTrayEvent(WPARAM wparam, LPARAM lparam);
    void OnClick();
    void ShowMenu(POINT anchor);
    void OnCommand(ipc::Request& request);
    [[nodiscard]] std::wstring StatusReply() const;

    bool ChooseFps(uint32_t fps, std::wstring& message);
    bool ChooseChroma(uint32_t chroma, std::wstring& message);
    bool ChooseMonitor(const MonitorInfo& monitor, std::wstring& message);
    bool ChooseAudio(bool& setting, bool on, const wchar_t* what, std::wstring& message);
    bool ChooseMicDevice(const AudioDevice* device, std::wstring& message);
    void Persist();
    void OpenRecordingsFolder();
    void Close();

    HWND hwnd_ = nullptr;
    UINT taskbar_created_ = 0;
    Settings settings_;
    std::unique_ptr<Recorder> recorder_;
    TrayIcon icon_;
    ipc::Server ipc_;

    std::wstring monitor_label_;

    /// Failures the recorder never saw, such as an output folder that cannot
    /// be created. Shown the same way as a recorder error; the next start
    /// attempt clears it.
    std::wstring local_error_;
    std::wstring last_logged_error_;

    ULONGLONG last_toggle_ = 0;
    bool timer_running_ = false;
    bool menu_up_ = false;
    bool closing_ = false;
};

LRESULT CALLBACK App::WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }

    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!app) {
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    if (!app->hwnd_) {
        app->hwnd_ = hwnd;   // messages sent from inside CreateWindowExW
    }
    return app->HandleMessage(message, wparam, lparam);
}

LRESULT App::HandleMessage(UINT message, WPARAM wparam, LPARAM lparam)
{
    if (taskbar_created_ != 0 && message == taskbar_created_) {
        icon_.Readd();
        Refresh();
        return 0;
    }

    switch (message) {
    case WM_APP_TRAY:
        OnTrayEvent(wparam, lparam);
        return 0;

    case WM_APP_RECORDER:
        Refresh();
        return 0;

    case WM_APP_COMMAND:
        if (ipc::Request* request = ipc_.Claim(lparam)) {
            OnCommand(*request);
            return 1;
        }
        return 0;

    case WM_TIMER:
        if (wparam == kTimerTooltip) {
            Refresh();
        }
        return 0;

    case WM_DISPLAYCHANGE:
        if (!IsRecording()) {
            RefreshMonitorLabel();
            Refresh();
        }
        return 0;

    case WM_POWERBROADCAST:
        // A recording across a sleep is hours of frozen picture and silence
        // that the pacer and the audio timeline would each have to paper over,
        // and never agree about. Finish the file before the machine goes down.
        if (wparam == PBT_APMSUSPEND && IsRecording()) {
            log::Write(L"tray: system suspending, finalizing the recording");
            std::wstring result;
            StopRecording(result);
        }
        return TRUE;

    case WM_QUERYENDSESSION:
        return TRUE;

    case WM_ENDSESSION:
        // The process is killed soon after this returns, so the file has to
        // be finalized here and not in some later handler.
        if (wparam && IsRecording()) {
            log::Write(L"tray: session ending, finalizing the recording");
            std::wstring result;
            StopRecording(result);
        }
        return 0;

    case WM_CLOSE:
        Close();
        return 0;

    case WM_DESTROY:
        if (timer_running_) {
            KillTimer(hwnd_, kTimerTooltip);
            timer_running_ = false;
        }
        icon_.Remove();
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(hwnd_, message, wparam, lparam);
    }
}

void App::RefreshMonitorLabel()
{
    const std::vector<MonitorInfo> monitors = EnumerateMonitors();
    const MonitorInfo* monitor = ResolveMonitor(monitors, settings_.monitor);
    monitor_label_ = monitor ? monitor->friendly_name : std::wstring(L"no display");
}

void App::Refresh()
{
    if (!recorder_) {
        return;
    }
    const RecorderStatus status = recorder_->Status();
    const bool recording = status.state == RecorderState::Recording;

    if (recording && !timer_running_) {
        SetTimer(hwnd_, kTimerTooltip, 1000, nullptr);
        timer_running_ = true;
    } else if (!recording && timer_running_) {
        KillTimer(hwnd_, kTimerTooltip);
        timer_running_ = false;
    }

    IconState state = IconState::Idle;
    std::wstring tip;
    if (recording) {
        state = IconState::Recording;
        const uint32_t fps = status.fps ? status.fps : settings_.fps;
        tip = L"ZCR: recording " + Clock(status.seconds) + L" (" + std::to_wstring(fps)
              + L" fps, " + (status.hdr ? L"HDR" : L"SDR")
              + (status.chroma444 ? L" 4:4:4" : L" 4:2:0")
              + (status.desktop_audio ? L", desktop audio" : L"")
              + (status.mic ? L", mic" : L"") + L")";
    } else if (!local_error_.empty()) {
        state = IconState::Error;
        tip = L"ZCR: " + local_error_;
    } else if (status.state == RecorderState::Error) {
        state = IconState::Error;
        tip = L"ZCR: " + status.error;
        if (status.error != last_logged_error_) {
            log::Writef(L"recorder: error: %s", status.error.c_str());
            last_logged_error_ = status.error;
        }
    } else {
        tip = L"ZCR: idle (" + std::to_wstring(settings_.fps) + L" fps, "
              + (settings_.chroma == 420 ? L"4:2:0, " : L"4:4:4, ") + monitor_label_
              + L")";
    }

    icon_.Update(state, OneLine(std::move(tip)));
}

bool App::StartRecording(std::wstring& path_or_error)
{
    local_error_.clear();
    last_logged_error_.clear();

    const RecorderSettings recorder_settings = settings_.ToRecorder();

    // The recorder tolerates a missing folder too, but creating it here means
    // a bad output_dir is reported as that, not as a muxer failure.
    if (recorder_settings.output_dir.empty()
        || !EnsureDirectory(recorder_settings.output_dir)) {
        local_error_ = L"cannot create the output folder " + recorder_settings.output_dir;
        log::Writef(L"tray: %s", local_error_.c_str());
        path_or_error = local_error_;
        Refresh();
        return false;
    }

    log::Writef(L"tray: start %u fps, chroma %s, cursor %s, monitor \"%s\", desktop audio %s, "
                L"mic %s \"%s\", into %s",
                recorder_settings.fps, recorder_settings.chroma444 ? L"4:4:4" : L"4:2:0",
                recorder_settings.cursor ? L"on" : L"off",
                recorder_settings.monitor_device_path.c_str(),
                recorder_settings.desktop_audio ? L"on" : L"off",
                recorder_settings.mic ? L"on" : L"off", recorder_settings.mic_device.c_str(),
                recorder_settings.output_dir.c_str());

    const bool ok = recorder_->Start(recorder_settings, path_or_error);
    if (ok) {
        log::Writef(L"tray: recording to %s", path_or_error.c_str());
    } else {
        log::Writef(L"tray: start failed: %s", path_or_error.c_str());
        last_logged_error_ = path_or_error;
    }
    Refresh();
    return ok;
}

bool App::StopRecording(std::wstring& path_or_error)
{
    const bool ok = recorder_->Stop(path_or_error);
    if (ok) {
        log::Writef(L"tray: finished %s", path_or_error.c_str());
    } else {
        // Stop reports "nothing was recording" when the recording already
        // died on its own; the reason it died is the more useful answer.
        const RecorderStatus status = recorder_->Status();
        if (status.state == RecorderState::Error && !status.error.empty()) {
            path_or_error = status.error;
        }
        log::Writef(L"tray: stop failed: %s", path_or_error.c_str());
    }
    Refresh();
    return ok;
}

void App::Toggle()
{
    std::wstring result;
    if (IsRecording()) {
        StopRecording(result);
    } else {
        StartRecording(result);
    }
    // Stamped after the action, not before: Start blocks for a few hundred ms,
    // and the second click of a double click is sitting in the queue by the
    // time it returns.
    last_toggle_ = GetTickCount64();
}

void App::OnClick()
{
    if (GetTickCount64() - last_toggle_ < kToggleDebounceMs) {
        return;
    }
    Toggle();
}

void App::OnTrayEvent(WPARAM wparam, LPARAM lparam)
{
    const UINT event = LOWORD(lparam);
    switch (event) {
    case NIN_SELECT:
    case NIN_KEYSELECT:
        OnClick();
        break;

    case WM_CONTEXTMENU: {
        // Version 4 puts the anchor in wParam, which for the keyboard menu key
        // is the icon rather than wherever the mouse happens to be.
        POINT anchor{static_cast<short>(LOWORD(wparam)), static_cast<short>(HIWORD(wparam))};
        if (!icon_.Version4()) {
            GetCursorPos(&anchor);
        }
        ShowMenu(anchor);
        break;
    }

    // Legacy callbacks, only when NIM_SETVERSION failed. Under version 4 the
    // raw button messages still arrive alongside NIN_SELECT and
    // WM_CONTEXTMENU, and acting on both would toggle twice.
    case WM_LBUTTONUP:
        if (!icon_.Version4()) {
            OnClick();
        }
        break;

    case WM_RBUTTONUP:
        if (!icon_.Version4()) {
            POINT anchor{};
            GetCursorPos(&anchor);
            ShowMenu(anchor);
        }
        break;

    default:
        break;
    }
}

void App::ShowMenu(POINT anchor)
{
    const bool recording = IsRecording();

    // Not enumerated while recording: the submenu is grayed then and cannot be
    // opened, so there is nothing to show.
    const std::vector<MonitorInfo> monitors =
        recording ? std::vector<MonitorInfo>{} : EnumerateMonitors();
    const std::vector<AudioDevice> mics =
        recording ? std::vector<AudioDevice>{} : EnumerateMicrophones();

    HMENU menu = CreatePopupMenu();
    HMENU fps_menu = CreatePopupMenu();
    HMENU monitor_menu = CreatePopupMenu();
    HMENU mic_menu = CreatePopupMenu();
    HMENU chroma_menu = CreatePopupMenu();
    if (!menu || !fps_menu || !monitor_menu || !mic_menu || !chroma_menu) {
        if (menu) DestroyMenu(menu);
        if (fps_menu) DestroyMenu(fps_menu);
        if (chroma_menu) DestroyMenu(chroma_menu);
        if (monitor_menu) DestroyMenu(monitor_menu);
        if (mic_menu) DestroyMenu(mic_menu);
        return;
    }

    AppendMenuW(menu, MF_STRING, kMenuToggle,
                recording ? L"Stop recording" : L"Start recording");
    SetMenuDefaultItem(menu, kMenuToggle, FALSE);

    AppendMenuW(fps_menu, MF_STRING, kMenuFps30, L"30 fps");
    AppendMenuW(fps_menu, MF_STRING, kMenuFps60, L"60 fps");
    AppendMenuW(fps_menu, MF_STRING, kMenuFps120, L"120 fps");
    const UINT fps_checked = settings_.fps == 30    ? kMenuFps30
                             : settings_.fps == 120 ? kMenuFps120
                                                    : kMenuFps60;
    CheckMenuRadioItem(fps_menu, kMenuFps30, kMenuFps120, fps_checked, MF_BYCOMMAND);

    const UINT locked = recording ? MF_GRAYED : 0u;
    AppendMenuW(menu, MF_POPUP | locked, reinterpret_cast<UINT_PTR>(fps_menu), L"Framerate");

    AppendMenuW(chroma_menu, MF_STRING, kMenuChroma444, L"4:4:4 (sharp text)");
    AppendMenuW(chroma_menu, MF_STRING, kMenuChroma420, L"4:2:0 (plays everywhere)");
    CheckMenuRadioItem(chroma_menu, kMenuChroma444, kMenuChroma420,
                       settings_.chroma == 420 ? kMenuChroma420 : kMenuChroma444, MF_BYCOMMAND);
    AppendMenuW(menu, MF_POPUP | locked, reinterpret_cast<UINT_PTR>(chroma_menu), L"Chroma");

    const size_t shown = (std::min)(monitors.size(), static_cast<size_t>(kMenuMonitorMax));
    if (shown == 0) {
        AppendMenuW(monitor_menu, MF_STRING | MF_GRAYED, 0,
                    recording ? L"Locked while recording" : L"No displays found");
    } else {
        for (size_t i = 0; i < shown; ++i) {
            const MonitorInfo& monitor = monitors[i];
            const UINT flags = MF_STRING | (monitor.is_nvidia ? 0u : MF_GRAYED);
            const std::wstring label = MonitorMenuLabel(i, monitor);
            AppendMenuW(monitor_menu, flags, kMenuMonitorFirst + static_cast<UINT>(i),
                        label.c_str());
        }
        const MonitorInfo* selected = ResolveMonitor(monitors, settings_.monitor);
        if (selected) {
            const auto index = static_cast<UINT>(selected - monitors.data());
            if (index < shown) {
                CheckMenuRadioItem(monitor_menu, kMenuMonitorFirst,
                                   kMenuMonitorFirst + static_cast<UINT>(shown) - 1,
                                   kMenuMonitorFirst + index, MF_BYCOMMAND);
            }
        }
    }
    AppendMenuW(menu, MF_POPUP | locked, reinterpret_cast<UINT_PTR>(monitor_menu),
                L"Monitor");

    AppendMenuW(menu, MF_STRING | (settings_.cursor ? MF_CHECKED : MF_UNCHECKED),
                kMenuCursor, L"Show cursor");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    // Locked while recording like framerate and monitor: the file's track list
    // is fixed when it opens.
    AppendMenuW(menu, MF_STRING | locked | (settings_.desktop_audio ? MF_CHECKED : MF_UNCHECKED),
                kMenuDesktopAudio, L"Record desktop audio");
    AppendMenuW(menu, MF_STRING | locked | (settings_.mic ? MF_CHECKED : MF_UNCHECKED),
                kMenuMic, L"Record microphone");

    const size_t mics_shown = (std::min)(mics.size(), static_cast<size_t>(kMenuMicMax));
    const auto default_mic = std::find_if(mics.begin(), mics.end(),
                                          [](const AudioDevice& d) { return d.is_default; });
    const std::wstring default_label =
        default_mic == mics.end() ? std::wstring(L"Default")
                                  : L"Default (" + EscapeMenuText(default_mic->name) + L")";
    AppendMenuW(mic_menu, MF_STRING, kMenuMicDefault, default_label.c_str());
    if (!mics.empty()) {
        AppendMenuW(mic_menu, MF_SEPARATOR, 0, nullptr);
    }
    UINT mic_checked = settings_.mic_device.empty() ? kMenuMicDefault : 0u;
    for (size_t i = 0; i < mics_shown; ++i) {
        const UINT id = kMenuMicFirst + static_cast<UINT>(i);
        const std::wstring label = EscapeMenuText(mics[i].name);
        AppendMenuW(mic_menu, MF_STRING, id, label.c_str());
        if (mics[i].id == settings_.mic_device) {
            mic_checked = id;
        }
    }
    if (mic_checked != 0) {
        // Radio groups need contiguous ids, which the default and the device
        // list are not, so each item is checked on its own.
        MENUITEMINFOW info{};
        info.cbSize = sizeof(info);
        info.fMask = MIIM_FTYPE | MIIM_STATE;
        info.fType = MFT_RADIOCHECK;
        info.fState = MFS_CHECKED;
        SetMenuItemInfoW(mic_menu, mic_checked, FALSE, &info);
    } else if (!recording) {
        // The saved device is unplugged. Say so instead of implying the default
        // is what will be used; a start fails until it is back or changed.
        AppendMenuW(mic_menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(mic_menu, MF_STRING | MF_GRAYED | MF_CHECKED, 0,
                    L"Saved microphone (not connected)");
    }
    AppendMenuW(menu, MF_POPUP | locked, reinterpret_cast<UINT_PTR>(mic_menu), L"Microphone");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(menu, MF_STRING, kMenuOpenFolder, L"Open recordings folder");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, L"Exit");

    // KB135788: without SetForegroundWindow first the menu does not dismiss
    // when the user clicks elsewhere, and without the WM_NULL afterwards it
    // can stay up even then.
    SetForegroundWindow(hwnd_);

    UINT align = TPM_LEFTALIGN;
    if (GetSystemMetrics(SM_MENUDROPALIGNMENT) != 0) {
        align = TPM_RIGHTALIGN;
    }

    menu_up_ = true;
    const UINT command = static_cast<UINT>(TrackPopupMenuEx(
        menu, align | TPM_BOTTOMALIGN | TPM_RIGHTBUTTON | TPM_NONOTIFY | TPM_RETURNCMD,
        anchor.x, anchor.y, hwnd_, nullptr));
    menu_up_ = false;

    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(menu);   // destroys the attached submenus too

    if (command == 0 || closing_) {
        return;
    }

    std::wstring message;
    switch (command) {
    case kMenuToggle:
        Toggle();
        break;
    case kMenuFps30:
        ChooseFps(30, message);
        break;
    case kMenuFps60:
        ChooseFps(60, message);
        break;
    case kMenuFps120:
        ChooseFps(120, message);
        break;
    case kMenuChroma444:
        ChooseChroma(444, message);
        break;
    case kMenuChroma420:
        ChooseChroma(420, message);
        break;
    case kMenuCursor:
        // Applies from the next recording; the current one keeps what it
        // started with.
        settings_.cursor = !settings_.cursor;
        Persist();
        break;
    case kMenuDesktopAudio:
        ChooseAudio(settings_.desktop_audio, !settings_.desktop_audio, L"desktop audio",
                    message);
        break;
    case kMenuMic:
        ChooseAudio(settings_.mic, !settings_.mic, L"microphone", message);
        break;
    case kMenuMicDefault:
        ChooseMicDevice(nullptr, message);
        break;
    case kMenuOpenFolder:
        OpenRecordingsFolder();
        break;
    case kMenuExit:
        Close();
        break;
    default:
        if (command >= kMenuMonitorFirst && command < kMenuMonitorFirst + shown) {
            ChooseMonitor(monitors[command - kMenuMonitorFirst], message);
        } else if (command >= kMenuMicFirst && command < kMenuMicFirst + mics_shown) {
            ChooseMicDevice(&mics[command - kMenuMicFirst], message);
        }
        break;
    }
    if (!message.empty()) {
        log::Writef(L"tray: %s", message.c_str());
    }
}

bool App::ChooseFps(uint32_t fps, std::wstring& message)
{
    if (!Settings::IsSupportedFps(fps)) {
        message = L"fps must be 30, 60 or 120";
        return false;
    }
    if (fps == settings_.fps) {
        message = L"fps " + std::to_wstring(fps);
        return true;
    }
    if (IsRecording()) {
        message = L"cannot change the framerate while recording";
        return false;
    }
    settings_.fps = fps;
    Persist();
    Refresh();
    message = L"fps " + std::to_wstring(fps);
    return true;
}

bool App::ChooseChroma(uint32_t chroma, std::wstring& message)
{
    if (!Settings::IsSupportedChroma(chroma)) {
        message = L"chroma must be 444 or 420";
        return false;
    }
    if (chroma == settings_.chroma) {
        message = L"chroma " + std::to_wstring(chroma);
        return true;
    }
    if (IsRecording()) {
        message = L"cannot change chroma while recording";
        return false;
    }
    settings_.chroma = chroma;
    Persist();
    Refresh();
    message = L"chroma " + std::to_wstring(chroma);
    return true;
}

bool App::ChooseMonitor(const MonitorInfo& monitor, std::wstring& message)
{
    if (!monitor.is_nvidia) {
        message = monitor.friendly_name + L" is on " + monitor.adapter_name
                  + L", not an NVIDIA GPU";
        return false;
    }
    if (monitor.device_path != settings_.monitor) {
        if (IsRecording()) {
            message = L"cannot change the monitor while recording";
            return false;
        }
        settings_.monitor = monitor.device_path;
        Persist();
    }
    monitor_label_ = monitor.friendly_name;
    Refresh();
    message = L"monitor " + monitor.friendly_name;
    return true;
}

bool App::ChooseAudio(bool& setting, bool on, const wchar_t* what, std::wstring& message)
{
    if (setting != on) {
        if (IsRecording()) {
            message = std::wstring(L"cannot change ") + what + L" while recording";
            return false;
        }
        setting = on;
        Persist();
    }
    message = std::wstring(what) + (on ? L" on" : L" off");
    return true;
}

/// nullptr means the default microphone, which is stored as an empty id so it
/// follows whatever Windows calls the default later rather than pinning today's.
bool App::ChooseMicDevice(const AudioDevice* device, std::wstring& message)
{
    const std::wstring id = device ? device->id : std::wstring{};
    if (id != settings_.mic_device) {
        if (IsRecording()) {
            message = L"cannot change the microphone while recording";
            return false;
        }
        settings_.mic_device = id;
        Persist();
    }
    message = L"microphone " + (device ? device->name : std::wstring(L"default"));
    return true;
}

void App::Persist()
{
    std::wstring error;
    if (!settings_.Save(error)) {
        log::Writef(L"settings: %s", error.c_str());
    }
}

void App::OpenRecordingsFolder()
{
    const std::wstring dir = settings_.ResolvedOutputDir();
    if (!EnsureDirectory(dir)) {
        log::Writef(L"tray: cannot create %s", dir.c_str());
        return;
    }
    const auto result = reinterpret_cast<INT_PTR>(
        ShellExecuteW(hwnd_, L"explore", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (result <= 32) {
        log::Writef(L"tray: could not open %s (ShellExecute %lld)", dir.c_str(),
                    static_cast<long long>(result));
    }
}

std::wstring App::StatusReply() const
{
    const RecorderStatus status = recorder_->Status();
    if (status.state == RecorderState::Recording) {
        wchar_t head[128];
        _snwprintf_s(head, _TRUNCATE, L"ok recording %llus %ux%u %ufps %s ",
                     static_cast<unsigned long long>(status.seconds > 0.0 ? status.seconds
                                                                          : 0.0),
                     status.width, status.height, status.fps, status.hdr ? L"HDR" : L"SDR");
        return head + status.monitor + L" " + status.file;
    }
    if (!local_error_.empty()) {
        return L"ok error " + local_error_;
    }
    if (status.state == RecorderState::Error) {
        return L"ok error " + status.error;
    }
    return L"ok idle";
}

void App::OnCommand(ipc::Request& request)
{
    if (closing_) {
        request.reply = L"err shutting down";
        return;
    }

    const std::vector<std::wstring> words = SplitWords(request.line);
    const std::wstring verb = words.empty() ? std::wstring{} : ToLower(words[0]);
    std::wstring reply;

    if (verb == L"start" && words.size() == 1) {
        if (IsRecording()) {
            reply = L"err already recording " + recorder_->Status().file;
        } else {
            std::wstring result;
            reply = (StartRecording(result) ? L"ok " : L"err ") + result;
        }
    } else if (verb == L"stop" && words.size() == 1) {
        if (!IsRecording()) {
            const RecorderStatus status = recorder_->Status();
            reply = (status.state == RecorderState::Error && !status.error.empty())
                        ? L"err " + status.error
                        : std::wstring(L"err not recording");
        } else {
            std::wstring result;
            reply = (StopRecording(result) ? L"ok " : L"err ") + result;
        }
    } else if (verb == L"toggle" && words.size() == 1) {
        std::wstring result;
        if (IsRecording()) {
            reply = StopRecording(result) ? L"ok stopped " + result : L"err " + result;
        } else {
            reply = StartRecording(result) ? L"ok started " + result : L"err " + result;
        }
    } else if (verb == L"status" && words.size() == 1) {
        reply = StatusReply();
    } else if (verb == L"quit" && words.size() == 1) {
        reply = L"ok quit";
        if (IsRecording()) {
            std::wstring result;
            reply = StopRecording(result) ? L"ok stopped " + result : L"err " + result;
        }
        // Posted, so this reply goes out before the window starts closing.
        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
    } else if (verb == L"set" && words.size() == 3 && ToLower(words[1]) == L"fps") {
        uint32_t fps = 0;
        std::wstring message;
        if (!ParseUint(words[2], fps)) {
            reply = L"err fps must be 30, 60 or 120";
        } else {
            reply = (ChooseFps(fps, message) ? L"ok " : L"err ") + message;
        }
    } else if (verb == L"set" && words.size() == 3 && ToLower(words[1]) == L"chroma") {
        uint32_t chroma = 0;
        std::wstring message;
        if (!ParseUint(words[2], chroma)) {
            reply = L"err chroma must be 444 or 420";
        } else {
            reply = (ChooseChroma(chroma, message) ? L"ok " : L"err ") + message;
        }
    } else if (verb == L"set" && words.size() == 3 && ToLower(words[1]) == L"monitor") {
        uint32_t number = 0;
        const std::vector<MonitorInfo> monitors = EnumerateMonitors();
        std::wstring message;
        if (!ParseUint(words[2], number) || number == 0 || number > monitors.size()) {
            reply = L"err monitor must be 1 to " + std::to_wstring(monitors.size())
                    + L"; see zcr --list-monitors";
        } else {
            reply = (ChooseMonitor(monitors[number - 1], message) ? L"ok " : L"err ")
                    + message;
        }
    } else if (verb == L"set" && words.size() == 3
               && (ToLower(words[1]) == L"desktop_audio" || ToLower(words[1]) == L"mic")) {
        const bool desktop = ToLower(words[1]) == L"desktop_audio";
        const std::wstring value = ToLower(words[2]);
        std::wstring message;
        if (value != L"on" && value != L"off") {
            reply = L"err " + words[1] + L" must be on or off";
        } else if (desktop) {
            reply = (ChooseAudio(settings_.desktop_audio, value == L"on", L"desktop audio",
                                 message) ? L"ok " : L"err ") + message;
        } else {
            reply = (ChooseAudio(settings_.mic, value == L"on", L"microphone", message)
                         ? L"ok " : L"err ") + message;
        }
    } else if (verb == L"set" && words.size() == 3 && ToLower(words[1]) == L"mic_device") {
        std::wstring message;
        if (ToLower(words[2]) == L"default") {
            reply = (ChooseMicDevice(nullptr, message) ? L"ok " : L"err ") + message;
        } else {
            uint32_t number = 0;
            const std::vector<AudioDevice> mics = EnumerateMicrophones();
            if (mics.empty()) {
                reply = L"err no microphones found";
            } else if (!ParseUint(words[2], number) || number == 0 || number > mics.size()) {
                reply = L"err mic device must be default or 1 to "
                        + std::to_wstring(mics.size()) + L"; see zcr --list-mics";
            } else {
                reply = (ChooseMicDevice(&mics[number - 1], message) ? L"ok " : L"err ")
                        + message;
            }
        }
    } else {
        reply = L"err unknown request \"" + log::Abbrev(request.line, 60) + L"\"";
    }

    request.reply = OneLine(std::move(reply));
}

void App::Close()
{
    if (closing_) {
        return;
    }

    // DestroyWindow on the owner of a menu that is still tracking would pull
    // the window out from under TrackPopupMenuEx. Dismiss it and come back.
    if (menu_up_) {
        EndMenu();
        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        return;
    }

    if (IsRecording()) {
        std::wstring result;
        StopRecording(result);
    }

    closing_ = true;
    ipc_.Stop();
    DestroyWindow(hwnd_);
}

int App::Run(HINSTANCE instance)
{
    // The capture and writer threads set their own MMCSS characteristics; this
    // keeps the UI thread that drives Start and Stop from being starved by a
    // game at normal priority.
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);

    std::wstring detail;
    settings_ = Settings::Load(detail);
    if (!detail.empty()) {
        log::Writef(L"settings: %s", detail.c_str());
    }
    log::Writef(L"settings: %u fps, chroma %u, cursor %s, cq %u, max %u Mbps, desktop audio %s, "
                L"mic %s, output %s",
                settings_.fps, settings_.chroma, settings_.cursor ? L"on" : L"off", settings_.cq,
                settings_.max_mbps, settings_.desktop_audio ? L"on" : L"off",
                settings_.mic ? L"on" : L"off", settings_.ResolvedOutputDir().c_str());

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = &App::WndProc;
    window_class.hInstance = instance;
    window_class.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&window_class)) {
        log::Writef(L"tray: RegisterClassEx failed (%lu)", GetLastError());
        return 1;
    }

    // A real top-level window, never shown, rather than HWND_MESSAGE: a
    // message-only window does not receive the TaskbarCreated broadcast, and
    // SetForegroundWindow for the menu needs a top-level window.
    if (!CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass, kWindowTitle, WS_POPUP, 0, 0, 0, 0,
                         nullptr, nullptr, instance, this)
        || !hwnd_) {
        log::Writef(L"tray: CreateWindowEx failed (%lu)", GetLastError());
        return 1;
    }

    taskbar_created_ = RegisterWindowMessageW(L"TaskbarCreated");
    // Lets the broadcast through UIPI when this instance runs elevated.
    ChangeWindowMessageFilterEx(hwnd_, taskbar_created_, MSGFLT_ALLOW, nullptr);

    recorder_ = std::make_unique<Recorder>(hwnd_, WM_APP_RECORDER);

    RefreshMonitorLabel();
    if (!icon_.Add(hwnd_)) {
        log::Write(L"tray: NIM_ADD failed; will retry when explorer announces itself");
    }
    Refresh();

    if (!ipc_.Start(hwnd_, WM_APP_COMMAND)) {
        log::Write(L"tray: no control pipe; command-line control will not reach this instance");
    }

    MSG msg{};
    int exit_code = 0;
    for (;;) {
        const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
        if (got == 0) {
            exit_code = static_cast<int>(msg.wParam);
            break;
        }
        if (got == -1) {
            log::Writef(L"tray: GetMessage failed (%lu)", GetLastError());
            exit_code = 1;
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    ipc_.Stop();
    recorder_.reset();   // finalizes anything still open
    log::Write(L"tray: exiting");
    return exit_code;
}

} // namespace

int RunTray(HINSTANCE instance)
{
    App app;
    return app.Run(instance);
}

} // namespace zcr
