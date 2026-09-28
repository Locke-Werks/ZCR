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
#include <windows.h>

#include "audio.h"
#include "autostart.h"
#include "diag_log.h"
#include "ids.h"
#include "ipc.h"
#include "monitors.h"
#include "parent_console.h"
#include "tray.h"
#include "win32.h"

#include <shellapi.h>

#include <cwchar>
#include <optional>
#include <string>
#include <vector>

namespace zcr {
namespace {

// Exit codes. Scripts and the automated tests branch on these.
constexpr int kExitOk = 0;
constexpr int kExitError = 1;
constexpr int kExitNotRunning = 2;

/// How long a freshly launched tray gets to open its control pipe. It has to
/// load settings and enumerate displays first, which is well under a second.
constexpr DWORD kLaunchWaitMs = 5000;

/// Longest --record-for accepted. Only there to catch a typo in a script, such
/// as milliseconds passed where seconds were meant.
constexpr double kMaxRecordSeconds = 24.0 * 60.0 * 60.0;

std::vector<std::wstring> CommandLineArgs()
{
    std::vector<std::wstring> args;
    int count = 0;
    LPWSTR* raw = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!raw) {
        return args;
    }
    for (int i = 1; i < count; ++i) {   // skip argv[0]
        args.emplace_back(raw[i]);
    }
    LocalFree(raw);
    return args;
}

int Fail(std::wstring_view message, int code = kExitError)
{
    console::Write(message, /*error=*/true);
    return code;
}

void ShowUsage()
{
    console::Write(
        L"zcr                           Run the tray instance.\r\n"
        L"zcr --start [options]         Start recording and print the output path.\r\n"
        L"                              Options change the saved settings, the same\r\n"
        L"                              as the tray menu does:\r\n"
        L"                                --fps 30|60|120\r\n"
        L"                                --monitor N\r\n"
        L"                                --desktop-audio on|off\r\n"
        L"                                --mic on|off\r\n"
        L"                                --mic-device default|N\r\n"
        L"zcr --stop                    Stop recording and print the finished path.\r\n"
        L"zcr --toggle                  Print \"started <path>\" or \"stopped <path>\".\r\n"
        L"zcr --status                  Print \"idle\", \"recording <seconds>s <W>x<H>\r\n"
        L"                              <fps>fps HDR|SDR <monitor> <path>\" or\r\n"
        L"                              \"error <message>\".\r\n"
        L"zcr --quit                    Stop any recording and exit the tray instance.\r\n"
        L"zcr --record-for SECONDS [options]\r\n"
        L"                              Record for SECONDS, then print the finished path.\r\n"
        L"zcr --list-monitors           List displays. N for --monitor is the number in\r\n"
        L"                              the first column; * marks the primary.\r\n"
        L"zcr --list-mics               List microphones. N for --mic-device is the\r\n"
        L"                              number in the first column; * marks the default.\r\n"
        L"zcr --register-autostart      Start ZCR at sign-in (HKCU Run key).\r\n"
        L"zcr --unregister-autostart\r\n"
        L"zcr --help\r\n"
        L"\r\n"
        L"--start, --toggle and --record-for launch the tray instance if it is not\r\n"
        L"running. Exit codes: 0 ok, 1 error (message on stderr), 2 not running.\r\n"
        L"\r\n"
        L"Settings: %LOCALAPPDATA%\\ZCR\\zcr.json   Log: %LOCALAPPDATA%\\ZCR\\zcr.log\r\n"
        L"\r\n"
        L"This is a GUI executable, so PowerShell does not wait for it unless its\r\n"
        L"output is captured: zcr --status | Write-Output");
}

// ---------------------------------------------------------------------------
// Options shared by --start and --record-for
// ---------------------------------------------------------------------------

struct Options {
    std::optional<uint32_t> fps;
    std::optional<uint32_t> monitor;   // 1-based, as --list-monitors prints it
    std::optional<bool> desktop_audio;
    std::optional<bool> mic;
    std::optional<std::wstring> mic_device;   // "default", or 1-based as --list-mics prints it
};

bool ParseOnOff(const std::wstring& text, bool& out)
{
    if (EqualsNoCase(text, L"on")) {
        out = true;
        return true;
    }
    if (EqualsNoCase(text, L"off")) {
        out = false;
        return true;
    }
    return false;
}

bool ParseUint(const std::wstring& text, uint32_t& out)
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

bool ParseSeconds(const std::wstring& text, double& out)
{
    if (text.empty()) {
        return false;
    }
    wchar_t* end = nullptr;
    const double value = std::wcstod(text.c_str(), &end);
    if (!end || *end != L'\0' || !(value > 0.0) || value > kMaxRecordSeconds) {
        return false;
    }
    out = value;
    return true;
}

/// Reads the recording options from args[first..]. Anything else is an error,
/// because a mistyped option silently ignored would record with the wrong
/// settings and nobody would find out until playback.
bool ParseOptions(const std::vector<std::wstring>& args, size_t first, Options& out,
                  std::wstring& error)
{
    for (size_t i = first; i < args.size(); ++i) {
        const std::wstring& arg = args[i];
        const bool is_fps = EqualsNoCase(arg, L"--fps");
        const bool is_monitor = EqualsNoCase(arg, L"--monitor");
        const bool is_desktop_audio = EqualsNoCase(arg, L"--desktop-audio");
        const bool is_mic = EqualsNoCase(arg, L"--mic");
        const bool is_mic_device = EqualsNoCase(arg, L"--mic-device");
        if (!is_fps && !is_monitor && !is_desktop_audio && !is_mic && !is_mic_device) {
            error = L"unexpected argument \"" + arg + L"\"; see zcr --help";
            return false;
        }
        if (i + 1 >= args.size()) {
            error = arg + L" needs a value";
            return false;
        }
        if (is_desktop_audio || is_mic) {
            bool on = false;
            if (!ParseOnOff(args[i + 1], on)) {
                error = arg + L" must be on or off, not \"" + args[i + 1] + L"\"";
                return false;
            }
            (is_mic ? out.mic : out.desktop_audio) = on;
            ++i;
            continue;
        }
        if (is_mic_device) {
            uint32_t number = 0;
            if (!EqualsNoCase(args[i + 1], L"default")
                && (!ParseUint(args[i + 1], number) || number == 0)) {
                error = L"--mic-device must be default or a number from zcr --list-mics";
                return false;
            }
            out.mic_device = args[i + 1];
            ++i;
            continue;
        }
        uint32_t value = 0;
        if (!ParseUint(args[i + 1], value)) {
            error = arg + L" needs a whole number, not \"" + args[i + 1] + L"\"";
            return false;
        }
        if (is_fps) {
            if (value != 30 && value != 60 && value != 120) {
                error = L"--fps must be 30, 60 or 120";
                return false;
            }
            out.fps = value;
        } else {
            if (value == 0) {
                error = L"--monitor counts from 1; see zcr --list-monitors";
                return false;
            }
            out.monitor = value;
        }
        ++i;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Talking to the tray instance
// ---------------------------------------------------------------------------

/// One request, one reply. Returns an exit code; `payload` is the reply text
/// after "ok " on success and the reason otherwise.
int Send(std::wstring_view request, std::wstring& payload)
{
    std::wstring reply;
    switch (ipc::Call(request, reply)) {
    case ipc::CallResult::NotRunning:
        payload = L"not running";
        return kExitNotRunning;
    case ipc::CallResult::Failed:
        payload = reply;
        return kExitError;
    case ipc::CallResult::Ok:
        break;
    }

    if (reply == L"ok") {
        payload.clear();
        return kExitOk;
    }
    if (reply.rfind(L"ok ", 0) == 0) {
        payload = reply.substr(3);
        return kExitOk;
    }
    if (reply.rfind(L"err ", 0) == 0) {
        payload = reply.substr(4);
        return kExitError;
    }
    payload = L"unexpected reply from the tray instance: " + reply;
    return kExitError;
}

/// Starts the tray instance detached from this process.
bool LaunchTray(std::wstring& error)
{
    const std::wstring exe = ExePath();
    if (exe.empty()) {
        error = L"could not determine this executable's path";
        return false;
    }
    std::wstring command_line = L"\"" + exe + L"\"";
    const std::wstring directory = ExeDir();

    // Null standard handles, explicitly. Our caller's may be the pipes of a
    // script waiting for EOF, and a tray that held them would keep that script
    // waiting for as long as the tray runs.
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;

    PROCESS_INFORMATION process{};

    // Out of our job if the job allows it: a test harness that kills its job
    // when the command finishes would otherwise take the tray down with it.
    DWORD flags = CREATE_BREAKAWAY_FROM_JOB | CREATE_NEW_PROCESS_GROUP;
    BOOL created = CreateProcessW(exe.c_str(), command_line.data(), nullptr, nullptr, FALSE,
                                  flags, nullptr, directory.empty() ? nullptr : directory.c_str(),
                                  &startup, &process);
    if (!created && GetLastError() == ERROR_ACCESS_DENIED) {
        flags &= ~static_cast<DWORD>(CREATE_BREAKAWAY_FROM_JOB);
        created = CreateProcessW(exe.c_str(), command_line.data(), nullptr, nullptr, FALSE,
                                 flags, nullptr,
                                 directory.empty() ? nullptr : directory.c_str(), &startup,
                                 &process);
    }
    if (!created) {
        error = L"could not start the tray instance (error " + std::to_wstring(GetLastError())
                + L")";
        return false;
    }

    log::Writef(L"cli: launched the tray instance, pid %lu", process.dwProcessId);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

/// For the commands that should just work from a cold start.
bool EnsureRunning(std::wstring& error)
{
    if (ipc::ServerExists()) {
        return true;
    }
    if (!LaunchTray(error)) {
        return false;
    }
    if (!ipc::WaitForServer(kLaunchWaitMs)) {
        error = L"the tray instance did not open its control pipe within "
                + std::to_wstring(kLaunchWaitMs / 1000) + L" seconds; see "
                + log::Path();
        return false;
    }
    return true;
}

/// Sends the option changes ahead of a start.
int ApplyOptions(const Options& options)
{
    std::vector<std::wstring> requests;
    if (options.fps) {
        requests.push_back(L"set fps " + std::to_wstring(*options.fps));
    }
    if (options.monitor) {
        requests.push_back(L"set monitor " + std::to_wstring(*options.monitor));
    }
    if (options.desktop_audio) {
        requests.push_back(std::wstring(L"set desktop_audio ")
                           + (*options.desktop_audio ? L"on" : L"off"));
    }
    if (options.mic) {
        requests.push_back(std::wstring(L"set mic ") + (*options.mic ? L"on" : L"off"));
    }
    if (options.mic_device) {
        requests.push_back(L"set mic_device " + *options.mic_device);
    }

    std::wstring payload;
    for (const std::wstring& request : requests) {
        const int code = Send(request, payload);
        if (code != kExitOk) {
            return Fail(payload, code);
        }
    }
    return kExitOk;
}

/// Prints the payload on success and the reason on failure.
int Report(int code, const std::wstring& payload)
{
    if (code == kExitOk) {
        if (!payload.empty()) {
            console::Write(payload);
        }
        return code;
    }
    return Fail(payload, code);
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

int CommandStart(const std::vector<std::wstring>& args)
{
    Options options;
    std::wstring error;
    if (!ParseOptions(args, 1, options, error)) {
        return Fail(error);
    }
    if (!EnsureRunning(error)) {
        return Fail(error);
    }
    if (const int code = ApplyOptions(options); code != kExitOk) {
        return code;
    }
    std::wstring payload;
    return Report(Send(L"start", payload), payload);
}

int CommandSimple(const std::vector<std::wstring>& args, std::wstring_view request,
                  bool launch)
{
    if (args.size() != 1) {
        return Fail(L"unexpected argument \"" + args[1] + L"\"; see zcr --help");
    }
    std::wstring error;
    if (launch && !EnsureRunning(error)) {
        return Fail(error);
    }
    std::wstring payload;
    return Report(Send(request, payload), payload);
}

int CommandQuit(const std::vector<std::wstring>& args)
{
    if (args.size() != 1) {
        return Fail(L"unexpected argument \"" + args[1] + L"\"; see zcr --help");
    }
    std::wstring payload;
    const int code = Send(L"quit", payload);
    if (code == kExitNotRunning) {
        return Fail(payload, code);
    }

    // Wait for the process to actually go, so a script can start a fresh one
    // straight after without landing on the instance that is shutting down.
    // The tray holds the mutex for its whole life and releases it last.
    if (code == kExitOk) {
        Handle mutex(OpenMutexW(SYNCHRONIZE, FALSE, kMutexName));
        if (mutex) {
            const DWORD waited = WaitForSingleObject(mutex.Get(), 10000);
            if (waited == WAIT_OBJECT_0 || waited == WAIT_ABANDONED) {
                ReleaseMutex(mutex.Get());
            }
        }
    }
    return Report(code, payload);
}

int CommandRecordFor(const std::vector<std::wstring>& args)
{
    double seconds = 0.0;
    if (args.size() < 2 || !ParseSeconds(args[1], seconds)) {
        return Fail(L"--record-for needs a number of seconds greater than 0");
    }

    Options options;
    std::wstring error;
    if (!ParseOptions(args, 2, options, error)) {
        return Fail(error);
    }
    if (!EnsureRunning(error)) {
        return Fail(error);
    }
    if (const int code = ApplyOptions(options); code != kExitOk) {
        return code;
    }

    std::wstring payload;
    if (const int code = Send(L"start", payload); code != kExitOk) {
        return Fail(payload, code);
    }
    log::Writef(L"cli: recording for %.3f s into %s", seconds, payload.c_str());

    Sleep(static_cast<DWORD>(seconds * 1000.0 + 0.5));

    return Report(Send(L"stop", payload), payload);
}

int CommandListMonitors(const std::vector<std::wstring>& args)
{
    if (args.size() != 1) {
        return Fail(L"unexpected argument \"" + args[1] + L"\"; see zcr --help");
    }
    const std::vector<MonitorInfo> monitors = EnumerateMonitors();
    if (monitors.empty()) {
        return Fail(L"no displays found");
    }

    std::wstring out;
    for (size_t i = 0; i < monitors.size(); ++i) {
        const MonitorInfo& m = monitors[i];
        if (!out.empty()) {
            out += L"\r\n";
        }
        out += std::to_wstring(i + 1) + (m.primary ? L"* " : L"  ") + m.device_name + L" "
               + m.friendly_name + L" " + std::to_wstring(Width(m.desktop_rect)) + L"x"
               + std::to_wstring(Height(m.desktop_rect)) + L" " + (m.hdr ? L"HDR" : L"SDR")
               + L" " + (m.is_nvidia ? L"NVIDIA" : L"not-NVIDIA");
    }
    console::Write(out);
    return kExitOk;
}

int CommandListMics(const std::vector<std::wstring>& args)
{
    if (args.size() != 1) {
        return Fail(L"unexpected argument \"" + args[1] + L"\"; see zcr --help");
    }
    const std::vector<AudioDevice> mics = EnumerateMicrophones();
    if (mics.empty()) {
        return Fail(L"no microphones found");
    }

    std::wstring out;
    for (size_t i = 0; i < mics.size(); ++i) {
        if (!out.empty()) {
            out += L"\r\n";
        }
        out += std::to_wstring(i + 1) + (mics[i].is_default ? L"* " : L"  ") + mics[i].name;
    }
    console::Write(out);
    return kExitOk;
}

int CommandAutostart(bool enable)
{
    // Invoked by the installer through a hook declared as = "user", so it must
    // never put a dialog on screen. console::Write is the silent one.
    std::wstring message;
    const bool ok = enable ? autostart::Register(message) : autostart::Unregister(message);
    console::Write(message, !ok);
    return ok ? kExitOk : kExitError;
}

int RunTrayInstance(HINSTANCE instance)
{
    HANDLE raw = CreateMutexW(nullptr, TRUE, kMutexName);
    const DWORD error = GetLastError();
    if (!raw) {
        // ERROR_ACCESS_DENIED: an elevated instance owns it. Still running.
        log::Writef(L"tray: instance mutex unavailable (%lu), another instance runs", error);
        return kExitOk;
    }
    Handle mutex(raw);
    if (error == ERROR_ALREADY_EXISTS) {
        // CreateMutexW hands back a handle without ownership when the object
        // already exists. A second plain launch is not an error.
        log::Write(L"tray: already running, exiting");
        return kExitOk;
    }

    const int code = RunTray(instance);
    ReleaseMutex(mutex.Get());
    return code;
}

int Dispatch(HINSTANCE instance, const std::vector<std::wstring>& args)
{
    if (args.empty()) {
        return RunTrayInstance(instance);
    }

    const std::wstring& verb = args[0];
    if (EqualsNoCase(verb, L"--help") || EqualsNoCase(verb, L"-h")
        || EqualsNoCase(verb, L"/?")) {
        ShowUsage();
        return kExitOk;
    }
    if (EqualsNoCase(verb, L"--start")) {
        return CommandStart(args);
    }
    if (EqualsNoCase(verb, L"--stop")) {
        return CommandSimple(args, L"stop", /*launch=*/false);
    }
    if (EqualsNoCase(verb, L"--toggle")) {
        return CommandSimple(args, L"toggle", /*launch=*/true);
    }
    if (EqualsNoCase(verb, L"--status")) {
        return CommandSimple(args, L"status", /*launch=*/false);
    }
    if (EqualsNoCase(verb, L"--quit")) {
        return CommandQuit(args);
    }
    if (EqualsNoCase(verb, L"--record-for")) {
        return CommandRecordFor(args);
    }
    if (EqualsNoCase(verb, L"--list-monitors")) {
        return CommandListMonitors(args);
    }
    if (EqualsNoCase(verb, L"--list-mics")) {
        return CommandListMics(args);
    }
    if (EqualsNoCase(verb, L"--register-autostart")) {
        return CommandAutostart(true);
    }
    if (EqualsNoCase(verb, L"--unregister-autostart")) {
        return CommandAutostart(false);
    }
    return Fail(L"unknown argument \"" + verb + L"\"; see zcr --help");
}

} // namespace
} // namespace zcr

int APIENTRY wWinMain(_In_ HINSTANCE instance,
                      _In_opt_ HINSTANCE previous,
                      _In_ LPWSTR command_line,
                      _In_ int show)
{
    UNREFERENCED_PARAMETER(previous);
    UNREFERENCED_PARAMETER(command_line);
    UNREFERENCED_PARAMETER(show);

    // First call in the process, before anything can load a DLL on our behalf.
    // nvEncodeAPI64.dll is loaded by name at runtime, and a portable copy run
    // from a Downloads folder must not pick up a planted one sitting next to it
    // under the current directory.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS
                             | LOAD_LIBRARY_SEARCH_APPLICATION_DIR);

    // ShellExecuteW (Open recordings folder) wants an STA.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    zcr::log::WriteBanner();

    const std::vector<std::wstring> args = zcr::CommandLineArgs();
    {
        std::wstring joined;
        for (const std::wstring& arg : args) {
            joined += (joined.empty() ? L"" : L" ") + arg;
        }
        zcr::log::Writef(L"  args           %s",
                         joined.empty() ? L"(none)" : zcr::log::Abbrev(joined).c_str());
    }

    const int exit_code = zcr::Dispatch(instance, args);

    zcr::console::Detach();
    if (SUCCEEDED(com)) {
        CoUninitialize();
    }
    return exit_code;
}
