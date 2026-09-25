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
#include "ipc.h"

#include "diag_log.h"
#include "ids.h"

#include <sddl.h>

#include <memory>
#include <string>
#include <vector>

namespace zcr::ipc {
namespace {

constexpr DWORD kRequestMaxBytes = 4096;
constexpr DWORD kReplyBufferBytes = 64 * 1024;   // a reply carries a full path

/// How long a connected client gets to send its line, and to take the reply.
/// A client that connects and then sits there must not wedge the only pipe
/// instance forever.
constexpr DWORD kClientIoTimeoutMs = 2000;
constexpr DWORD kReplyWriteTimeoutMs = 5000;

/// How long Call waits for a busy pipe. A stop drains the encoder and
/// finalizes the file, which is seconds on a long recording, and a second
/// client queued behind it should wait that out rather than fail.
constexpr ULONGLONG kBusyWaitMs = 60000;

enum class Io { Done, Failed, Stopped, TimedOut };

/// Waits for an overlapped operation already issued on `pipe`. On stop or
/// timeout the operation is cancelled and waited for, because `ov` and the
/// buffer live on the caller's stack and the kernel must be done with both
/// before the caller returns.
Io Complete(HANDLE pipe, OVERLAPPED& ov, HANDLE stop, DWORD timeout_ms, DWORD& bytes,
            DWORD& error)
{
    const HANDLE waits[2] = {ov.hEvent, stop};
    const DWORD which = WaitForMultipleObjects(stop ? 2 : 1, waits, FALSE, timeout_ms);
    if (which != WAIT_OBJECT_0) {
        CancelIoEx(pipe, &ov);
        GetOverlappedResult(pipe, &ov, &bytes, TRUE);
        return which == WAIT_OBJECT_0 + 1 ? Io::Stopped : Io::TimedOut;
    }
    if (!GetOverlappedResult(pipe, &ov, &bytes, FALSE)) {
        error = GetLastError();
        return Io::Failed;
    }
    return Io::Done;
}

/// Folds the three ways an overlapped call can report back into one: finished
/// synchronously, pending, or failed outright.
Io Finish(BOOL issued, HANDLE pipe, OVERLAPPED& ov, HANDLE stop, DWORD timeout_ms,
          DWORD& bytes, DWORD& error)
{
    if (!issued) {
        error = GetLastError();
        if (error != ERROR_IO_PENDING) {
            return Io::Failed;
        }
    }
    error = 0;
    return Complete(pipe, ov, stop, timeout_ms, bytes, error);
}

/// Owner-only DACL: this user and SYSTEM. The default pipe DACL grants read
/// to Everyone, which is more than a remote control for a screen recorder
/// should offer to other accounts on the machine.
struct PipeSecurity {
    SECURITY_ATTRIBUTES attributes{};
    PSECURITY_DESCRIPTOR descriptor = nullptr;

    PipeSecurity(const PipeSecurity&) = delete;
    PipeSecurity& operator=(const PipeSecurity&) = delete;
    PipeSecurity() = default;
    ~PipeSecurity()
    {
        if (descriptor) {
            LocalFree(descriptor);
        }
    }

    bool Build()
    {
        HANDLE raw_token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token)) {
            return false;
        }
        Handle token(raw_token);

        DWORD size = 0;
        GetTokenInformation(token.Get(), TokenUser, nullptr, 0, &size);
        if (size == 0) {
            return false;
        }
        std::vector<BYTE> buffer(size);
        if (!GetTokenInformation(token.Get(), TokenUser, buffer.data(), size, &size)) {
            return false;
        }
        const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());

        LPWSTR sid_text = nullptr;
        if (!ConvertSidToStringSidW(user->User.Sid, &sid_text)) {
            return false;
        }
        const std::wstring sddl =
            L"D:P(A;;GA;;;SY)(A;;GA;;;" + std::wstring(sid_text) + L")";
        LocalFree(sid_text);

        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
            descriptor = nullptr;
            return false;
        }
        attributes.nLength = sizeof(attributes);
        attributes.lpSecurityDescriptor = descriptor;
        attributes.bInheritHandle = FALSE;
        return true;
    }
};

bool IsSignaled(HANDLE event)
{
    return WaitForSingleObject(event, 0) == WAIT_OBJECT_0;
}

} // namespace

std::wstring PipeName()
{
    DWORD session = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session)) {
        session = 0;
    }
    return std::wstring(kPipePrefix) + std::to_wstring(session);
}

Server::~Server()
{
    Stop();
}

