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

// Standalone harness for src/nvenc.cpp. Not part of the CMake build.
//
// Build from a VS x64 developer prompt at the repo root:
//   cl /nologo /std:c++20 /W4 /WX /permissive- /EHsc /utf-8 /MT /O2
//      /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS
//      /Isrc /Ithird_party\nvenc tests\nvenc_test.cpp src\nvenc.cpp
//      /Fe:tests\out\nvenc_test.exe /link d3d11.lib dxgi.lib
//
// Run from the repo root:
//   nvenc_test [--sdr] [--420] [--fps N] [--frames N] [--size WxH] [--cq N] [--out path]
// Defaults: HDR, 4:4:4, 3840x2160, 120 fps, 600 frames, tests\out\nvenc_test.hevc
// (tests\out\nvenc_test_sdr.hevc with --sdr).

#include "nvenc.h"

#include <d3d10.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <thread>
#include <vector>

namespace {

template <typename T>
class Com {
public:
    Com() = default;
    ~Com() { Reset(); }
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;

    void Reset()
    {
        if (ptr_ != nullptr) {
            ptr_->Release();
            ptr_ = nullptr;
        }
    }
    [[nodiscard]] T* Get() const { return ptr_; }
    [[nodiscard]] T* operator->() const { return ptr_; }
    [[nodiscard]] T** Receive()
    {
        Reset();
        return &ptr_;
    }

private:
    T* ptr_ = nullptr;
};

struct Options {
    bool hdr = true;
    bool chroma444 = true;
    uint32_t width = 3840;
    uint32_t height = 2160;
    uint32_t fps = 120;
    uint32_t frames = 600;
    uint32_t cq = 24;
    std::wstring out;
};

constexpr size_t kSlots = 6;

[[nodiscard]] bool ParseArgs(int argc, wchar_t** argv, Options& o)
{
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        const bool has_value = i + 1 < argc;
        if (arg == L"--sdr") {
            o.hdr = false;
        } else if (arg == L"--420") {
            o.chroma444 = false;
        } else if (arg == L"--fps" && has_value) {
            o.fps = static_cast<uint32_t>(std::wcstoul(argv[++i], nullptr, 10));
        } else if (arg == L"--frames" && has_value) {
            o.frames = static_cast<uint32_t>(std::wcstoul(argv[++i], nullptr, 10));
        } else if (arg == L"--cq" && has_value) {
            o.cq = static_cast<uint32_t>(std::wcstoul(argv[++i], nullptr, 10));
        } else if (arg == L"--size" && has_value) {
            wchar_t* end = nullptr;
            o.width = static_cast<uint32_t>(std::wcstoul(argv[++i], &end, 10));
            o.height = (end != nullptr && *end == L'x')
                ? static_cast<uint32_t>(std::wcstoul(end + 1, nullptr, 10))
                : 0;
        } else if (arg == L"--out" && has_value) {
            o.out = argv[++i];
        } else {
            std::fwprintf(stderr, L"unknown or incomplete argument: %ls\n", arg.c_str());
            return false;
        }
    }
    if (o.out.empty()) {
        o.out = o.hdr ? L"tests\\out\\nvenc_test.hevc" : L"tests\\out\\nvenc_test_sdr.hevc";
    }
    return o.width != 0 && o.height != 0 && o.fps != 0 && o.frames != 0;
}

[[nodiscard]] bool CreateNvidiaDevice(Com<ID3D11Device>& device, Com<ID3D11DeviceContext>& context)
{
    Com<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                  reinterpret_cast<void**>(factory.Receive())))) {
        std::fwprintf(stderr, L"CreateDXGIFactory1 failed\n");
        return false;
    }

    Com<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, adapter.Receive()) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (desc.VendorId == 0x10DE) {
            std::wprintf(L"adapter: %ls\n", desc.Description);
            break;
        }
        adapter.Reset();
    }
    if (adapter.Get() == nullptr) {
        std::fwprintf(stderr, L"no NVIDIA adapter found\n");
        return false;
    }

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    const HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                         D3D11_CREATE_DEVICE_VIDEO_SUPPORT
                                             | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                         levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                                         device.Receive(), nullptr, context.Receive());
    if (FAILED(hr)) {
        std::fwprintf(stderr, L"D3D11CreateDevice failed: 0x%08lX\n", static_cast<unsigned long>(hr));
        return false;
    }

    Com<ID3D10Multithread> multithread;
    if (FAILED(device->QueryInterface(__uuidof(ID3D10Multithread),
                                      reinterpret_cast<void**>(multithread.Receive())))) {
        std::fwprintf(stderr, L"ID3D10Multithread unavailable\n");
        return false;
    }
    multithread->SetMultithreadProtected(TRUE);
    return true;
}

