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
#include <d3d11.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "video_format.h"

namespace zcr {

struct EncoderSettings {
    VideoFormat format;

    /// Constant-quality target for VBR (NV_ENC_RC_PARAMS::targetQuality).
    uint32_t cq = 24;

    /// Peak bitrate cap in kbit/s. 0 = derived from pixel rate: about
    /// 100 Mbit/s at 3840x2160@60, 160 Mbit/s at 3840x2160@120, scaled linearly
    /// in pixels per second and clamped to [20, 200] Mbit/s.
    uint32_t max_kbps = 0;
};

struct EncodedPacket {
    /// One access unit, Annex B (start codes), exactly as NVENC produced it:
    /// VPS/SPS/PPS repeated on every IDR, HDR SEI on every IDR when hdr.
    std::vector<uint8_t> data;
    uint64_t frame_index = 0; // as passed to Submit; no B-frames, so output
                              // order == input order
    bool keyframe = false;    // IDR
    size_t slot = 0;          // input ring slot, free for reuse once this
                              // packet has been returned
};

/// NVENC HEVC on a D3D11 device, async mode: Main10 4:2:0 from P010 textures,
/// or Main 4:4:4 10 from R10G10B10A2 R'G'B' textures that NVENC converts with
/// the VUI matrix (VideoFormat::SurfaceFormat).
///
/// Loads nvEncodeAPI64.dll from System32 (it ships with the driver), opens a
/// session on the caller's ID3D11Device, and encodes textures the caller owns
/// and registers once. Nothing is ever copied to system memory except the
/// finished bitstream.
///
/// Fixed configuration: HEVC Main10 High tier, preset P5 up to 3840x2160@60;
/// above that P4 with split-frame encoding forced across both NVENC engines,
/// because on driver 616 auto mode never splits and one engine tops out at
/// exactly 120 fps at 4K, which leaves no headroom. Tuning HIGH_QUALITY, VBR
/// with targetQuality = cq
/// and maxBitRate as above, no B-frames (frameIntervalP = 1), GOP = IDR period
/// = 2 seconds of frames, repeatSPSPPS, VUI colour description from
/// VideoFormat, and when hdr the mastering display and content light level SEI
/// on every IDR (NV_ENC_PIC_PARAMS_HEVC::pMasteringDisplay / pMaxCll).
///
/// Threading: Submit from the capture thread, Next from the writer thread,
/// concurrently. Open/RegisterInputs/SequenceHeader/Close from one thread while
/// neither is running.
class NvencEncoder {
public:
    NvencEncoder();
    ~NvencEncoder();
    NvencEncoder(const NvencEncoder&) = delete;
    NvencEncoder& operator=(const NvencEncoder&) = delete;

    /// The ID3D11Device must have multithread protection enabled
    /// (ID3D10Multithread::SetMultithreadProtected(TRUE)); the recorder does
    /// this when it creates the device.
    bool Open(ID3D11Device* device, const EncoderSettings& settings, std::wstring& error);

    /// Registers the caller's input ring: format.SurfaceFormat() textures of
    /// exactly format.width x format.height. Allocates one output bitstream buffer and
    /// one completion event per slot.
    bool RegisterInputs(ID3D11Texture2D* const* textures, size_t count, std::wstring& error);

    /// Encodes ring slot `slot` as frame `frame_index` (0, 1, 2, ...). The GPU
    /// work that filled the texture must already be submitted on the immediate
    /// context. The caller must not submit a slot that is still in flight, i.e.
    /// whose packet Next has not yet returned.
    bool Submit(size_t slot, uint64_t frame_index, std::wstring& error);

    enum class Result {
        Packet,   // `out` filled
        Timeout,  // nothing finished within timeout_ms
        Drained,  // Flush was called and everything has been returned
        Error,    // `error` filled; the session is unusable
    };

    /// Waits for the oldest in-flight frame and returns its bitstream.
    Result Next(EncodedPacket& out, DWORD timeout_ms, std::wstring& error);

    /// Sends end-of-stream. Next then returns what remains, then Drained.
    /// Submit must not be called after this.
    void Flush();

    /// VPS + SPS + PPS, Annex B, via nvEncGetSequenceParams. For hvcC.
    bool SequenceHeader(std::vector<uint8_t>& out, std::wstring& error);

    /// Unregisters inputs, frees buffers, destroys the session. Idempotent.
    void Close();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace zcr
