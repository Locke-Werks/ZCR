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
#include "nvenc.h"

#include <nvEncodeAPI.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>

namespace zcr {

namespace {

struct StatusName {
    NVENCSTATUS status;
    const wchar_t* name;
};

constexpr StatusName kStatusNames[] = {
    {NV_ENC_SUCCESS, L"NV_ENC_SUCCESS"},
    {NV_ENC_ERR_NO_ENCODE_DEVICE, L"NV_ENC_ERR_NO_ENCODE_DEVICE"},
    {NV_ENC_ERR_UNSUPPORTED_DEVICE, L"NV_ENC_ERR_UNSUPPORTED_DEVICE"},
    {NV_ENC_ERR_INVALID_ENCODERDEVICE, L"NV_ENC_ERR_INVALID_ENCODERDEVICE"},
    {NV_ENC_ERR_INVALID_DEVICE, L"NV_ENC_ERR_INVALID_DEVICE"},
    {NV_ENC_ERR_DEVICE_NOT_EXIST, L"NV_ENC_ERR_DEVICE_NOT_EXIST"},
    {NV_ENC_ERR_INVALID_PTR, L"NV_ENC_ERR_INVALID_PTR"},
    {NV_ENC_ERR_INVALID_EVENT, L"NV_ENC_ERR_INVALID_EVENT"},
    {NV_ENC_ERR_INVALID_PARAM, L"NV_ENC_ERR_INVALID_PARAM"},
    {NV_ENC_ERR_INVALID_CALL, L"NV_ENC_ERR_INVALID_CALL"},
    {NV_ENC_ERR_OUT_OF_MEMORY, L"NV_ENC_ERR_OUT_OF_MEMORY"},
    {NV_ENC_ERR_ENCODER_NOT_INITIALIZED, L"NV_ENC_ERR_ENCODER_NOT_INITIALIZED"},
    {NV_ENC_ERR_UNSUPPORTED_PARAM, L"NV_ENC_ERR_UNSUPPORTED_PARAM"},
    {NV_ENC_ERR_LOCK_BUSY, L"NV_ENC_ERR_LOCK_BUSY"},
    {NV_ENC_ERR_NOT_ENOUGH_BUFFER, L"NV_ENC_ERR_NOT_ENOUGH_BUFFER"},
    {NV_ENC_ERR_INVALID_VERSION, L"NV_ENC_ERR_INVALID_VERSION"},
    {NV_ENC_ERR_MAP_FAILED, L"NV_ENC_ERR_MAP_FAILED"},
    {NV_ENC_ERR_NEED_MORE_INPUT, L"NV_ENC_ERR_NEED_MORE_INPUT"},
    {NV_ENC_ERR_ENCODER_BUSY, L"NV_ENC_ERR_ENCODER_BUSY"},
    {NV_ENC_ERR_EVENT_NOT_REGISTERD, L"NV_ENC_ERR_EVENT_NOT_REGISTERD"},
    {NV_ENC_ERR_GENERIC, L"NV_ENC_ERR_GENERIC"},
    {NV_ENC_ERR_INCOMPATIBLE_CLIENT_KEY, L"NV_ENC_ERR_INCOMPATIBLE_CLIENT_KEY"},
    {NV_ENC_ERR_UNIMPLEMENTED, L"NV_ENC_ERR_UNIMPLEMENTED"},
    {NV_ENC_ERR_RESOURCE_REGISTER_FAILED, L"NV_ENC_ERR_RESOURCE_REGISTER_FAILED"},
    {NV_ENC_ERR_RESOURCE_NOT_REGISTERED, L"NV_ENC_ERR_RESOURCE_NOT_REGISTERED"},
    {NV_ENC_ERR_RESOURCE_NOT_MAPPED, L"NV_ENC_ERR_RESOURCE_NOT_MAPPED"},
    {NV_ENC_ERR_NEED_MORE_OUTPUT, L"NV_ENC_ERR_NEED_MORE_OUTPUT"},
};

[[nodiscard]] std::wstring StatusText(NVENCSTATUS status)
{
    const wchar_t* name = L"unknown NVENCSTATUS";
    for (const StatusName& entry : kStatusNames) {
        if (entry.status == status) {
            name = entry.name;
            break;
        }
    }
    return std::wstring(name) + L" (" + std::to_wstring(static_cast<int>(status)) + L")";
}

[[nodiscard]] std::wstring WidenUtf8(const char* text)
{
    if (text == nullptr || *text == '\0') {
        return {};
    }
    const int length = static_cast<int>(std::strlen(text));
    const int needed = MultiByteToWideChar(CP_UTF8, 0, text, length, nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, length, out.data(), needed);
    return out;
}

[[nodiscard]] uint32_t DefaultMaxKbps(const VideoFormat& format)
{
    // Two anchor points from the header contract: 100 Mbit/s at 4K60 and
    // 160 Mbit/s at 4K120. The line through them is 40 + 60 * (rate / 4K60).
    constexpr double kReferenceRate = 3840.0 * 2160.0 * 60.0;
    const double rate = static_cast<double>(format.width) * format.height * format.fps;
    const double kbps = 40000.0 + 60000.0 * (rate / kReferenceRate);
    return static_cast<uint32_t>(std::clamp(kbps, 20000.0, 200000.0));
}

// H.265 D.3.28: display_primaries and white_point are in increments of 0.00002,
// valid range [0, 50000].
[[nodiscard]] uint16_t ChromaUnits(double coordinate)
{
    const double units = std::round(coordinate * 50000.0);
    return static_cast<uint16_t>(std::clamp(units, 0.0, 50000.0));
}

// H.265 D.3.28: max/min_display_mastering_luminance in 0.0001 cd/m2.
[[nodiscard]] uint32_t LuminanceUnits(double nits)
{
    const double units = std::round(nits * 10000.0);
    return static_cast<uint32_t>(std::clamp(units, 0.0, 4294967295.0));
}

} // namespace

struct NvencEncoder::Impl {
    struct Slot {
        NV_ENC_REGISTERED_PTR registered = nullptr;
        NV_ENC_OUTPUT_PTR bitstream = nullptr;
        HANDLE event = nullptr;
        bool event_registered = false;
    };