// Cheap integer hash so every pixel gets stable grain without a PRNG object.
[[nodiscard]] uint32_t Hash(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

// P010: 10-bit samples in the top bits of 16-bit words. Luma plane, then one
// interleaved CbCr plane at half height, both at the same row pitch.
[[nodiscard]] std::vector<uint16_t> BuildFrame(uint32_t w, uint32_t h, uint32_t phase)
{
    std::vector<uint16_t> buffer(static_cast<size_t>(w) * h * 3 / 2);
    uint16_t* luma = buffer.data();
    const uint32_t bar = (phase * 97) % w;
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            // Limited range 64..940: a diagonal ramp, a moving bright bar and
            // low-amplitude grain so the encoder has real detail to spend bits on.
            uint32_t v = 64 + ((x + y + phase * 24) % 876);
            if (x >= bar && x < bar + 64) {
                v = 900;
            }
            const int grain = static_cast<int>(Hash(x * 7919u + y * 104729u + phase) & 15) - 8;
            const int value = std::clamp(static_cast<int>(v) + grain, 64, 940);
            luma[static_cast<size_t>(y) * w + x] = static_cast<uint16_t>(value << 6);
        }
    }
    uint16_t* chroma = luma + static_cast<size_t>(w) * h;
    for (uint32_t y = 0; y < h / 2; ++y) {
        for (uint32_t x = 0; x < w / 2; ++x) {
            const uint32_t cb = 64 + ((x * 2 + phase * 30) % 896);
            const uint32_t cr = 64 + ((y * 2 + phase * 30) % 896);
            uint16_t* pair = chroma + static_cast<size_t>(y) * w + x * 2;
            pair[0] = static_cast<uint16_t>(cb << 6);
            pair[1] = static_cast<uint16_t>(cr << 6);
        }
    }
    return buffer;
}

// R10G10B10A2, red in the low bits: the same ramp, bar and grain as BuildFrame,
// with per-channel detail at full resolution so 4:4:4 has chroma to spend on.
[[nodiscard]] std::vector<uint32_t> BuildFrameRgb(uint32_t w, uint32_t h, uint32_t phase)
{
    std::vector<uint32_t> buffer(static_cast<size_t>(w) * h);
    const uint32_t bar = (phase * 97) % w;
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            uint32_t v = (x + y + phase * 24) % 1024;
            if (x >= bar && x < bar + 64) {
                v = 1000;
            }
            const uint32_t noise = Hash(x * 7919u + y * 104729u + phase);
            const auto channel = [&](uint32_t base, int shift) {
                const int grain = static_cast<int>((noise >> shift) & 15) - 8;
                return static_cast<uint32_t>(std::clamp(static_cast<int>(base) + grain, 0, 1023));
            };
            const uint32_t r = channel(v, 0);
            const uint32_t g = channel((x * 2 + phase * 30) % 1024, 4);
            const uint32_t b = channel((y * 2 + phase * 30) % 1024, 8);
            buffer[static_cast<size_t>(y) * w + x] = r | (g << 10) | (b << 20) | (3u << 30);
        }
    }
    return buffer;
}

[[nodiscard]] const wchar_t* NalName(uint8_t type)
{
    switch (type) {
    case 32: return L"VPS";
    case 33: return L"SPS";
    case 34: return L"PPS";
    case 39: return L"SEI";
    case 19: return L"IDR_W_RADL";
    case 20: return L"IDR_N_LP";
    case 1: return L"TRAIL_R";
    default: return L"?";
    }
}

