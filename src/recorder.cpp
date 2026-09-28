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
#include "recorder.h"

#include "audio.h"
#include "capture.h"
#include "convert.h"
#include "diag_log.h"
#include "monitors.h"
#include "mp4mux.h"
#include "nvenc.h"
#include "win32.h"

#include <avrt.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <iterator>
#include <mutex>
#include <thread>
#include <vector>

namespace zcr {

using Microsoft::WRL::ComPtr;

namespace {

/// Input ring depth. Each slot is one P010 frame in VRAM (12 MB at 4K). Eight
/// gives NVENC's async queue room to absorb a slow frame without the pacer
/// ever waiting at 120 fps, and costs under 100 MB of a 24 GB card.
constexpr size_t kRingSize = 8;

/// A pacer this far behind has not hiccupped, the machine slept. Catching up
/// would mean encoding seconds of duplicate frames in a burst; resynchronizing
/// instead leaves a gap in wall-clock terms but keeps the file sane.
constexpr double kResyncAfterSeconds = 1.0;

constexpr uint64_t kAacFrameSamples = 1024;

std::wstring HrText(HRESULT hr)
{
    wchar_t buf[32];
    swprintf(buf, 32, L"0x%08lX", static_cast<unsigned long>(hr));
    return buf;
}

std::wstring TimestampedName(const std::wstring& dir, int part)
{
    SYSTEMTIME t{};
    GetLocalTime(&t);
    wchar_t name[96];
    if (part <= 1) {
        swprintf(name, 96, L"ZCR_%04u-%02u-%02u_%02u-%02u-%02u.mp4",
                 t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    } else {
        swprintf(name, 96, L"ZCR_%04u-%02u-%02u_%02u-%02u-%02u_part%d.mp4",
                 t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, part);
    }
    return JoinPath(dir, name);
}

/// A start in the same second as the previous one would collide; the muxer
/// opens with CREATE_NEW, so pick a free name rather than fail.
std::wstring UniquePath(const std::wstring& dir, int part)
{
    std::wstring path = TimestampedName(dir, part);
    if (!FileExists(path)) {
        return path;
    }
    const std::wstring stem = path.substr(0, path.size() - 4);
    for (int i = 2; i < 100; ++i) {
        std::wstring candidate = stem + L"-" + std::to_wstring(i) + L".mp4";
        if (!FileExists(candidate)) {
            return candidate;
        }
    }
    return path;
}

double QpcSeconds()
{
    static const double freq = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart) / freq;
}

bool CreateDeviceOn(const MonitorInfo& monitor, ComPtr<ID3D11Device>& device,
                    ComPtr<ID3D11DeviceContext>& context, std::wstring& error)
{
    ComPtr<IDXGIFactory4> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        error = L"CreateDXGIFactory1 failed: " + HrText(hr);
        return false;
    }

    ComPtr<IDXGIAdapter1> adapter;
    hr = factory->EnumAdapterByLuid(monitor.adapter_luid, IID_PPV_ARGS(&adapter));
    if (FAILED(hr)) {
        error = L"adapter for " + monitor.friendly_name + L" not found: " + HrText(hr);
        return false;
    }

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels,
                           static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &device,
                           nullptr, &context);
    if (FAILED(hr)) {
        error = L"D3D11CreateDevice on " + monitor.adapter_name + L" failed: " + HrText(hr);
        return false;
    }

    // NVENC calls into the device from the capture thread while the writer
    // thread locks bitstreams; without this the driver's view of the immediate
    // context is not serialized.
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(device.As(&mt))) {
        mt->SetMultithreadProtected(TRUE);
    }

    // Keeps capture and conversion from queueing behind a game that saturates
    // the GPU. Positive values can be refused without the base-priority
    // privilege; recording still works, just with less headroom, so it is
    // logged rather than treated as an error.
    ComPtr<IDXGIDevice> dxgi_device;
    if (SUCCEEDED(device.As(&dxgi_device))) {
        const HRESULT prio = dxgi_device->SetGPUThreadPriority(7);
        log::Writef(L"recorder: SetGPUThreadPriority(7) -> %s", HrText(prio).c_str());
    }
    return true;
}

uint32_t EvenDown(uint32_t v) { return v & ~1u; }

} // namespace

// ---------------------------------------------------------------------------
// One output file. Everything whose shape depends on the video format lives
// here, so a display mode change tears it down and builds a fresh one.
// ---------------------------------------------------------------------------
struct Segment {
    VideoFormat format;
    std::wstring path;
    std::vector<ComPtr<ID3D11Texture2D>> ring;
    NvencEncoder encoder;
    Mp4Writer mp4;
    std::thread writer;