bool Server::Start(HWND target, UINT message)
{
    target_ = target;
    message_ = message;

    stop_event_.Reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!stop_event_) {
        log::Writef(L"ipc: CreateEvent failed (%lu)", GetLastError());
        return false;
    }

    PipeSecurity security;
    const bool secured = security.Build();
    if (!secured) {
        log::Write(L"ipc: could not build the owner-only DACL, using the default");
    }

    const std::wstring name = PipeName();

    // FIRST_PIPE_INSTANCE: if anything already owns this name, fail rather than
    // join it as a second instance and share clients with a stranger.
    pipe_.Reset(CreateNamedPipeW(
        name.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, kReplyBufferBytes, kRequestMaxBytes, 0,
        secured ? &security.attributes : nullptr));
    if (!pipe_) {
        log::Writef(L"ipc: CreateNamedPipe %s failed (%lu)", name.c_str(), GetLastError());
        return false;
    }

    thread_ = std::thread([this] { Run(); });
    log::Writef(L"ipc: listening on %s", name.c_str());
    return true;
}

void Server::Stop()
{
    if (!thread_.joinable()) {
        return;
    }
    SetEvent(stop_event_.Get());

    // The pipe thread may be blocked in SendMessageW to this thread. Keep
    // dispatching sent messages until it exits, or neither side ever returns.
    const HANDLE thread = thread_.native_handle();
    for (;;) {
        const DWORD which = MsgWaitForMultipleObjectsEx(1, &thread, INFINITE,
                                                        QS_SENDMESSAGE, 0);
        if (which != WAIT_OBJECT_0 + 1) {
            break;
        }
        MSG msg{};
        PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
    }
    thread_.join();
    pipe_.Reset();
}

Request* Server::Claim(LPARAM lparam) const
{
    Request* const pending = pending_.load();
    if (!pending || reinterpret_cast<LPARAM>(pending) != lparam) {
        return nullptr;
    }
    return pending;
}

void Server::Run()
{
    Handle io_event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!io_event) {
        log::Writef(L"ipc: CreateEvent for I/O failed (%lu)", GetLastError());
        return;
    }

    const HANDLE pipe = pipe_.Get();
    const HANDLE stop = stop_event_.Get();

    while (!IsSignaled(stop)) {
        OVERLAPPED ov{};
        ov.hEvent = io_event.Get();
        ResetEvent(ov.hEvent);

        bool connected = false;
        if (ConnectNamedPipe(pipe, &ov)) {
            connected = true;
        } else {
            const DWORD error = GetLastError();
            if (error == ERROR_PIPE_CONNECTED) {
                // A client got in between CreateNamedPipe/Disconnect and this
                // call. It is connected; there is nothing to wait for.
                connected = true;
            } else if (error == ERROR_IO_PENDING) {
                DWORD bytes = 0;
                DWORD io_error = 0;
                const Io result = Complete(pipe, ov, stop, INFINITE, bytes, io_error);
                if (result == Io::Stopped) {
                    break;
                }
                connected = (result == Io::Done);
            } else {
                log::Writef(L"ipc: ConnectNamedPipe failed (%lu)", error);
            }
        }

        if (connected) {
            Serve(io_event.Get());
        }
        DisconnectNamedPipe(pipe);

        if (!connected) {
            // Whatever broke, do not spin on it.
            if (WaitForSingleObject(stop, 250) == WAIT_OBJECT_0) {
                break;
            }
        }
    }
}