    struct InFlight {
        size_t slot = 0;
        uint64_t frame_index = 0;
        NV_ENC_INPUT_PTR mapped = nullptr;
    };

    HMODULE library = nullptr;
    NV_ENCODE_API_FUNCTION_LIST api{};
    void* encoder = nullptr;
    ID3D11Device* device = nullptr; // not owned; the caller outlives the session

    EncoderSettings settings;
    uint32_t gop = 0;
    MASTERING_DISPLAY_INFO mastering{};
    CONTENT_LIGHT_LEVEL light_level{};

    std::vector<Slot> slots;
    HANDLE eos_event = nullptr;
    bool eos_registered = false;

    // Only the capture thread touches this, so it needs no lock.
    uint64_t submitted = 0;

    // Shared between Submit/Flush (capture thread) and Next (writer thread).
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<InFlight> fifo;
    bool flushed = false;
    bool eos_sent = false;
    bool drained = false;

    [[nodiscard]] std::wstring Failure(const wchar_t* call, NVENCSTATUS status) const
    {
        std::wstring message = std::wstring(call) + L" failed: " + StatusText(status);
        if (encoder != nullptr && api.nvEncGetLastErrorString != nullptr) {
            const std::wstring detail = WidenUtf8(api.nvEncGetLastErrorString(encoder));
            if (!detail.empty()) {
                message += L": ";
                message += detail;
            }
        }
        return message;
    }

    [[nodiscard]] bool Cap(NV_ENC_CAPS cap, int& value, std::wstring& error) const
    {
        NV_ENC_CAPS_PARAM param{};
        param.version = NV_ENC_CAPS_PARAM_VER;
        param.capsToQuery = cap;
        value = 0;
        const NVENCSTATUS status =
            api.nvEncGetEncodeCaps(encoder, NV_ENC_CODEC_HEVC_GUID, &param, &value);
        if (status != NV_ENC_SUCCESS) {
            error = Failure(L"nvEncGetEncodeCaps", status);
            return false;
        }
        return true;
    }