    // Frames whose packets the writer has taken back from NVENC. Slot
    // (n % kRingSize) is free to overwrite once completed > n - kRingSize,
    // which holds because there are no B-frames and output order is input
    // order.
    std::atomic<uint64_t> completed{0};
    std::atomic<bool> writer_failed{false};
    std::wstring writer_error;   // written before writer_failed is set
    std::mutex mu;
    std::condition_variable cv;
};

struct Recorder::Impl {
    HWND notify_hwnd = nullptr;
    UINT notify_msg = 0;

    mutable std::mutex status_mu;
    RecorderStatus status;

    RecorderSettings settings;
    MonitorInfo monitor;

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    DesktopCapture capture;
    Converter converter;
    std::unique_ptr<Segment> segment;
    int part = 1;
    std::wstring first_path;

    // Outlive every segment of a recording: a display mode change rolls the
    // file over but the audio streams keep running, and each new segment
    // attaches them afresh at its own origin.
    AudioSource desktop_audio;
    AudioSource mic_audio;
    std::vector<AudioSource*> audio;   // in track order
    // Frames written to the current segment per track, so a pacer resync can
    // pick each track up exactly where its file timeline stands.
    std::array<std::atomic<uint64_t>, 2> audio_frames{};


    std::thread session;
    std::atomic<bool> stop{false};
    std::atomic<bool> running{false};

    // ----- status ----------------------------------------------------------

    void SetStatus(RecorderState state, const std::wstring& error = {})
    {
        std::lock_guard lock(status_mu);
        status.state = state;
        status.error = error;
    }

    void Notify() const
    {
        if (notify_hwnd) {
            PostMessageW(notify_hwnd, notify_msg, 0, 0);
        }
    }

    // ----- segments --------------------------------------------------------

    [[nodiscard]] VideoFormat FormatFromCapture() const
    {
        VideoFormat f;
        const bool swap = capture.Rotation() == DXGI_MODE_ROTATION_ROTATE90
                          || capture.Rotation() == DXGI_MODE_ROTATION_ROTATE270;
        f.width = EvenDown(swap ? capture.Height() : capture.Width());
        f.height = EvenDown(swap ? capture.Width() : capture.Height());
        f.fps = settings.fps;
        f.hdr = capture.Hdr();
        f.hdr_meta = monitor.hdr_meta;
        return f;
    }

    bool OpenSegment(std::wstring& error)
    {
        auto seg = std::make_unique<Segment>();
        seg->format = FormatFromCapture();

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = seg->format.width;
        desc.Height = seg->format.height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_P010;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        seg->ring.resize(kRingSize);
        for (auto& tex : seg->ring) {
            const HRESULT hr = device->CreateTexture2D(&desc, nullptr, &tex);
            if (FAILED(hr)) {
                error = L"CreateTexture2D(P010 " + std::to_wstring(desc.Width) + L"x"
                        + std::to_wstring(desc.Height) + L") failed: " + HrText(hr);
                return false;
            }
        }
        converter.ForgetTargets();

        EncoderSettings es;
        es.format = seg->format;
        es.cq = settings.cq;
        es.max_kbps = settings.max_mbps * 1000;
        if (!seg->encoder.Open(device.Get(), es, error)) {
            return false;
        }

        std::vector<ID3D11Texture2D*> raw;
        for (auto& tex : seg->ring) {
            raw.push_back(tex.Get());
        }
        if (!seg->encoder.RegisterInputs(raw.data(), raw.size(), error)) {
            return false;
        }

        std::vector<uint8_t> parameter_sets;
        if (!seg->encoder.SequenceHeader(parameter_sets, error)) {
            return false;
        }

        std::vector<AudioTrackConfig> tracks;
        for (const AudioSource* source : audio) {
            tracks.push_back(source->TrackConfig());
        }

        EnsureDirectory(settings.output_dir);
        seg->path = UniquePath(settings.output_dir, part);
        if (!seg->mp4.Open(seg->path, seg->format, parameter_sets, tracks, error)) {
            return false;
        }

        log::Writef(L"recorder: segment %d %s  %ux%u@%u %s  %s  %zu audio track(s)", part,
                    seg->path.c_str(), seg->format.width, seg->format.height,
                    seg->format.fps, seg->format.hdr ? L"HDR" : L"SDR",
                    monitor.friendly_name.c_str(), tracks.size());

        {
            std::lock_guard lock(status_mu);
            status.file = seg->path;
            status.frames = 0;
            status.seconds = 0.0;
            status.fps = seg->format.fps;
            status.width = seg->format.width;
            status.height = seg->format.height;
            status.hdr = seg->format.hdr;
            status.monitor = monitor.friendly_name;
        }

        Segment* s = seg.get();
        s->writer = std::thread([this, s] { WriterLoop(*s); });
        segment = std::move(seg);
        return true;
    }