void Server::Serve(HANDLE io_event)
{
    const HANDLE pipe = pipe_.Get();
    const HANDLE stop = stop_event_.Get();

    std::string bytes(kRequestMaxBytes, '\0');
    DWORD read = 0;
    DWORD error = 0;
    bool too_long = false;
    {
        OVERLAPPED ov{};
        ov.hEvent = io_event;
        ResetEvent(io_event);
        const BOOL issued = ReadFile(pipe, bytes.data(), kRequestMaxBytes, nullptr, &ov);
        const Io result = Finish(issued, pipe, ov, stop, kClientIoTimeoutMs, read, error);
        if (result == Io::Failed && error == ERROR_MORE_DATA) {
            too_long = true;
        } else if (result != Io::Done) {
            return;
        }
    }
    bytes.resize(read);

    Request request;
    request.line = std::wstring(Trim(Widen(bytes)));

    if (too_long) {
        request.reply = L"err request too long";
    } else if (IsSignaled(stop)) {
        request.reply = L"err shutting down";
    } else {
        // SendMessageW, not Post: the request lives on this stack and the
        // reply has to be filled in before this returns. Returns 0 at once if
        // the window has already been destroyed, which leaves reply empty.
        pending_.store(&request);
        SendMessageW(target_, message_, 0, reinterpret_cast<LPARAM>(&request));
        pending_.store(nullptr);
        if (request.reply.empty()) {
            request.reply = L"err the tray did not answer";
        }
    }

    log::Writef(L"ipc: \"%s\" -> \"%s\"", log::Abbrev(request.line, 80).c_str(),
                log::Abbrev(request.reply, 200).c_str());

    const std::string reply = Narrow(request.reply + L"\n");
    {
        OVERLAPPED ov{};
        ov.hEvent = io_event;
        ResetEvent(io_event);
        DWORD written = 0;
        const BOOL issued = WriteFile(pipe, reply.data(), static_cast<DWORD>(reply.size()),
                                      nullptr, &ov);
        if (Finish(issued, pipe, ov, stop, kReplyWriteTimeoutMs, written, error)
            != Io::Done) {
            return;
        }
    }

    // DisconnectNamedPipe throws away anything the client has not read yet, so
    // wait for the client to close its end first. The read fails with
    // ERROR_BROKEN_PIPE when it does; the timeout covers a client that never
    // closes. The stop event is deliberately not watched here: "quit" sets it
    // while its own reply is still in the pipe, and cutting that short would
    // lose the one answer the client is waiting for.
    {
        char sink[64];
        OVERLAPPED ov{};
        ov.hEvent = io_event;
        ResetEvent(io_event);
        DWORD ignored = 0;
        const BOOL issued = ReadFile(pipe, sink, sizeof(sink), nullptr, &ov);
        (void)Finish(issued, pipe, ov, nullptr, kClientIoTimeoutMs, ignored, error);
    }
}

CallResult Call(std::wstring_view request, std::wstring& reply)
{
    reply.clear();
    const std::wstring name = PipeName();
    const ULONGLONG deadline = GetTickCount64() + kBusyWaitMs;

    Handle pipe;
    for (;;) {
        // SECURITY_IDENTIFICATION: whoever answers may learn who is asking but
        // may not act as us, which matters if something ever squats the name.
        pipe.Reset(CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                               OPEN_EXISTING,
                               SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
        if (pipe) {
            break;
        }
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND) {
            return CallResult::NotRunning;
        }
        if (error != ERROR_PIPE_BUSY) {
            reply = L"could not open " + name + L" (error " + std::to_wstring(error) + L")";
            return CallResult::Failed;
        }
        if (GetTickCount64() >= deadline) {
            reply = L"the tray instance stayed busy for too long";
            return CallResult::Failed;
        }
        WaitNamedPipeW(name.c_str(), 1000);
    }

    DWORD mode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(pipe.Get(), &mode, nullptr, nullptr)) {
        reply = L"could not switch the pipe to message mode (error "
                + std::to_wstring(GetLastError()) + L")";
        return CallResult::Failed;
    }

    const std::string line = Narrow(std::wstring(request) + L"\n");
    DWORD written = 0;
    if (!WriteFile(pipe.Get(), line.data(), static_cast<DWORD>(line.size()), &written,
                   nullptr)
        || written != line.size()) {
        reply = L"could not send the request (error " + std::to_wstring(GetLastError())
                + L")";
        return CallResult::Failed;
    }

    std::string bytes;
    std::vector<char> chunk(4096);
    for (;;) {
        DWORD read = 0;
        const BOOL ok = ReadFile(pipe.Get(), chunk.data(), static_cast<DWORD>(chunk.size()),
                                 &read, nullptr);
        const DWORD error = ok ? 0 : GetLastError();
        bytes.append(chunk.data(), read);
        if (ok) {
            break;
        }
        if (error == ERROR_MORE_DATA) {
            continue;
        }
        reply = (error == ERROR_BROKEN_PIPE)
                    ? std::wstring(L"the tray instance closed the connection without answering")
                    : L"could not read the reply (error " + std::to_wstring(error) + L")";
        return CallResult::Failed;
    }

    reply = std::wstring(Trim(Widen(bytes)));
    return CallResult::Ok;
}

bool ServerExists()
{
    const std::wstring name = PipeName();
    if (WaitNamedPipeW(name.c_str(), 1)) {
        return true;
    }
    // Exists but every instance is busy serving someone else.
    return GetLastError() == ERROR_SEM_TIMEOUT;
}

bool WaitForServer(DWORD timeout_ms)
{
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    for (;;) {
        if (ServerExists()) {
            return true;
        }
        if (GetTickCount64() >= deadline) {
            return false;
        }
        Sleep(100);
    }
}

} // namespace zcr::ipc