    void Release()
    {
        // Frames still in flight own their mapping and their output buffer.
        // Tearing either down under a running encode is undefined, so give each
        // one a bounded chance to finish before unmapping it.
        for (const InFlight& record : fifo) {
            if (record.slot < slots.size() && slots[record.slot].event != nullptr) {
                WaitForSingleObject(slots[record.slot].event, 2000);
            }
            if (encoder != nullptr && record.mapped != nullptr) {
                api.nvEncUnmapInputResource(encoder, record.mapped);
            }
        }
        fifo.clear();

        for (Slot& slot : slots) {
            if (encoder != nullptr && slot.registered != nullptr) {
                api.nvEncUnregisterResource(encoder, slot.registered);
            }
            if (encoder != nullptr && slot.bitstream != nullptr) {
                api.nvEncDestroyBitstreamBuffer(encoder, slot.bitstream);
            }
            if (encoder != nullptr && slot.event_registered) {
                NV_ENC_EVENT_PARAMS params{};
                params.version = NV_ENC_EVENT_PARAMS_VER;
                params.completionEvent = slot.event;
                api.nvEncUnregisterAsyncEvent(encoder, &params);
            }
            if (slot.event != nullptr) {
                CloseHandle(slot.event);
            }
        }
        slots.clear();

        if (encoder != nullptr && eos_registered) {
            NV_ENC_EVENT_PARAMS params{};
            params.version = NV_ENC_EVENT_PARAMS_VER;
            params.completionEvent = eos_event;
            api.nvEncUnregisterAsyncEvent(encoder, &params);
        }
        eos_registered = false;
        if (eos_event != nullptr) {
            CloseHandle(eos_event);
            eos_event = nullptr;
        }

        if (encoder != nullptr) {
            api.nvEncDestroyEncoder(encoder);
            encoder = nullptr;
        }
        api = NV_ENCODE_API_FUNCTION_LIST{};
        if (library != nullptr) {
            FreeLibrary(library);
            library = nullptr;
        }

        device = nullptr;
        submitted = 0;
        flushed = false;
        eos_sent = false;
        drained = false;
    }
};

NvencEncoder::NvencEncoder() : impl_(std::make_unique<Impl>()) {}

NvencEncoder::~NvencEncoder()
{
    Close();
}

void NvencEncoder::Close()
{
    if (impl_) {
        impl_->Release();
    }
}

bool NvencEncoder::Open(ID3D11Device* device, const EncoderSettings& settings, std::wstring& error)
{
    Close();
    Impl& d = *impl_;
    const VideoFormat& format = settings.format;

    if (device == nullptr) {
        error = L"NVENC: no D3D11 device";
        return false;
    }
    if (format.width == 0 || format.height == 0 || (format.width & 1) || (format.height & 1)) {
        error = L"NVENC: encode size must be non-zero and even, got "
            + std::to_wstring(format.width) + L"x" + std::to_wstring(format.height);
        return false;
    }
    if (format.fps == 0) {
        error = L"NVENC: frame rate must be non-zero";
        return false;
    }

    // Close() on every failure path below, so a half-built session never
    // survives a false return and the object stays reusable.
    const auto fail = [&](std::wstring message) {
        error = std::move(message);
        Close();
        return false;
    };

    // The DLL ships with the display driver into System32. Restricting the
    // search there keeps a planted copy next to the exe from being loaded.
    d.library = LoadLibraryExW(L"nvEncodeAPI64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (d.library == nullptr) {
        return fail(L"NVENC: nvEncodeAPI64.dll could not be loaded from System32 (error "
                    + std::to_wstring(GetLastError())
                    + L"); an NVIDIA display driver with NVENC is required");
    }

    using GetMaxVersionFn = NVENCSTATUS(NVENCAPI*)(uint32_t*);
    using CreateInstanceFn = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);
    const auto get_max_version = reinterpret_cast<GetMaxVersionFn>(
        reinterpret_cast<void*>(GetProcAddress(d.library, "NvEncodeAPIGetMaxSupportedVersion")));
    const auto create_instance = reinterpret_cast<CreateInstanceFn>(
        reinterpret_cast<void*>(GetProcAddress(d.library, "NvEncodeAPICreateInstance")));
    if (get_max_version == nullptr || create_instance == nullptr) {
        return fail(L"NVENC: nvEncodeAPI64.dll is missing its entry points");
    }