    /// Flushes the encoder, waits for the writer to drain and finalize the
    /// file. Safe to call with no segment.
    bool CloseSegment(std::wstring& error)
    {
        if (!segment) {
            return true;
        }
        segment->encoder.Flush();
        if (segment->writer.joinable()) {
            segment->writer.join();
        }
        bool ok = true;
        if (segment->writer_failed) {
            error = segment->writer_error;
            ok = false;
        }
        std::wstring close_error;
        if (!segment->mp4.Close(close_error)) {
            if (ok) {
                error = close_error;
            }
            ok = false;
        }
        log::Writef(L"recorder: closed %s  %llu frames  %llu bytes", segment->path.c_str(),
                    static_cast<unsigned long long>(segment->mp4.SampleCount()),
                    static_cast<unsigned long long>(segment->mp4.BytesWritten()));
        segment->encoder.Close();
        segment.reset();
        return ok;
    }

    void WriterLoop(Segment& s)
    {
        EncodedPacket packet;
        std::wstring error;
        for (;;) {
            const auto result = s.encoder.Next(packet, 250, error);
            if (result == NvencEncoder::Result::Timeout) {
                continue;
            }
            if (result == NvencEncoder::Result::Drained) {
                return;
            }
            if (result == NvencEncoder::Result::Packet
                && !s.mp4.WriteSample(packet.data.data(), packet.data.size(),
                                      packet.keyframe, error)) {
                // A full disk lands here. The fragments already written stay
                // playable; stop taking frames.
            } else if (result == NvencEncoder::Result::Packet) {
                const uint64_t done = s.completed.fetch_add(1) + 1;
                {
                    std::lock_guard lock(status_mu);
                    status.frames = done;
                    status.seconds = static_cast<double>(done) / s.format.fps;
                }
                s.cv.notify_all();
                continue;
            }

            log::Writef(L"recorder: writer failed: %s", error.c_str());
            {
                std::lock_guard lock(s.mu);
                s.writer_error = error;
            }
            s.writer_failed = true;
            s.cv.notify_all();
            stop = true;
            return;
        }
    }

    // ----- the session thread ---------------------------------------------

    /// Encodes one frame into the next ring slot from whatever the capture
    /// surface holds now. Repeat frames go through the same path: re-running
    /// the conversion is cheaper than tracking which slot held the last image,
    /// and it picks up cursor-only movement for free.
    bool EmitFrame(uint64_t index, std::wstring& error)
    {
        Segment& s = *segment;
        {
            std::unique_lock lock(s.mu);
            const bool free = s.cv.wait_for(lock, std::chrono::seconds(2), [&] {
                return s.writer_failed.load() || index < s.completed.load() + kRingSize;
            });
            if (s.writer_failed) {
                error = s.writer_error;
                return false;
            }
            if (!free) {
                error = L"NVENC stalled: no frame came back for 2 seconds";
                return false;
            }
        }

        const size_t slot = static_cast<size_t>(index % kRingSize);
        ConvertParams params;
        params.hdr = capture.Hdr();
        params.sdr_white_nits = monitor.sdr_white_nits;
        params.rotation = capture.Rotation();
        params.draw_cursor = settings.cursor;
        if (!converter.Convert(capture.SurfaceSrv(), capture.Width(), capture.Height(),
                               capture.Cursor(), s.ring[slot].Get(), params, error)) {
            return false;
        }
        // Get the conversion onto the GPU now rather than whenever the driver
        // decides; NVENC is about to wait on it.
        context->Flush();
        return s.encoder.Submit(slot, index, error);
    }

    // ----- audio -----------------------------------------------------------

    /// Points every audio source at the current segment so that file time t of
    /// each track sits at QPC time `origin + t`, the same mapping video uses.
    /// `resume` continues tracks that already have frames in this segment,
    /// after the pacer moved its origin, instead of starting them at zero.
    void AttachAudio(double origin, bool resume = false)
    {
        Segment* s = segment.get();
        for (size_t track = 0; track < audio.size(); ++track) {
            std::atomic<uint64_t>& count = audio_frames[track];
            if (!resume) {
                count = 0;
            }
            const double sample_rate = audio[track]->TrackConfig().sample_rate;
            const double start =
                origin + static_cast<double>(count.load() * kAacFrameSamples) / sample_rate;
            audio[track]->Attach(
                [s, track, &count](const uint8_t* data, size_t size) {
                    // A failed write here is the disk the video writer is
                    // about to report too, so it is not reported twice.
                    std::wstring ignored;
                    if (s->mp4.WriteAudioFrame(track, data, size, ignored)) {
                        ++count;
                    }
                },
                start);
        }
    }

