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
#pragma once

#include <windows.h>

#include <atomic>
#include <string>
#include <string_view>
#include <thread>

#include "win32.h"

namespace zcr::ipc {

/// The control channel between `zcr --start` and friends and the tray instance.
///
/// A named pipe, \\.\pipe\ZCR.<session id>, message mode, one request line in
/// and one reply line out, UTF-8. A reply starts with "ok " or "err ". The tray
/// serves one client at a time; the next one waits in WaitNamedPipeW.

/// \\.\pipe\ZCR.<session id> for the session this process runs in.
[[nodiscard]] std::wstring PipeName();

/// One request, handed from the pipe thread to the UI thread by pointer in a
/// SendMessageW, so it lives on the pipe thread's stack for the whole call.
struct Request {
    std::wstring line;    // trimmed, e.g. L"set fps 60"
    std::wstring reply;   // filled by the UI thread: L"ok ..." or L"err ..."
};

class Server {
public:
    Server() = default;
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    /// Creates the pipe and the thread. Each request is delivered as
    /// SendMessageW(target, message, 0, reinterpret_cast<LPARAM>(Request*)).
    /// False when the pipe name is already taken or cannot be created; the tray
    /// still works then, only remote control does not.
    bool Start(HWND target, UINT message);

    /// Idempotent. Called on the UI thread, and dispatches messages sent to that
    /// thread while it waits: the pipe thread may be inside a SendMessageW to
    /// this very thread, and a plain join would deadlock against it.
    void Stop();

    /// The request a WM_APP_COMMAND carries, or null when `lparam` is not the
    /// one this server is waiting on. Any process on the desktop can send our
    /// window WM_APP+n with an arbitrary lParam, so the pointer is checked
    /// before it is dereferenced.
    [[nodiscard]] Request* Claim(LPARAM lparam) const;

private:
    void Run();
    void Serve(HANDLE io_event);

    HWND target_ = nullptr;
    UINT message_ = 0;
    std::atomic<Request*> pending_{nullptr};
    Handle pipe_;
    Handle stop_event_;
    std::thread thread_;
};

enum class CallResult {
    Ok,           // `reply` holds the server's line
    NotRunning,   // no pipe: no tray instance in this session
    Failed,       // the pipe exists but the exchange broke; `reply` says how
};

/// Sends one request line and waits for the reply. Waits for a busy pipe (the
/// tray serves one client at a time and a stop can take a few seconds).
[[nodiscard]] CallResult Call(std::wstring_view request, std::wstring& reply);

/// True once the pipe exists, polling until `timeout_ms` has passed. Used right
/// after launching the tray, which needs a moment to create it.
[[nodiscard]] bool WaitForServer(DWORD timeout_ms);

/// True when a server pipe exists right now.
[[nodiscard]] bool ServerExists();

} // namespace zcr::ipc