    uint32_t max_version = 0;
    NVENCSTATUS status = get_max_version(&max_version);
    if (status != NV_ENC_SUCCESS) {
        return fail(d.Failure(L"NvEncodeAPIGetMaxSupportedVersion", status));
    }
    // The driver reports major << 4 | minor, not the NVENCAPI_VERSION layout.
    constexpr uint32_t kRequired = (NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION;
    if (max_version < kRequired) {
        return fail(L"NVIDIA driver too old for NVENC API "
                    + std::to_wstring(NVENCAPI_MAJOR_VERSION) + L"."
                    + std::to_wstring(NVENCAPI_MINOR_VERSION) + L" (driver supports "
                    + std::to_wstring(max_version >> 4) + L"."
                    + std::to_wstring(max_version & 0xF) + L"); update the driver");
    }

    d.api.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    status = create_instance(&d.api);
    if (status != NV_ENC_SUCCESS) {
        return fail(d.Failure(L"NvEncodeAPICreateInstance", status));
    }

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS session{};
    session.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    session.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    session.device = device;
    session.apiVersion = NVENCAPI_VERSION;
    status = d.api.nvEncOpenEncodeSessionEx(&session, &d.encoder);
    if (status != NV_ENC_SUCCESS) {
        // Take the message before Close destroys the half-open session, which
        // the SDK requires even when the open itself failed.
        return fail(d.Failure(L"nvEncOpenEncodeSessionEx", status)
                    + L" (is the D3D11 device on the NVIDIA adapter?)");
    }
    d.device = device;

    uint32_t guid_count = 0;
    status = d.api.nvEncGetEncodeGUIDCount(d.encoder, &guid_count);
    if (status != NV_ENC_SUCCESS) {
        return fail(d.Failure(L"nvEncGetEncodeGUIDCount", status));
    }
    std::vector<GUID> guids(guid_count);
    uint32_t guid_written = 0;
    if (guid_count > 0) {
        status = d.api.nvEncGetEncodeGUIDs(d.encoder, guids.data(), guid_count, &guid_written);
        if (status != NV_ENC_SUCCESS) {
            return fail(d.Failure(L"nvEncGetEncodeGUIDs", status));
        }
    }
    guids.resize(guid_written);
    if (std::find(guids.begin(), guids.end(), NV_ENC_CODEC_HEVC_GUID) == guids.end()) {
        return fail(L"NVENC: this GPU does not encode HEVC");
    }

    int cap = 0;
    if (!d.Cap(NV_ENC_CAPS_SUPPORT_10BIT_ENCODE, cap, error)) {
        Close();
        return false;
    }
    if (cap == 0) {
        return fail(L"NVENC: this GPU does not encode 10-bit HEVC (Main10)");
    }
    if (format.chroma444) {
        if (!d.Cap(NV_ENC_CAPS_SUPPORT_YUV444_ENCODE, cap, error)) {
            Close();
            return false;
        }
        if (cap == 0) {
            return fail(L"NVENC: this GPU does not encode 4:4:4 HEVC; switch chroma to 4:2:0");
        }
    }
    if (!d.Cap(NV_ENC_CAPS_ASYNC_ENCODE_SUPPORT, cap, error)) {
        Close();
        return false;
    }
    if (cap == 0) {
        return fail(L"NVENC: asynchronous encode is not supported on this device");
    }
    int max_width = 0;
    int max_height = 0;
    if (!d.Cap(NV_ENC_CAPS_WIDTH_MAX, max_width, error)
        || !d.Cap(NV_ENC_CAPS_HEIGHT_MAX, max_height, error)) {
        Close();
        return false;
    }
    if (format.width > static_cast<uint32_t>(max_width)
        || format.height > static_cast<uint32_t>(max_height)) {
        return fail(L"NVENC: " + std::to_wstring(format.width) + L"x"
                    + std::to_wstring(format.height) + L" exceeds the HEVC limit of "
                    + std::to_wstring(max_width) + L"x" + std::to_wstring(max_height));
    }

    // P5 is the quality target. Above 4K60 a single engine at P5 falls behind,
    // so drop to P4 and keep up.
    constexpr uint64_t k4k60 = 3840ull * 2160ull * 60ull;
    const uint64_t pixel_rate = static_cast<uint64_t>(format.width) * format.height * format.fps;
    const GUID preset_guid = pixel_rate > k4k60 ? NV_ENC_PRESET_P4_GUID : NV_ENC_PRESET_P5_GUID;

    NV_ENC_PRESET_CONFIG preset{};
    preset.version = NV_ENC_PRESET_CONFIG_VER;
    preset.presetCfg.version = NV_ENC_CONFIG_VER;
    status = d.api.nvEncGetEncodePresetConfigEx(d.encoder, NV_ENC_CODEC_HEVC_GUID, preset_guid,
                                                NV_ENC_TUNING_INFO_HIGH_QUALITY, &preset);
    if (status != NV_ENC_SUCCESS) {
        return fail(d.Failure(L"nvEncGetEncodePresetConfigEx", status));
    }

    d.settings = settings;
    d.gop = format.fps * 2;

    NV_ENC_CONFIG config = preset.presetCfg;
    config.version = NV_ENC_CONFIG_VER;
    // FREXT is Main 4:4:4 10 once chromaFormatIDC is 3 and the depth is 10.
    config.profileGUID =
        format.chroma444 ? NV_ENC_HEVC_PROFILE_FREXT_GUID : NV_ENC_HEVC_PROFILE_MAIN10_GUID;
    config.gopLength = d.gop;
    // No B-frames: output order equals input order, which is what lets Next
    // hand packets back strictly FIFO and the muxer skip composition offsets.
    config.frameIntervalP = 1;

    NV_ENC_RC_PARAMS& rc = config.rcParams;
    rc.rateControlMode = NV_ENC_PARAMS_RC_VBR;
    // Zero average with a target quality makes VBR behave as capped constant
    // quality: spend what cq needs, never more than maxBitRate.
    rc.averageBitRate = 0;
    rc.maxBitRate = (settings.max_kbps != 0 ? settings.max_kbps : DefaultMaxKbps(format)) * 1000u;
    rc.targetQuality = static_cast<uint8_t>(std::min<uint32_t>(settings.cq, 51));
    rc.targetQualityLSB = 0;
    // Lookahead would make NvEncEncodePicture return NEED_MORE_INPUT and hold
    // frames back, breaking the one-in, one-out contract Next relies on.
    rc.enableLookahead = 0;
    rc.lookaheadDepth = 0;
    rc.multiPass = NV_ENC_TWO_PASS_QUARTER_RESOLUTION;

    NV_ENC_CONFIG_HEVC& hevc = config.encodeCodecConfig.hevcConfig;
    // High tier lets the stream sit at level 5.1/5.2 even at 4K120 bitrates;
    // Main tier would push the level to 6.x, which many decoders refuse.
    hevc.tier = NV_ENC_TIER_HEVC_HIGH;
    hevc.level = NV_ENC_LEVEL_AUTOSELECT;
    hevc.chromaFormatIDC = format.chroma444 ? 3 : 1;
    hevc.inputBitDepth = NV_ENC_BIT_DEPTH_10;
    hevc.outputBitDepth = NV_ENC_BIT_DEPTH_10;
    hevc.idrPeriod = d.gop;
    hevc.repeatSPSPPS = 1;
    // The HQ preset turns these on for its B-frame GOP; with frameIntervalP = 1
    // they are invalid and fail initialization.
    hevc.useBFramesAsRef = NV_ENC_BFRAME_REF_MODE_DISABLED;
    hevc.tfLevel = NV_ENC_TEMPORAL_FILTER_LEVEL_0;

    NV_ENC_CONFIG_HEVC_VUI_PARAMETERS& vui = hevc.hevcVUIParameters;
    vui.videoSignalTypePresentFlag = 1;
    vui.videoFormat = NV_ENC_VUI_VIDEO_FORMAT_UNSPECIFIED;
    vui.videoFullRangeFlag = format.FullRange() ? 1 : 0;
    vui.colourDescriptionPresentFlag = 1;
    vui.colourPrimaries = static_cast<NV_ENC_VUI_COLOR_PRIMARIES>(format.ColourPrimaries());
    vui.transferCharacteristics =
        static_cast<NV_ENC_VUI_TRANSFER_CHARACTERISTIC>(format.TransferCharacteristics());
    // With R'G'B' input (4:4:4) these two fields are also what NVENC converts
    // with: measured on an RTX 4090, driver 616.56, flat 10-bit patches came out
    // exactly on the BT.2020 limited-range codes with matrix 9 and on BT.709 with
    // matrix 1. So the VUI cannot disagree with the pixels it describes.
    vui.colourMatrix = static_cast<NV_ENC_VUI_MATRIX_COEFFS>(format.MatrixCoefficients());

    if (format.hdr) {
        hevc.outputMasteringDisplay = 1;
        hevc.outputMaxCll = 1;

        const HdrMetadata& meta = format.hdr_meta;
        d.mastering.g = {ChromaUnits(meta.green_x), ChromaUnits(meta.green_y)};
        d.mastering.b = {ChromaUnits(meta.blue_x), ChromaUnits(meta.blue_y)};
        d.mastering.r = {ChromaUnits(meta.red_x), ChromaUnits(meta.red_y)};
        d.mastering.whitePoint = {ChromaUnits(meta.white_x), ChromaUnits(meta.white_y)};
        d.mastering.maxLuma = LuminanceUnits(meta.max_mastering_nits);
        d.mastering.minLuma = LuminanceUnits(meta.min_mastering_nits);
        d.light_level.maxContentLightLevel = meta.max_cll;
        d.light_level.maxPicAverageLightLevel = meta.max_fall;
    }

    NV_ENC_INITIALIZE_PARAMS init{};
    init.version = NV_ENC_INITIALIZE_PARAMS_VER;
    init.encodeGUID = NV_ENC_CODEC_HEVC_GUID;
    init.presetGUID = preset_guid;
    init.encodeWidth = format.width;
    init.encodeHeight = format.height;
    init.darWidth = format.width;
    init.darHeight = format.height;
    init.frameRateNum = format.fps;
    init.frameRateDen = 1;
    init.enableEncodeAsync = 1;
    init.enablePTD = 1;
    // Plain auto mode never splits at 4K P4 HIGH_QUALITY: measured on an RTX
    // 4090 (driver 616.56) it encodes on one engine at 120 fps flat, no headroom
    // for 4K120. Auto-forced splits each frame across both engines, about 160
    // fps, and on a single-engine part it quietly falls back to one strip.
    // Below that rate one engine keeps up and an unsplit frame avoids the
    // strip seams, so auto stays the default there.
    init.splitEncodeMode = pixel_rate > k4k60 ? NV_ENC_SPLIT_AUTO_FORCED_MODE
                                              : NV_ENC_SPLIT_AUTO_MODE;
    init.encodeConfig = &config;
    init.tuningInfo = NV_ENC_TUNING_INFO_HIGH_QUALITY;

    status = d.api.nvEncInitializeEncoder(d.encoder, &init);
    if (status != NV_ENC_SUCCESS) {
        return fail(d.Failure(L"nvEncInitializeEncoder", status));
    }

    // Auto-reset, as every completion event here is waited on exactly once.
    d.eos_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (d.eos_event == nullptr) {
        return fail(L"NVENC: CreateEventW failed (error " + std::to_wstring(GetLastError()) + L")");
    }
    NV_ENC_EVENT_PARAMS event_params{};
    event_params.version = NV_ENC_EVENT_PARAMS_VER;
    event_params.completionEvent = d.eos_event;
    status = d.api.nvEncRegisterAsyncEvent(d.encoder, &event_params);
    if (status != NV_ENC_SUCCESS) {
        return fail(d.Failure(L"nvEncRegisterAsyncEvent", status));
    }
    d.eos_registered = true;

    return true;
}

bool NvencEncoder::RegisterInputs(ID3D11Texture2D* const* textures, size_t count,
                                  std::wstring& error)
{
    Impl& d = *impl_;
    if (d.encoder == nullptr) {
        error = L"NVENC: RegisterInputs called before Open";
        return false;
    }
    if (!d.slots.empty()) {
        error = L"NVENC: inputs are already registered for this session";
        return false;
    }
    if (textures == nullptr || count == 0) {
        error = L"NVENC: RegisterInputs needs at least one texture";
        return false;
    }

    const VideoFormat& format = d.settings.format;
    d.slots.resize(count);
    for (size_t i = 0; i < count; ++i) {
        const std::wstring which = L"input slot " + std::to_wstring(i);
        ID3D11Texture2D* texture = textures[i];
        if (texture == nullptr) {
            error = L"NVENC: " + which + L" is null";
            return false;
        }

        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        if (desc.Format != format.SurfaceFormat() || desc.Width != format.width
            || desc.Height != format.height) {
            error = L"NVENC: " + which + L" must be DXGI format "
                + std::to_wstring(static_cast<int>(format.SurfaceFormat())) + L" at "
                + std::to_wstring(format.width) + L"x" + std::to_wstring(format.height)
                + L", got format " + std::to_wstring(static_cast<int>(desc.Format)) + L" at "
                + std::to_wstring(desc.Width) + L"x" + std::to_wstring(desc.Height);
            return false;
        }
        if (desc.MipLevels != 1 || desc.ArraySize != 1 || desc.SampleDesc.Count != 1) {
            error = L"NVENC: " + which
                + L" must have one mip level, one array slice and no multisampling";
            return false;
        }
        ID3D11Device* owner = nullptr;
        texture->GetDevice(&owner);
        const bool same_device = owner == d.device;
        if (owner != nullptr) {
            owner->Release();
        }
        if (!same_device) {
            error = L"NVENC: " + which + L" was created on a different D3D11 device";
            return false;
        }

        Impl::Slot& slot = d.slots[i];

        NV_ENC_REGISTER_RESOURCE reg{};
        reg.version = NV_ENC_REGISTER_RESOURCE_VER;
        reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
        reg.width = format.width;
        reg.height = format.height;
        reg.pitch = 0; // DirectX resources carry their own pitch
        reg.subResourceIndex = 0;
        reg.resourceToRegister = texture;
        // ABGR10 is the word-ordered name for DXGI R10G10B10A2: red in the low bits.
        reg.bufferFormat = format.chroma444 ? NV_ENC_BUFFER_FORMAT_ABGR10
                                            : NV_ENC_BUFFER_FORMAT_YUV420_10BIT;
        reg.bufferUsage = NV_ENC_INPUT_IMAGE;
        NVENCSTATUS status = d.api.nvEncRegisterResource(d.encoder, &reg);
        if (status != NV_ENC_SUCCESS) {
            error = d.Failure(L"nvEncRegisterResource", status) + L" (" + which + L")";
            return false;
        }
        slot.registered = reg.registeredResource;

        NV_ENC_CREATE_BITSTREAM_BUFFER bitstream{};
        bitstream.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
        status = d.api.nvEncCreateBitstreamBuffer(d.encoder, &bitstream);
        if (status != NV_ENC_SUCCESS) {
            error = d.Failure(L"nvEncCreateBitstreamBuffer", status) + L" (" + which + L")";
            return false;
        }
        slot.bitstream = bitstream.bitstreamBuffer;

        slot.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (slot.event == nullptr) {
            error = L"NVENC: CreateEventW failed for " + which + L" (error "
                + std::to_wstring(GetLastError()) + L")";
            return false;
        }
        NV_ENC_EVENT_PARAMS event_params{};
        event_params.version = NV_ENC_EVENT_PARAMS_VER;
        event_params.completionEvent = slot.event;
        status = d.api.nvEncRegisterAsyncEvent(d.encoder, &event_params);
        if (status != NV_ENC_SUCCESS) {
            error = d.Failure(L"nvEncRegisterAsyncEvent", status) + L" (" + which + L")";
            return false;
        }
        slot.event_registered = true;
    }
    return true;
}

bool NvencEncoder::Submit(size_t slot_index, uint64_t frame_index, std::wstring& error)
{
    Impl& d = *impl_;
    if (d.encoder == nullptr) {
        error = L"NVENC: Submit called before Open";
        return false;
    }
    if (slot_index >= d.slots.size()) {
        error = L"NVENC: Submit slot " + std::to_wstring(slot_index) + L" out of range (ring has "
            + std::to_wstring(d.slots.size()) + L")";
        return false;
    }
    {
        std::lock_guard lock(d.mutex);
        if (d.flushed) {
            error = L"NVENC: Submit called after Flush";
            return false;
        }
    }

    Impl::Slot& slot = d.slots[slot_index];

    NV_ENC_MAP_INPUT_RESOURCE map{};
    map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    map.registeredResource = slot.registered;
    NVENCSTATUS status = d.api.nvEncMapInputResource(d.encoder, &map);
    if (status != NV_ENC_SUCCESS) {
        error = d.Failure(L"nvEncMapInputResource", status);
        return false;
    }

    const VideoFormat& format = d.settings.format;
    NV_ENC_PIC_PARAMS pic{};
    pic.version = NV_ENC_PIC_PARAMS_VER;
    pic.inputWidth = format.width;
    pic.inputHeight = format.height;
    pic.inputPitch = format.width; // ignored for DirectX input, which knows its pitch
    pic.inputBuffer = map.mappedResource;
    pic.bufferFmt = map.mappedBufferFmt;
    pic.outputBitstream = slot.bitstream;
    pic.completionEvent = slot.event;
    pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    pic.inputTimeStamp = frame_index;
    pic.frameIdx = static_cast<uint32_t>(frame_index);
    if (d.submitted == 0) {
        pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
    }
    // PTD places an IDR every `gop` pictures counted from the first one this
    // session saw, so our own submit count predicts exactly which pictures are
    // IDR regardless of any gaps the caller leaves in frame_index.
    if (format.hdr && d.submitted % d.gop == 0) {
        pic.codecPicParams.hevcPicParams.pMasteringDisplay = &d.mastering;
        pic.codecPicParams.hevcPicParams.pMaxCll = &d.light_level;
    }

    status = d.api.nvEncEncodePicture(d.encoder, &pic);
    if (status != NV_ENC_SUCCESS) {
        d.api.nvEncUnmapInputResource(d.encoder, map.mappedResource);
        if (status == NV_ENC_ERR_NEED_MORE_INPUT) {
            error = L"nvEncEncodePicture returned NV_ENC_ERR_NEED_MORE_INPUT: the encoder "
                    L"buffered frame " + std::to_wstring(frame_index)
                + L" instead of encoding it, which only happens with B-frames or lookahead. "
                  L"The session configuration is wrong and cannot continue.";
        } else {
            error = d.Failure(L"nvEncEncodePicture", status);
        }
        return false;
    }
    ++d.submitted;

    // Queued after the encode call rather than before: if it failed there is
    // nothing to wait for, and if it already completed the auto-reset event
    // stays signalled until Next consumes it.
    {
        std::lock_guard lock(d.mutex);
        d.fifo.push_back({slot_index, frame_index, map.mappedResource});
    }
    d.cv.notify_one();
    return true;
}

NvencEncoder::Result NvencEncoder::Next(EncodedPacket& out, DWORD timeout_ms, std::wstring& error)
{
    Impl& d = *impl_;
    if (d.encoder == nullptr) {
        error = L"NVENC: Next called before Open";
        return Result::Error;
    }

    using Clock = std::chrono::steady_clock;
    const bool forever = timeout_ms == INFINITE;
    const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    const auto remaining = [&]() -> DWORD {
        if (forever) {
            return INFINITE;
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - Clock::now()).count();
        return left > 0 ? static_cast<DWORD>(left) : 0;
    };

    Impl::InFlight head;
    bool drain = false;
    bool wait_eos = false;
    {
        std::unique_lock lock(d.mutex);
        const auto ready = [&] { return !d.fifo.empty() || d.flushed; };
        if (forever) {
            d.cv.wait(lock, ready);
        } else if (!d.cv.wait_until(lock, deadline, ready)) {
            return Result::Timeout;
        }
        if (d.fifo.empty()) {
            if (d.drained) {
                return Result::Drained;
            }
            drain = true;
            wait_eos = d.eos_sent;
        } else {
            head = d.fifo.front();
        }
    }

    if (drain) {
        // Every picture has already been handed back through its own event,
        // so the EOS event only confirms the encoder has retired the stream.
        if (wait_eos) {
            const DWORD wait = WaitForSingleObject(d.eos_event, remaining());
            if (wait == WAIT_TIMEOUT) {
                return Result::Timeout;
            }
            if (wait != WAIT_OBJECT_0) {
                error = L"NVENC: waiting for end of stream failed (error "
                    + std::to_wstring(GetLastError()) + L")";
                return Result::Error;
            }
        }
        std::lock_guard lock(d.mutex);
        d.drained = true;
        return Result::Drained;
    }

    Impl::Slot& slot = d.slots[head.slot];
    const DWORD wait = WaitForSingleObject(slot.event, remaining());
    if (wait == WAIT_TIMEOUT) {
        return Result::Timeout;
    }
    if (wait != WAIT_OBJECT_0) {
        error = L"NVENC: waiting for frame " + std::to_wstring(head.frame_index)
            + L" failed (error " + std::to_wstring(GetLastError()) + L")";
        return Result::Error;
    }

    NV_ENC_LOCK_BITSTREAM lock_params{};
    lock_params.version = NV_ENC_LOCK_BITSTREAM_VER;
    lock_params.outputBitstream = slot.bitstream;
    lock_params.doNotWait = 0;
    NVENCSTATUS status = d.api.nvEncLockBitstream(d.encoder, &lock_params);
    if (status != NV_ENC_SUCCESS) {
        error = d.Failure(L"nvEncLockBitstream", status) + L" (frame "
            + std::to_wstring(head.frame_index) + L")";
        return Result::Error;
    }

    const auto* bytes = static_cast<const uint8_t*>(lock_params.bitstreamBufferPtr);
    out.data.assign(bytes, bytes + lock_params.bitstreamSizeInBytes);
    out.frame_index = lock_params.outputTimeStamp;
    out.keyframe = lock_params.pictureType == NV_ENC_PIC_TYPE_IDR;
    out.slot = head.slot;

    status = d.api.nvEncUnlockBitstream(d.encoder, slot.bitstream);
    if (status != NV_ENC_SUCCESS) {
        error = d.Failure(L"nvEncUnlockBitstream", status);
        return Result::Error;
    }
    status = d.api.nvEncUnmapInputResource(d.encoder, head.mapped);
    if (status != NV_ENC_SUCCESS) {
        error = d.Failure(L"nvEncUnmapInputResource", status);
        return Result::Error;
    }

    {
        std::lock_guard lock(d.mutex);
        d.fifo.pop_front();
    }
    return Result::Packet;
}

void NvencEncoder::Flush()
{
    Impl& d = *impl_;
    {
        std::lock_guard lock(d.mutex);
        if (d.flushed) {
            return;
        }
    }

    bool sent = false;
    if (d.encoder != nullptr) {
        // In async mode the EOS picture carries its own registered event; the
        // driver signals it once every earlier picture has completed.
        NV_ENC_PIC_PARAMS eos{};
        eos.version = NV_ENC_PIC_PARAMS_VER;
        eos.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
        eos.completionEvent = d.eos_event;
        sent = d.api.nvEncEncodePicture(d.encoder, &eos) == NV_ENC_SUCCESS;
    }

    {
        std::lock_guard lock(d.mutex);
        d.flushed = true;
        d.eos_sent = sent;
    }
    d.cv.notify_all();
}

bool NvencEncoder::SequenceHeader(std::vector<uint8_t>& out, std::wstring& error)
{
    Impl& d = *impl_;
    if (d.encoder == nullptr) {
        error = L"NVENC: SequenceHeader called before Open";
        return false;
    }

    // VPS + SPS + PPS with VUI is a few hundred bytes; 4 KB leaves room for
    // any extension the driver decides to add.
    std::vector<uint8_t> buffer(4096);
    uint32_t written = 0;
    NV_ENC_SEQUENCE_PARAM_PAYLOAD payload{};
    payload.version = NV_ENC_SEQUENCE_PARAM_PAYLOAD_VER;
    payload.inBufferSize = static_cast<uint32_t>(buffer.size());
    payload.spsppsBuffer = buffer.data();
    payload.outSPSPPSPayloadSize = &written;
    const NVENCSTATUS status = d.api.nvEncGetSequenceParams(d.encoder, &payload);
    if (status != NV_ENC_SUCCESS) {
        error = d.Failure(L"nvEncGetSequenceParams", status);
        return false;
    }
    buffer.resize(std::min<size_t>(written, buffer.size()));
    out = std::move(buffer);
    return true;
}

} // namespace zcr