    /// Blocks until each source has delivered its audio up to `end` into the
    /// segment. Must run before CloseSegment: after it the sinks, which point
    /// at that segment's writer, are never called again.
    void DetachAudio(double end)
    {
        for (AudioSource* source : audio) {
            source->Detach(end);
        }
    }

    void StopAudio()
    {
        for (AudioSource* source : audio) {
            source->Stop();
        }
        audio.clear();
    }

    bool StartAudio(std::wstring& error)
    {
        audio.clear();
        if (settings.desktop_audio) {
            if (!desktop_audio.Start(AudioSourceKind::Desktop, {}, error)) {
                error = L"desktop audio: " + error;
                return false;
            }
            audio.push_back(&desktop_audio);
        }
        if (settings.mic) {
            if (!mic_audio.Start(AudioSourceKind::Microphone, settings.mic_device, error)) {
                error = L"microphone: " + error;
                StopAudio();
                return false;
            }
            audio.push_back(&mic_audio);
        }
        std::lock_guard lock(status_mu);
        status.desktop_audio = settings.desktop_audio;
        status.mic = settings.mic;
        return true;
    }

    bool RollOver(std::wstring& error)
    {
        log::Write(L"recorder: display mode changed, starting a new segment");
        if (!CloseSegment(error)) {
            return false;
        }
        capture.Close();

        const auto monitors = EnumerateMonitors();
        const MonitorInfo* m = ResolveMonitor(monitors, monitor.device_path);
        if (!m || m->device_path != monitor.device_path) {
            error = L"monitor " + monitor.friendly_name + L" disappeared";
            return false;
        }
        monitor = *m;
        if (!capture.Open(device.Get(), monitor, error)) {
            return false;
        }
        ++part;
        if (!OpenSegment(error)) {
            return false;
        }
        Notify();
        return true;
    }

    void Session()
    {
        // MMCSS "Capture" gets the pacer scheduled ahead of ordinary threads
        // without the priority inversion risks of TIME_CRITICAL.
        DWORD task_index = 0;
        const HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Capture", &task_index);
        Handle timer(CreateWaitableTimerExW(nullptr, nullptr,
                                            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                            TIMER_ALL_ACCESS));

        std::wstring error;
        bool failed = false;
        uint64_t index = 0;   // frame index within the current segment
        double period = 1.0 / segment->format.fps;
        double origin = QpcSeconds();
        AttachAudio(origin);

        while (!stop) {
            const double deadline = origin + static_cast<double>(index) * period;
            const double wait = deadline - QpcSeconds();
            if (wait > 0.0) {
                if (timer) {
                    LARGE_INTEGER due{};
                    due.QuadPart = -static_cast<LONGLONG>(wait * 1e7);
                    SetWaitableTimer(timer.Get(), &due, 0, nullptr, nullptr, FALSE);
                    WaitForSingleObject(timer.Get(), INFINITE);
                } else {
                    Sleep(static_cast<DWORD>(wait * 1000.0));
                }
            } else if (-wait > kResyncAfterSeconds) {
                log::Writef(L"recorder: pacer %.2f s behind, resynchronizing", -wait);
                // Video skips the gap rather than filling it, so audio has to
                // skip it too. Left alone it would fill the gap with silence
                // and trail the picture by the length of the sleep from here on.
                DetachAudio(origin + static_cast<double>(index) * period);
                origin = QpcSeconds() - static_cast<double>(index) * period;
                AttachAudio(origin, /*resume=*/true);
            } else if (-wait > period) {
                std::lock_guard lock(status_mu);
                ++status.late_ticks;
            }

            const CaptureStatus cs = capture.Poll();
            if (cs == CaptureStatus::Fatal) {
                error = capture.LastError();
                failed = true;
                break;
            }
            if (cs == CaptureStatus::ModeChanged) {
                // Audio ends where the video of this segment ends, so the
                // next file does not open with a slice of the old one.
                DetachAudio(origin + static_cast<double>(index) * period);
                if (!RollOver(error)) {
                    failed = true;
                    break;
                }
                index = 0;
                period = 1.0 / segment->format.fps;
                origin = QpcSeconds();
                AttachAudio(origin);
                continue;
            }

            if (!EmitFrame(index, error)) {
                failed = true;
                break;
            }
            ++index;
        }

        // Finalize whatever exists, on failure too: the fragments written so
        // far are a valid file and the only copy of the recording. A failed
        // rollover leaves no segment, and the sources were detached before it.
        if (segment) {
            DetachAudio(origin + static_cast<double>(index) * period);
        }
        std::wstring close_error;
        const bool closed = CloseSegment(close_error);
        if (!failed && !closed) {
            error = close_error;
            failed = true;
        }
        capture.Close();
        StopAudio();

        if (mmcss) {
            AvRevertMmThreadCharacteristics(mmcss);
        }

        if (failed) {
            log::Writef(L"recorder: failed: %s", error.c_str());
            SetStatus(RecorderState::Error, error);
        } else {
            SetStatus(RecorderState::Idle);
        }
        running = false;
        Notify();
    }