void PrintNals(const std::vector<uint8_t>& au, const wchar_t* label)
{
    std::wprintf(L"%ls:", label);
    for (size_t i = 0; i + 4 < au.size(); ++i) {
        if (au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1) {
            const uint8_t type = static_cast<uint8_t>((au[i + 3] >> 1) & 0x3F);
            std::wprintf(L" %ls(%u)", NalName(type), type);
            i += 3;
        }
    }
    std::wprintf(L"\n");
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    Options options;
    if (!ParseArgs(argc, argv, options)) {
        return 2;
    }

    Com<ID3D11Device> device;
    Com<ID3D11DeviceContext> context;
    if (!CreateNvidiaDevice(device, context)) {
        return 1;
    }

    // The textures the recorder hands NVENC come out of the converter as
    // render targets, so the test uses the same bind flags.
    std::vector<Com<ID3D11Texture2D>> ring(kSlots);
    std::vector<ID3D11Texture2D*> raw(kSlots);
    zcr::EncoderSettings settings;
    settings.format.width = options.width;
    settings.format.height = options.height;
    settings.format.fps = options.fps;
    settings.format.hdr = options.hdr;
    settings.format.chroma444 = options.chroma444;
    settings.cq = options.cq;

    for (size_t i = 0; i < kSlots; ++i) {
        const auto phase = static_cast<uint32_t>(i);
        const std::vector<uint16_t> yuv =
            options.chroma444 ? std::vector<uint16_t>{} : BuildFrame(options.width, options.height, phase);
        const std::vector<uint32_t> rgb =
            options.chroma444 ? BuildFrameRgb(options.width, options.height, phase) : std::vector<uint32_t>{};

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = options.width;
        desc.Height = options.height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = settings.format.SurfaceFormat();
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;

        D3D11_SUBRESOURCE_DATA init{};
        init.pSysMem = options.chroma444 ? static_cast<const void*>(rgb.data()) : yuv.data();
        init.SysMemPitch = options.width * (options.chroma444 ? 4 : 2);
        const HRESULT hr = device->CreateTexture2D(&desc, &init, ring[i].Receive());
        if (FAILED(hr)) {
            std::fwprintf(stderr, L"CreateTexture2D(format %d) failed: 0x%08lX\n",
                          static_cast<int>(desc.Format), static_cast<unsigned long>(hr));
            return 1;
        }
        raw[i] = ring[i].Get();
    }
    context->Flush();

    const uint64_t pixel_rate =
        static_cast<uint64_t>(options.width) * options.height * options.fps;
    std::wprintf(L"config: %ux%u@%u %ls %ls, preset %ls, %u frames\n", options.width,
                 options.height, options.fps, options.hdr ? L"HDR" : L"SDR",
                 options.chroma444 ? L"4:4:4" : L"4:2:0",
                 pixel_rate > 3840ull * 2160ull * 60ull ? L"P4" : L"P5", options.frames);

    zcr::NvencEncoder encoder;
    std::wstring error;
    if (!encoder.Open(device.Get(), settings, error)) {
        std::fwprintf(stderr, L"Open: %ls\n", error.c_str());
        return 1;
    }
    if (!encoder.RegisterInputs(raw.data(), raw.size(), error)) {
        std::fwprintf(stderr, L"RegisterInputs: %ls\n", error.c_str());
        return 1;
    }

    std::vector<uint8_t> header;
    if (!encoder.SequenceHeader(header, error)) {
        std::fwprintf(stderr, L"SequenceHeader: %ls\n", error.c_str());
        return 1;
    }
    std::wprintf(L"sequence header: %zu bytes\n", header.size());
    PrintNals(header, L"  NALs");

    CreateDirectoryW(L"tests\\out", nullptr);
    FILE* file = _wfopen(options.out.c_str(), L"wb");
    if (file == nullptr) {
        std::fwprintf(stderr, L"cannot open %ls\n", options.out.c_str());
        return 1;
    }
    std::setvbuf(file, nullptr, _IOFBF, 8 << 20);

    // One count per ring slot not currently in flight. The writer returns a
    // count as each packet comes back, which is exactly the header's rule for
    // when a slot may be submitted again.
    HANDLE free_slots = CreateSemaphoreW(nullptr, static_cast<LONG>(kSlots),
                                         static_cast<LONG>(kSlots), nullptr);

    using Clock = std::chrono::steady_clock;
    std::atomic<bool> writer_failed{false};
    uint64_t packets = 0;
    uint64_t bytes = 0;
    std::vector<uint64_t> keyframes;
    Clock::time_point last_packet{};

    std::thread writer([&] {
        zcr::EncodedPacket packet;
        std::wstring writer_error;
        uint64_t expected = 0;
        for (;;) {
            const auto result = encoder.Next(packet, 5000, writer_error);
            if (result == zcr::NvencEncoder::Result::Packet) {
                last_packet = Clock::now();
                if (packet.frame_index != expected) {
                    std::fwprintf(stderr, L"out of order: got frame %llu, expected %llu\n",
                                  static_cast<unsigned long long>(packet.frame_index),
                                  static_cast<unsigned long long>(expected));
                    writer_failed = true;
                }
                if (packet.slot != expected % kSlots) {
                    std::fwprintf(stderr, L"frame %llu came back on slot %zu\n",
                                  static_cast<unsigned long long>(packet.frame_index),
                                  packet.slot);
                    writer_failed = true;
                }
                if (packet.keyframe) {
                    keyframes.push_back(packet.frame_index);
                }
                if (packet.frame_index == 0 || packet.frame_index == 1) {
                    PrintNals(packet.data, packet.frame_index == 0 ? L"frame 0" : L"frame 1");
                }
                std::fwrite(packet.data.data(), 1, packet.data.size(), file);
                bytes += packet.data.size();
                ++packets;
                ++expected;
                ReleaseSemaphore(free_slots, 1, nullptr);
            } else if (result == zcr::NvencEncoder::Result::Drained) {
                return;
            } else if (result == zcr::NvencEncoder::Result::Timeout) {
                std::fwprintf(stderr, L"Next: timed out after 5 s\n");
                writer_failed = true;
                ReleaseSemaphore(free_slots, static_cast<LONG>(kSlots), nullptr);
                return;
            } else {
                std::fwprintf(stderr, L"Next: %ls\n", writer_error.c_str());
                writer_failed = true;
                ReleaseSemaphore(free_slots, static_cast<LONG>(kSlots), nullptr);
                return;
            }
        }
    });

    const Clock::time_point start = Clock::now();
    bool submit_failed = false;
    for (uint32_t i = 0; i < options.frames && !writer_failed; ++i) {
        WaitForSingleObject(free_slots, INFINITE);
        if (writer_failed) {
            break;
        }
        if (!encoder.Submit(i % kSlots, i, error)) {
            std::fwprintf(stderr, L"Submit(%u): %ls\n", i, error.c_str());
            submit_failed = true;
            break;
        }
    }
    const Clock::time_point submitted = Clock::now();
    encoder.Flush();
    writer.join();
    const Clock::time_point drained = Clock::now();

    std::fclose(file);
    CloseHandle(free_slots);
    encoder.Close();
    encoder.Close(); // idempotent by contract

    const double encode_s = std::chrono::duration<double>(last_packet - start).count();
    const double flush_ms = std::chrono::duration<double, std::milli>(drained - submitted).count();
    std::wprintf(L"packets: %llu, bytes: %llu (%.1f Mbit/s at %u fps)\n",
                 static_cast<unsigned long long>(packets), static_cast<unsigned long long>(bytes),
                 packets ? bytes * 8.0 / 1e6 / (static_cast<double>(packets) / options.fps) : 0.0,
                 options.fps);
    std::wprintf(L"keyframes:");
    for (const uint64_t k : keyframes) {
        std::wprintf(L" %llu", static_cast<unsigned long long>(k));
    }
    std::wprintf(L"\n");
    if (encode_s > 0.0) {
        std::wprintf(L"encode throughput: %.1f fps (%.2f ms/frame), flush+drain %.1f ms\n",
                     packets / encode_s, encode_s * 1000.0 / static_cast<double>(packets),
                     flush_ms);
    }
    std::wprintf(L"wrote %ls\n", options.out.c_str());

    const bool ok = !submit_failed && !writer_failed && packets == options.frames;
    std::wprintf(L"%ls\n", ok ? L"PASS" : L"FAIL");
    return ok ? 0 : 1;
}