    void JoinSession()
    {
        if (session.joinable()) {
            session.join();
        }
    }

    void ReleaseDevice()
    {
        converter.Shutdown();
        context.Reset();
        device.Reset();
    }
};

Recorder::Recorder(HWND notify_hwnd, UINT notify_msg) : impl_(std::make_unique<Impl>())
{
    impl_->notify_hwnd = notify_hwnd;
    impl_->notify_msg = notify_msg;
}

Recorder::~Recorder()
{
    std::wstring ignored;
    if (IsRecording()) {
        Stop(ignored);
    }
    impl_->JoinSession();
    impl_->ReleaseDevice();
}

bool Recorder::Start(const RecorderSettings& settings, std::wstring& path_or_error)
{
    Impl& d = *impl_;
    if (d.running) {
        path_or_error = L"already recording";
        return false;
    }
    // A session that ended on its own (an error mid-recording) has finished
    // but was never joined.
    d.JoinSession();
    d.ReleaseDevice();

    auto fail = [&](const std::wstring& why) {
        log::Writef(L"recorder: start failed: %s", why.c_str());
        d.capture.Close();
        if (d.segment) {
            std::wstring ignored;
            d.CloseSegment(ignored);
        }
        d.StopAudio();
        d.ReleaseDevice();
        d.SetStatus(RecorderState::Error, why);
        path_or_error = why;
        return false;
    };

    if (settings.fps != 30 && settings.fps != 60 && settings.fps != 120) {
        return fail(L"fps must be 30, 60 or 120");
    }
    d.settings = settings;
    d.part = 1;

    const auto monitors = EnumerateMonitors();
    const MonitorInfo* m = ResolveMonitor(monitors, settings.monitor_device_path);
    if (!m) {
        return fail(L"no monitors found");
    }
    if (!m->is_nvidia) {
        return fail(m->friendly_name + L" is driven by " + m->adapter_name
                    + L", not an NVIDIA GPU");
    }
    d.monitor = *m;

    std::wstring error;
    if (!CreateDeviceOn(d.monitor, d.device, d.context, error)) {
        return fail(error);
    }
    if (!d.converter.Init(d.device.Get(), error)) {
        return fail(error);
    }
    if (!d.capture.Open(d.device.Get(), d.monitor, error)) {
        return fail(error);
    }
    // Before the segment, because the file's track list is fixed when it
    // opens. A missing mic fails the start rather than recording without it:
    // the person asked for that track and would only find out on playback.
    if (!d.StartAudio(error)) {
        return fail(error);
    }
    if (!d.OpenSegment(error)) {
        return fail(error);
    }

    {
        std::lock_guard lock(d.status_mu);
        d.status.late_ticks = 0;
    }
    d.first_path = d.segment->path;
    d.stop = false;
    d.running = true;
    d.SetStatus(RecorderState::Recording);
    d.session = std::thread([&d] { d.Session(); });

    path_or_error = d.first_path;
    return true;
}

bool Recorder::Stop(std::wstring& path_or_error)
{
    Impl& d = *impl_;
    if (!d.running && !d.session.joinable()) {
        path_or_error = L"not recording";
        return false;
    }
    d.stop = true;
    d.JoinSession();
    d.ReleaseDevice();

    const RecorderStatus s = Status();
    if (s.state == RecorderState::Error) {
        path_or_error = s.error;
        return false;
    }
    path_or_error = s.file;
    return true;
}

bool Recorder::IsRecording() const
{
    return impl_->running;
}

RecorderStatus Recorder::Status() const
{
    std::lock_guard lock(impl_->status_mu);
    return impl_->status;
}

} // namespace zcr
