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
#include "mp4mux.h"

#include "win32.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

namespace zcr {
namespace {

constexpr uint32_t kMovieTimescale = 1000;
constexpr uint32_t kTrackTimescale = 90000;
constexpr uint32_t kTrackId = 1;
constexpr uint32_t kFirstAudioTrackId = kTrackId + 1;

constexpr uint32_t kAacFrameSamples = 1024;

// About 10 s of 48 kHz AAC. Audio normally rides out with the video fragment
// its keyframe closes; this only matters when video stops arriving at all, and
// then an audio-only fragment keeps the buffer from growing for as long as the
// stall lasts.
constexpr size_t kMaxPendingAudioFrames = 470;

constexpr uint8_t kNalVps = 32;
constexpr uint8_t kNalSps = 33;
constexpr uint8_t kNalPps = 34;
constexpr uint8_t kNalAud = 35;

// ISO/IEC 14496-12 8.8.3.1 sample flags. Only sample_depends_on and
// sample_is_non_sync_sample are set: players seek on is_non_sync, and
// depends_on lets a trick-play reader skip straight to the I frames.
constexpr uint32_t kSyncSampleFlags = 0x02000000;
constexpr uint32_t kNonSyncSampleFlags = 0x01010000;

constexpr uint32_t kTfhdDefaultSampleDuration = 0x000008;
constexpr uint32_t kTfhdDefaultSampleFlags = 0x000020;
constexpr uint32_t kTfhdDefaultBaseIsMoof = 0x020000;
constexpr uint32_t kTrunDataOffset = 0x000001;
constexpr uint32_t kTrunSampleSize = 0x000200;
constexpr uint32_t kTrunSampleFlags = 0x000400;

// ---------------------------------------------------------------------------
// Box building
// ---------------------------------------------------------------------------

class Bytes {
public:
    void U8(uint32_t v) { buf_.push_back(static_cast<uint8_t>(v)); }

    void U16(uint32_t v)
    {
        U8(v >> 8);
        U8(v);
    }

    void U24(uint32_t v)
    {
        U8(v >> 16);
        U16(v);
    }

    void U32(uint32_t v)
    {
        U16(v >> 16);
        U16(v);
    }

    void U64(uint64_t v)
    {
        U32(static_cast<uint32_t>(v >> 32));
        U32(static_cast<uint32_t>(v));
    }

    void Tag(const char (&fourcc)[5]) { Append(fourcc, 4); }

    void Append(const void* data, size_t size)
    {
        const auto* p = static_cast<const uint8_t*>(data);
        buf_.insert(buf_.end(), p, p + size);
    }

    void Zeros(size_t count) { buf_.insert(buf_.end(), count, 0); }

    /// Opens a box whose size is patched by End, so nesting is written in the
    /// order the file reads instead of being sized up front.
    [[nodiscard]] size_t Begin(const char (&fourcc)[5])
    {
        const size_t start = buf_.size();
        U32(0);
        Tag(fourcc);
        return start;
    }

    [[nodiscard]] size_t BeginFull(const char (&fourcc)[5], uint8_t version, uint32_t flags)
    {
        const size_t start = Begin(fourcc);
        U8(version);
        U24(flags);
        return start;
    }

    void End(size_t start) { PatchU32(start, static_cast<uint32_t>(buf_.size() - start)); }

    void PatchU32(size_t at, uint32_t v)
    {
        buf_[at] = static_cast<uint8_t>(v >> 24);
        buf_[at + 1] = static_cast<uint8_t>(v >> 16);
        buf_[at + 2] = static_cast<uint8_t>(v >> 8);
        buf_[at + 3] = static_cast<uint8_t>(v);
    }

    [[nodiscard]] size_t Size() const { return buf_.size(); }
    [[nodiscard]] const uint8_t* Data() const { return buf_.data(); }
    [[nodiscard]] std::vector<uint8_t>& Raw() { return buf_; }
    void Clear() { buf_.clear(); }

private:
    std::vector<uint8_t> buf_;
};

void WriteUnityMatrix(Bytes& b)
{
    const uint32_t matrix[9] = {0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000};
    for (uint32_t v : matrix) {
        b.U32(v);
    }
}

/// ISO/IEC 14496-1 descriptors store their size in 7-bit groups. The four-byte
/// form is always used, as ffmpeg does, so the size can be patched afterwards
/// the same way a box size is.
[[nodiscard]] size_t BeginDescriptor(Bytes& b, uint8_t tag)
{
    b.U8(tag);
    const size_t start = b.Size();
    b.U32(0);
    return start;
}

void EndDescriptor(Bytes& b, size_t start)
{
    const size_t size = b.Size() - start - 4;
    std::vector<uint8_t>& raw = b.Raw();
    raw[start] = static_cast<uint8_t>(0x80 | ((size >> 21) & 0x7F));
    raw[start + 1] = static_cast<uint8_t>(0x80 | ((size >> 14) & 0x7F));
    raw[start + 2] = static_cast<uint8_t>(0x80 | ((size >> 7) & 0x7F));
    raw[start + 3] = static_cast<uint8_t>(size & 0x7F);
}

[[nodiscard]] std::string ToUtf8(const std::wstring& text)
{
    if (text.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(),
                        length, nullptr, nullptr);
    return out;
}

// ---------------------------------------------------------------------------
// Annex B and SPS parsing
// ---------------------------------------------------------------------------

[[nodiscard]] uint8_t NalType(const uint8_t* nal) { return (nal[0] >> 1) & 0x3F; }

/// Calls `fn(nal, size)` for every NAL unit in an Annex B buffer. Trailing zero
/// bytes are dropped from each unit: they are either the leading byte of the
/// next four-byte start code or trailing_zero_8bits, never payload, because an
/// RBSP always ends in a stop bit.
template <typename Fn>
void ForEachNal(const uint8_t* p, size_t n, Fn&& fn)
{
    size_t i = 0;
    size_t nal_start = SIZE_MAX;
    while (i + 2 < n) {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
            if (nal_start != SIZE_MAX) {
                size_t end = i;
                while (end > nal_start && p[end - 1] == 0) {
                    --end;
                }
                if (end - nal_start >= 2) {
                    fn(p + nal_start, end - nal_start);
                }
            }
            i += 3;
            nal_start = i;
        } else {
            ++i;
        }
    }
    if (nal_start != SIZE_MAX && nal_start < n) {
        size_t end = n;
        while (end > nal_start && p[end - 1] == 0) {
            --end;
        }
        if (end - nal_start >= 2) {
            fn(p + nal_start, end - nal_start);
        }
    }
}

/// NAL payload with emulation_prevention_three_byte removed. The SPS is parsed
/// bit by bit and an unstripped 0x03 shifts every field after it.
[[nodiscard]] std::vector<uint8_t> ToRbsp(const uint8_t* nal, size_t size)
{
    std::vector<uint8_t> rbsp;
    rbsp.reserve(size);
    int zeros = 0;
    for (size_t i = 0; i < size; ++i) {
        const uint8_t b = nal[i];
        if (zeros >= 2 && b == 3) {
            zeros = 0;
            continue;
        }
        zeros = (b == 0) ? zeros + 1 : 0;
        rbsp.push_back(b);
    }
    return rbsp;
}

class BitReader {
public:
    explicit BitReader(const std::vector<uint8_t>& data) : data_(data) {}

    uint32_t Bits(int count)
    {
        uint32_t v = 0;
        for (int i = 0; i < count; ++i) {
            v = (v << 1) | Bit();
        }
        return v;
    }

    uint32_t Bit()
    {
        if (pos_ >= data_.size() * 8) {
            overrun_ = true;
            return 0;
        }
        const uint32_t bit = (data_[pos_ / 8] >> (7 - pos_ % 8)) & 1u;
        ++pos_;
        return bit;
    }

    void Skip(size_t count)
    {
        pos_ += count;
        if (pos_ > data_.size() * 8) {
            overrun_ = true;
        }
    }

    uint32_t Ue()
    {
        int leading = 0;
        while (Bit() == 0) {
            if (overrun_ || ++leading > 31) {
                overrun_ = true;
                return 0;
            }
        }
        if (leading == 0) {
            return 0;
        }
        return ((1u << leading) - 1) + Bits(leading);
    }

    [[nodiscard]] bool Overrun() const { return overrun_; }

private:
    const std::vector<uint8_t>& data_;
    size_t pos_ = 0;
    bool overrun_ = false;
};

struct SpsInfo {
    uint8_t profile_space = 0;
    uint8_t tier = 0;
    uint8_t profile_idc = 0;
    uint32_t compatibility = 0;
    uint64_t constraints = 0; // 48 bits
    uint8_t level_idc = 0;
    uint8_t max_sub_layers_minus1 = 0;
    bool temporal_id_nesting = false;
    uint8_t chroma_format_idc = 1;
    uint8_t bit_depth_luma_minus8 = 0;
    uint8_t bit_depth_chroma_minus8 = 0;
};

/// H.265 7.3.2.2.1 as far as bit_depth_chroma_minus8, which is everything hvcC
/// needs. `sps` is the whole NAL unit including its two-byte header.
[[nodiscard]] bool ParseSps(const uint8_t* sps, size_t size, SpsInfo& out)
{
    const std::vector<uint8_t> rbsp = ToRbsp(sps, size);
    BitReader r(rbsp);
    r.Skip(16); // nal_unit_header

    r.Skip(4); // sps_video_parameter_set_id
    out.max_sub_layers_minus1 = static_cast<uint8_t>(r.Bits(3));
    out.temporal_id_nesting = r.Bit() != 0;

    out.profile_space = static_cast<uint8_t>(r.Bits(2));
    out.tier = static_cast<uint8_t>(r.Bit());
    out.profile_idc = static_cast<uint8_t>(r.Bits(5));
    out.compatibility = r.Bits(32);
    out.constraints = (static_cast<uint64_t>(r.Bits(24)) << 24) | r.Bits(24);
    out.level_idc = static_cast<uint8_t>(r.Bits(8));

    const int sub_layers = out.max_sub_layers_minus1;
    bool profile_present[8] = {};
    bool level_present[8] = {};
    for (int i = 0; i < sub_layers; ++i) {
        profile_present[i] = r.Bit() != 0;
        level_present[i] = r.Bit() != 0;
    }
    if (sub_layers > 0) {
        for (int i = sub_layers; i < 8; ++i) {
            r.Skip(2); // reserved_zero_2bits
        }
    }
    for (int i = 0; i < sub_layers; ++i) {
        if (profile_present[i]) {
            r.Skip(88);
        }
        if (level_present[i]) {
            r.Skip(8);
        }
    }

    r.Ue(); // sps_seq_parameter_set_id
    const uint32_t chroma = r.Ue();
    if (chroma > 3) {
        return false;
    }
    out.chroma_format_idc = static_cast<uint8_t>(chroma);
    if (chroma == 3) {
        r.Skip(1); // separate_colour_plane_flag
    }
    r.Ue(); // pic_width_in_luma_samples
    r.Ue(); // pic_height_in_luma_samples
    if (r.Bit()) { // conformance_window_flag
        r.Ue();
        r.Ue();
        r.Ue();
        r.Ue();
    }
    const uint32_t luma = r.Ue();
    const uint32_t chroma_depth = r.Ue();
    if (luma > 8 || chroma_depth > 8) {
        return false;
    }
    out.bit_depth_luma_minus8 = static_cast<uint8_t>(luma);
    out.bit_depth_chroma_minus8 = static_cast<uint8_t>(chroma_depth);
    return !r.Overrun();
}

// ---------------------------------------------------------------------------
// Errors and file I/O
// ---------------------------------------------------------------------------

[[nodiscard]] std::wstring Win32Message(DWORD code)
{
    wchar_t* text = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
            | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::wstring message = L"error " + std::to_wstring(code);
    if (length > 0 && text) {
        std::wstring_view trimmed = Trim(std::wstring_view(text, length));
        message += L" (";
        message += trimmed;
        message += L")";
    }
    LocalFree(text);
    return message;
}

[[nodiscard]] bool WriteAll(HANDLE file, const uint8_t* data, size_t size, DWORD& failure)
{
    while (size > 0) {
        // WriteFile takes a DWORD; chunk so a fragment past 4 GB cannot wrap.
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(size, 1u << 30));
        DWORD written = 0;
        if (!WriteFile(file, data, chunk, &written, nullptr)) {
            failure = GetLastError();
            return false;
        }
        if (written == 0) {
            failure = ERROR_WRITE_FAULT;
            return false;
        }
        data += written;
        size -= written;
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Mp4Writer
// ---------------------------------------------------------------------------

struct Mp4Writer::Impl {
    struct Fragment {
        uint64_t decode_time = 0;
        uint64_t moof_offset = 0;
    };

    struct AudioTrack {
        AudioTrackConfig config;
        std::vector<uint8_t> payload; // frames waiting for the next fragment
        std::vector<uint32_t> sizes;
        uint64_t decode_time = 0; // samples of every frame already on disk
        size_t data_offset_at = 0; // trun data_offset in the moof being built
    };

    // Everything one open file owns. It sits apart from the mutex so Open can
    // reset it with a single assignment; a std::mutex can be neither copied
    // nor moved.
    struct State {
        Handle file;
        std::wstring path;
        VideoFormat format;
        bool open = false;
        bool failed = false;
        std::wstring failure;   // the first write error, reported again by every later call

        uint32_t sample_duration = 0;
        uint64_t mehd_value_offset = 0;
        uint64_t file_size = 0;
        uint64_t decode_time = 0; // ticks of every sample already on disk
        uint32_t sequence = 0;
        uint64_t sample_count = 0;

        // The fragment being accumulated. Only its first sample is a keyframe,
        // since a keyframe is what closes the previous fragment.
        std::vector<uint8_t> payload;
        std::vector<uint32_t> sizes;
        Bytes staging;

        std::vector<AudioTrack> audio;
        std::vector<Fragment> fragments;

        [[nodiscard]] std::wstring Failure(const wchar_t* what, DWORD code) const
        {
            return std::wstring(what) + L" failed on " + path + L": " + Win32Message(code);
        }

        bool Write(const uint8_t* data, size_t size, std::wstring& error)
        {
            DWORD code = 0;
            if (!WriteAll(file.Get(), data, size, code)) {
                failed = true;
                error = Failure(L"WriteFile", code);
                failure = error;
                return false;
            }
            file_size += size;
            return true;
        }

        /// Writes every pending audio frame, and the pending video samples too
        /// unless `with_video` is false. Only a keyframe may start a video
        /// fragment, so the audio-only flush has to leave the GOP in progress
        /// where it is.
        bool FlushFragment(bool with_video, std::wstring& error);

        /// Longest track, in movie timescale units, for mehd.
        [[nodiscard]] uint64_t DurationMs() const;
    };

    std::mutex mutex;
    State state;
};

namespace {

void WriteHvcC(Bytes& b, const SpsInfo& sps,
               const std::vector<std::vector<uint8_t>> (&arrays)[3])
{
    const size_t box = b.Begin("hvcC");
    b.U8(1); // configurationVersion
    b.U8((static_cast<uint32_t>(sps.profile_space) << 6)
         | (static_cast<uint32_t>(sps.tier) << 5) | sps.profile_idc);
    b.U32(sps.compatibility);
    b.U16(static_cast<uint32_t>(sps.constraints >> 32));
    b.U32(static_cast<uint32_t>(sps.constraints));
    b.U8(sps.level_idc);
    b.U16(0xF000); // reserved + min_spatial_segmentation_idc 0
    b.U8(0xFC);    // reserved + parallelismType 0
    b.U8(0xFCu | sps.chroma_format_idc);
    b.U8(0xF8u | sps.bit_depth_luma_minus8);
    b.U8(0xF8u | sps.bit_depth_chroma_minus8);
    b.U16(0); // avgFrameRate
    b.U8((static_cast<uint32_t>(sps.max_sub_layers_minus1 + 1) << 3)
         | (sps.temporal_id_nesting ? 0x04u : 0u) | 3u); // lengthSizeMinusOne 3

    const uint8_t types[3] = {kNalVps, kNalSps, kNalPps};
    b.U8(3);
    for (int a = 0; a < 3; ++a) {
        b.U8(0x80u | types[a]); // array_completeness 1
        b.U16(static_cast<uint32_t>(arrays[a].size()));
        for (const auto& nal : arrays[a]) {
            b.U16(static_cast<uint32_t>(nal.size()));
            b.Append(nal.data(), nal.size());
        }
    }
    b.End(box);
}

[[nodiscard]] uint32_t Chroma(double v) { return static_cast<uint32_t>(std::lround(v * 50000.0)); }

void WriteSampleEntry(Bytes& b, const VideoFormat& format, const SpsInfo& sps,
                      const std::vector<std::vector<uint8_t>> (&arrays)[3])
{
    const size_t entry = b.Begin("hvc1");
    b.Zeros(6);  // reserved
    b.U16(1);    // data_reference_index
    b.U16(0);    // pre_defined
    b.U16(0);    // reserved
    b.Zeros(12); // pre_defined
    b.U16(format.width);
    b.U16(format.height);
    b.U32(0x00480000); // 72 dpi
    b.U32(0x00480000);
    b.U32(0);  // reserved
    b.U16(1);  // frame_count
    char compressor[32] = {};
    compressor[0] = 3;
    std::memcpy(compressor + 1, "ZCR", 3);
    b.Append(compressor, sizeof(compressor));
    b.U16(0x0018); // depth
    b.U16(0xFFFF); // pre_defined -1

    WriteHvcC(b, sps, arrays);

    const size_t colr = b.Begin("colr");
    b.Tag("nclx");
    b.U16(format.ColourPrimaries());
    b.U16(format.TransferCharacteristics());
    b.U16(format.MatrixCoefficients());
    b.U8(format.FullRange() ? 0x80 : 0x00);
    b.End(colr);

    const size_t pasp = b.Begin("pasp");
    b.U32(1);
    b.U32(1);
    b.End(pasp);

    if (format.hdr) {
        // Same layout and units as ffmpeg's mov_write_mdcv_tag: primaries in
        // G, B, R order in 0.00002 steps, luminance in 0.0001 cd/m2 steps.
        const HdrMetadata& m = format.hdr_meta;
        const size_t mdcv = b.Begin("mdcv");
        b.U16(Chroma(m.green_x));
        b.U16(Chroma(m.green_y));
        b.U16(Chroma(m.blue_x));
        b.U16(Chroma(m.blue_y));
        b.U16(Chroma(m.red_x));
        b.U16(Chroma(m.red_y));
        b.U16(Chroma(m.white_x));
        b.U16(Chroma(m.white_y));
        b.U32(static_cast<uint32_t>(std::llround(m.max_mastering_nits * 10000.0)));
        b.U32(static_cast<uint32_t>(std::llround(m.min_mastering_nits * 10000.0)));
        b.End(mdcv);

        const size_t clli = b.Begin("clli");
        b.U16(m.max_cll);
        b.U16(m.max_fall);
        b.End(clli);
    }
    b.End(entry);
}

void WriteLanguageUnd(Bytes& b)
{
    // 'und' packed as three 5-bit letters offset from 0x60.
    b.U16((('u' - 0x60) << 10) | (('n' - 0x60) << 5) | ('d' - 0x60));
}

void WriteDataInformation(Bytes& b)
{
    const size_t dinf = b.Begin("dinf");
    const size_t dref = b.BeginFull("dref", 0, 0);
    b.U32(1);
    const size_t url = b.BeginFull("url ", 0, 1); // media is in this file
    b.End(url);
    b.End(dref);
    b.End(dinf);
}

/// stts, stsc, stsz and stco with no entries: every sample lives in a fragment.
void WriteEmptySampleTables(Bytes& b)
{
    const size_t stts = b.BeginFull("stts", 0, 0);
    b.U32(0);
    b.End(stts);
    const size_t stsc = b.BeginFull("stsc", 0, 0);
    b.U32(0);
    b.End(stsc);
    const size_t stsz = b.BeginFull("stsz", 0, 0);
    b.U32(0);
    b.U32(0);
    b.End(stsz);
    const size_t stco = b.BeginFull("stco", 0, 0);
    b.U32(0);
    b.End(stco);
}

void WriteTrex(Bytes& b, uint32_t track_id)
{
    const size_t trex = b.BeginFull("trex", 0, 0);
    b.U32(track_id);
    b.U32(1); // default_sample_description_index
    b.U32(0);
    b.U32(0);
    b.U32(0);
    b.End(trex);
}

/// ISO/IEC 14496-14 esds around a DecoderConfigDescriptor for AAC.
void WriteEsds(Bytes& b, const AudioTrackConfig& config)
{
    const size_t esds = b.BeginFull("esds", 0, 0);
    const size_t es = BeginDescriptor(b, 0x03);
    b.U16(0); // ES_ID: 14496-14 says 0 in the file, the track id identifies it
    b.U8(0);  // no stream dependence, URL or OCR stream

    const size_t dcd = BeginDescriptor(b, 0x04);
    b.U8(0x40); // objectTypeIndication: MPEG-4 Audio
    b.U8(0x15); // streamType 5 (audio) << 2, upStream 0, reserved 1
    // 6144 bits per channel is the most one raw_data_block may hold, so this
    // bounds every frame without having seen any.
    b.U24(768 * config.channels);
    b.U32(config.avg_bitrate); // maxBitrate
    b.U32(config.avg_bitrate);

    const size_t dsi = BeginDescriptor(b, 0x05);
    b.Append(config.specific_config.data(), config.specific_config.size());
    EndDescriptor(b, dsi);
    EndDescriptor(b, dcd);

    const size_t sl = BeginDescriptor(b, 0x06);
    b.U8(2); // predefined: reserved for MP4 files
    EndDescriptor(b, sl);

    EndDescriptor(b, es);
    b.End(esds);
}

void WriteAudioTrak(Bytes& b, const AudioTrackConfig& config, uint32_t track_id)
{
    const size_t trak = b.Begin("trak");
    const size_t tkhd = b.BeginFull("tkhd", 0, 0x3); // enabled | in_movie
    b.U32(0);
    b.U32(0);
    b.U32(track_id);
    b.U32(0); // reserved
    b.U32(0); // duration
    b.Zeros(8);
    b.U16(0); // layer
    // One shared group: a player picks a single audio track to play, while an
    // editor still sees desktop and microphone as separate tracks.
    b.U16(1);
    b.U16(0x0100); // volume 1.0
    b.U16(0);
    WriteUnityMatrix(b);
    b.U32(0); // width
    b.U32(0); // height
    b.End(tkhd);

    const size_t mdia = b.Begin("mdia");
    const size_t mdhd = b.BeginFull("mdhd", 0, 0);
    b.U32(0);
    b.U32(0);
    b.U32(config.sample_rate);
    b.U32(0);
    WriteLanguageUnd(b);
    b.U16(0);
    b.End(mdhd);

    const size_t hdlr = b.BeginFull("hdlr", 0, 0);
    b.U32(0);
    b.Tag("soun");
    b.Zeros(12);
    const std::string name = ToUtf8(config.name);
    b.Append(name.c_str(), name.size() + 1);
    b.End(hdlr);

    const size_t minf = b.Begin("minf");
    const size_t smhd = b.BeginFull("smhd", 0, 0);
    b.U16(0); // balance
    b.U16(0); // reserved
    b.End(smhd);

    WriteDataInformation(b);

    const size_t stbl = b.Begin("stbl");
    const size_t stsd = b.BeginFull("stsd", 0, 0);
    b.U32(1);
    const size_t entry = b.Begin("mp4a");
    b.Zeros(6); // reserved
    b.U16(1);   // data_reference_index
    b.Zeros(8); // reserved
    b.U16(config.channels);
    b.U16(16); // samplesize
    b.U16(0);  // pre_defined
    b.U16(0);  // reserved
    b.U32(config.sample_rate << 16);
    WriteEsds(b, config);
    b.End(entry);
    b.End(stsd);
    WriteEmptySampleTables(b);
    b.End(stbl);

    b.End(minf);
    b.End(mdia);
    b.End(trak);
}

/// ftyp + moov. Returns the offset of mehd's fragment_duration within `b`, so
/// Close can patch it once the length is known.
[[nodiscard]] size_t WriteHeader(Bytes& b, const VideoFormat& format, const SpsInfo& sps,
                                 const std::vector<std::vector<uint8_t>> (&arrays)[3],
                                 const std::vector<AudioTrackConfig>& audio)
{
    const size_t ftyp = b.Begin("ftyp");
    b.Tag("isom");
    b.U32(0x200);
    b.Tag("isom");
    b.Tag("iso6");
    b.Tag("mp41");
    b.End(ftyp);

    const size_t moov = b.Begin("moov");

    const size_t mvhd = b.BeginFull("mvhd", 0, 0);
    b.U32(0); // creation_time
    b.U32(0); // modification_time
    b.U32(kMovieTimescale);
    b.U32(0); // duration: fragmented, mehd carries it
    b.U32(0x00010000); // rate 1.0
    b.U16(0x0100);     // volume 1.0
    b.Zeros(10);
    WriteUnityMatrix(b);
    b.Zeros(24); // pre_defined
    b.U32(kFirstAudioTrackId + static_cast<uint32_t>(audio.size())); // next_track_ID
    b.End(mvhd);

    const size_t trak = b.Begin("trak");
    const size_t tkhd = b.BeginFull("tkhd", 0, 0x3); // enabled | in_movie
    b.U32(0);
    b.U32(0);
    b.U32(kTrackId);
    b.U32(0); // reserved
    b.U32(0); // duration
    b.Zeros(8);
    b.U16(0); // layer
    b.U16(0); // alternate_group
    b.U16(0); // volume: video
    b.U16(0);
    WriteUnityMatrix(b);
    b.U32(format.width << 16);
    b.U32(format.height << 16);
    b.End(tkhd);

    const size_t mdia = b.Begin("mdia");
    const size_t mdhd = b.BeginFull("mdhd", 0, 0);
    b.U32(0);
    b.U32(0);
    b.U32(kTrackTimescale);
    b.U32(0);
    WriteLanguageUnd(b);
    b.U16(0);
    b.End(mdhd);

    const size_t hdlr = b.BeginFull("hdlr", 0, 0);
    b.U32(0);
    b.Tag("vide");
    b.Zeros(12);
    b.Append("VideoHandler", 13);
    b.End(hdlr);

    const size_t minf = b.Begin("minf");
    const size_t vmhd = b.BeginFull("vmhd", 0, 1);
    b.Zeros(8); // graphicsmode + opcolor
    b.End(vmhd);

    WriteDataInformation(b);

    const size_t stbl = b.Begin("stbl");
    const size_t stsd = b.BeginFull("stsd", 0, 0);
    b.U32(1);
    WriteSampleEntry(b, format, sps, arrays);
    b.End(stsd);
    WriteEmptySampleTables(b);
    b.End(stbl);

    b.End(minf);
    b.End(mdia);
    b.End(trak);

    for (size_t i = 0; i < audio.size(); ++i) {
        WriteAudioTrak(b, audio[i], kFirstAudioTrackId + static_cast<uint32_t>(i));
    }

    const size_t mvex = b.Begin("mvex");
    const size_t mehd = b.BeginFull("mehd", 1, 0);
    const size_t mehd_value = b.Size();
    b.U64(0);
    b.End(mehd);
    WriteTrex(b, kTrackId);
    for (size_t i = 0; i < audio.size(); ++i) {
        WriteTrex(b, kFirstAudioTrackId + static_cast<uint32_t>(i));
    }
    b.End(mvex);

    b.End(moov);
    return mehd_value;
}

} // namespace

bool Mp4Writer::Impl::State::FlushFragment(bool with_video, std::wstring& error)
{
    const bool video = with_video && !sizes.empty();
    size_t audio_bytes = 0;
    bool any_audio = false;
    for (const AudioTrack& track : audio) {
        if (!track.sizes.empty()) {
            any_audio = true;
            audio_bytes += track.payload.size();
        }
    }
    if (!video && !any_audio) {
        return true;
    }

    const uint64_t moof_offset = file_size;
    const uint64_t base_time = decode_time;

    staging.Clear();
    Bytes& b = staging;
    const size_t moof = b.Begin("moof");
    const size_t mfhd = b.BeginFull("mfhd", 0, 0);
    b.U32(++sequence);
    b.End(mfhd);

    size_t video_offset_at = 0;
    if (video) {
        const size_t traf = b.Begin("traf");
        const size_t tfhd =
            b.BeginFull("tfhd", 0, kTfhdDefaultBaseIsMoof | kTfhdDefaultSampleDuration);
        b.U32(kTrackId);
        b.U32(sample_duration);
        b.End(tfhd);

        const size_t tfdt = b.BeginFull("tfdt", 1, 0);
        b.U64(base_time);
        b.End(tfdt);

        const size_t trun =
            b.BeginFull("trun", 0, kTrunDataOffset | kTrunSampleSize | kTrunSampleFlags);
        b.U32(static_cast<uint32_t>(sizes.size()));
        video_offset_at = b.Size();
        b.U32(0);
        for (size_t i = 0; i < sizes.size(); ++i) {
            b.U32(sizes[i]);
            b.U32(i == 0 ? kSyncSampleFlags : kNonSyncSampleFlags);
        }
        b.End(trun);
        b.End(traf);
    }

    for (size_t t = 0; t < audio.size(); ++t) {
        AudioTrack& track = audio[t];
        if (track.sizes.empty()) {
            continue;
        }
        const size_t traf = b.Begin("traf");
        // Every AAC frame is a sync sample of the same length, so both go in
        // tfhd and the trun only carries sizes.
        const size_t tfhd = b.BeginFull(
            "tfhd", 0,
            kTfhdDefaultBaseIsMoof | kTfhdDefaultSampleDuration | kTfhdDefaultSampleFlags);
        b.U32(kFirstAudioTrackId + static_cast<uint32_t>(t));
        b.U32(kAacFrameSamples);
        b.U32(kSyncSampleFlags);
        b.End(tfhd);

        const size_t tfdt = b.BeginFull("tfdt", 1, 0);
        b.U64(track.decode_time);
        b.End(tfdt);

        const size_t trun = b.BeginFull("trun", 0, kTrunDataOffset | kTrunSampleSize);
        b.U32(static_cast<uint32_t>(track.sizes.size()));
        track.data_offset_at = b.Size();
        b.U32(0);
        for (uint32_t size : track.sizes) {
            b.U32(size);
        }
        b.End(trun);
        b.End(traf);
    }
    b.End(moof);

    const size_t moof_size = b.Size();
    const uint64_t payload_size = audio_bytes + (video ? payload.size() : 0);
    const bool large = payload_size > UINT32_MAX - 8;
    const size_t mdat_header = large ? 16 : 8;
    if (large) {
        b.U32(1);
        b.Tag("mdat");
        b.U64(payload_size + 16);
    } else {
        b.U32(static_cast<uint32_t>(payload_size + 8));
        b.Tag("mdat");
    }

    // Audio goes ahead of video in the mdat. trun's data_offset is a signed
    // 32-bit field, and audio behind a GOP over 2 GB would be out of its
    // reach; a few hundred kilobytes of audio in front keeps every offset
    // small whatever the video bitrate.
    //
    // One WriteFile per fragment, so a crash can only ever leave a torn tail,
    // never a moof on disk whose mdat is missing from the middle of the file.
    // That means copying the payload behind the moof; at recording bitrates it
    // is a few megabytes every two seconds.
    size_t data_offset = moof_size + mdat_header;
    for (AudioTrack& track : audio) {
        if (track.sizes.empty()) {
            continue;
        }
        b.PatchU32(track.data_offset_at, static_cast<uint32_t>(data_offset));
        b.Append(track.payload.data(), track.payload.size());
        data_offset += track.payload.size();
    }
    if (video) {
        b.PatchU32(video_offset_at, static_cast<uint32_t>(data_offset));
        b.Append(payload.data(), payload.size());
    }
    if (!Write(b.Data(), b.Size(), error)) {
        return false;
    }

    for (AudioTrack& track : audio) {
        track.decode_time += static_cast<uint64_t>(kAacFrameSamples) * track.sizes.size();
        track.payload.clear();
        track.sizes.clear();
    }
    if (video) {
        // tfra indexes video only, and its entries say the video traf is the
        // first in the moof; audio-only fragments are not seek points.
        fragments.push_back({base_time, moof_offset});
        decode_time += static_cast<uint64_t>(sample_duration) * sizes.size();
        payload.clear();
        sizes.clear();
    }
    return true;
}

uint64_t Mp4Writer::Impl::State::DurationMs() const
{
    uint64_t longest =
        (decode_time * kMovieTimescale + kTrackTimescale / 2) / kTrackTimescale;
    for (const AudioTrack& track : audio) {
        const uint64_t rate = track.config.sample_rate;
        longest = std::max(longest, (track.decode_time * kMovieTimescale + rate / 2) / rate);
    }
    return longest;
}

Mp4Writer::Mp4Writer() : impl_(std::make_unique<Impl>()) {}

Mp4Writer::~Mp4Writer()
{
    std::wstring ignored;
    Close(ignored);
}

bool Mp4Writer::Open(const std::wstring& path, const VideoFormat& format,
                     const std::vector<uint8_t>& parameter_sets, std::wstring& error)
{
    return Open(path, format, parameter_sets, {}, error);
}

bool Mp4Writer::Open(const std::wstring& path, const VideoFormat& format,
                     const std::vector<uint8_t>& parameter_sets,
                     const std::vector<AudioTrackConfig>& audio, std::wstring& error)
{
    std::lock_guard lock(impl_->mutex);
    Impl::State& s = impl_->state;
    if (s.open) {
        error = L"MP4 writer is already open on " + s.path;
        return false;
    }
    if (format.width == 0 || format.height == 0 || format.width > 0xFFFF
        || format.height > 0xFFFF) {
        error = L"MP4 writer: invalid frame size " + std::to_wstring(format.width) + L"x"
            + std::to_wstring(format.height);
        return false;
    }
    if (format.fps == 0 || kTrackTimescale % format.fps != 0) {
        error = L"MP4 writer: frame rate " + std::to_wstring(format.fps)
            + L" does not divide the 90 kHz track timescale";
        return false;
    }

    std::vector<std::vector<uint8_t>> arrays[3];
    ForEachNal(parameter_sets.data(), parameter_sets.size(),
               [&](const uint8_t* nal, size_t size) {
                   const uint8_t type = NalType(nal);
                   if (type >= kNalVps && type <= kNalPps) {
                       arrays[type - kNalVps].emplace_back(nal, nal + size);
                   }
               });
    if (arrays[0].empty() || arrays[1].empty() || arrays[2].empty()) {
        error = L"MP4 writer: the sequence header is missing a VPS, SPS or PPS";
        return false;
    }

    SpsInfo sps;
    if (!ParseSps(arrays[1][0].data(), arrays[1][0].size(), sps)) {
        error = L"MP4 writer: the SPS could not be parsed";
        return false;
    }

    for (size_t i = 0; i < audio.size(); ++i) {
        const AudioTrackConfig& a = audio[i];
        const std::wstring which = L"MP4 writer: audio track " + std::to_wstring(i);
        // mp4a carries the rate as 16.16 fixed point.
        if (a.sample_rate == 0 || a.sample_rate > 0xFFFF) {
            error = which + L" has sample rate " + std::to_wstring(a.sample_rate)
                + L", which the mp4a sample entry cannot hold";
            return false;
        }
        if (a.channels == 0 || a.channels > 8) {
            error = which + L" has " + std::to_wstring(a.channels) + L" channels";
            return false;
        }
        if (a.specific_config.empty()) {
            error = which + L" has no AudioSpecificConfig";
            return false;
        }
    }

    Bytes header;
    const size_t mehd_value = WriteHeader(header, format, sps, arrays, audio);

    Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!file) {
        const DWORD code = GetLastError();
        error = L"CreateFileW failed on " + path + L": " + Win32Message(code);
        return false;
    }

    s = Impl::State{};
    s.file = std::move(file);
    s.path = path;
    s.format = format;
    s.sample_duration = kTrackTimescale / format.fps;
    s.mehd_value_offset = mehd_value;
    s.audio.resize(audio.size());
    for (size_t i = 0; i < audio.size(); ++i) {
        s.audio[i].config = audio[i];
    }
    s.open = true;

    if (!s.Write(header.Data(), header.Size(), error)) {
        s.file.Reset();
        s.open = false;
        DeleteFileW(path.c_str());
        return false;
    }
    return true;
}

bool Mp4Writer::WriteAudioFrame(size_t track, const uint8_t* data, size_t size,
                                std::wstring& error)
{
    std::lock_guard lock(impl_->mutex);
    Impl::State& s = impl_->state;
    if (!s.open) {
        error = L"MP4 writer is not open";
        return false;
    }
    if (s.failed) {
        error = s.failure;
        return false;
    }
    if (track >= s.audio.size()) {
        error = L"MP4 writer: there is no audio track " + std::to_wstring(track);
        return false;
    }
    if (size == 0 || size > UINT32_MAX) {
        error = L"MP4 writer: audio frame of " + std::to_wstring(size) + L" bytes";
        return false;
    }

    Impl::AudioTrack& t = s.audio[track];
    t.payload.insert(t.payload.end(), data, data + size);
    t.sizes.push_back(static_cast<uint32_t>(size));

    if (t.sizes.size() > kMaxPendingAudioFrames) {
        return s.FlushFragment(false, error);
    }
    return true;
}

bool Mp4Writer::WriteSample(const uint8_t* annexb, size_t size, bool keyframe,
                            std::wstring& error)
{
    std::lock_guard lock(impl_->mutex);
    Impl::State& s = impl_->state;
    if (!s.open) {
        error = L"MP4 writer is not open";
        return false;
    }
    if (s.failed) {
        error = s.failure;
        return false;
    }
    if (s.sample_count == 0 && !keyframe) {
        error = L"MP4 writer: the first sample must be a keyframe";
        return false;
    }

    // With no video pending there is nothing for the keyframe to close. Audio
    // that arrived before it waits, and goes out with this GOP rather than in
    // a fragment of its own.
    if (keyframe && !s.sizes.empty() && !s.FlushFragment(true, error)) {
        return false;
    }

    const size_t payload_before = s.payload.size();
    ForEachNal(annexb, size, [&](const uint8_t* nal, size_t length) {
        const uint8_t type = NalType(nal);
        if ((type >= kNalVps && type <= kNalPps) || type == kNalAud) {
            return;
        }
        const uint32_t n = static_cast<uint32_t>(length);
        const uint8_t prefix[4] = {static_cast<uint8_t>(n >> 24), static_cast<uint8_t>(n >> 16),
                                   static_cast<uint8_t>(n >> 8), static_cast<uint8_t>(n)};
        s.payload.insert(s.payload.end(), prefix, prefix + 4);
        s.payload.insert(s.payload.end(), nal, nal + length);
    });

    const size_t sample_size = s.payload.size() - payload_before;
    if (sample_size == 0) {
        error = L"MP4 writer: access unit contains no slice data";
        return false;
    }
    s.sizes.push_back(static_cast<uint32_t>(sample_size));
    ++s.sample_count;
    return true;
}

bool Mp4Writer::Close(std::wstring& error)
{
    std::lock_guard lock(impl_->mutex);
    Impl::State& s = impl_->state;
    if (!s.open) {
        return true;
    }

    bool ok = !s.failed;
    if (!ok) {
        error = s.failure + L"; the file ends at the last complete fragment";
    }

    if (ok) {
        // Audio that arrived after the last keyframe goes out here too, in an
        // audio-only fragment when no video is pending.
        ok = s.FlushFragment(true, error);
    }

    if (ok) {
        Bytes b;
        const size_t mfra = b.Begin("mfra");
        const size_t tfra = b.BeginFull("tfra", 1, 0);
        b.U32(kTrackId);
        b.U32(0); // traf, trun and sample numbers are one byte each
        b.U32(static_cast<uint32_t>(s.fragments.size()));
        for (const auto& f : s.fragments) {
            b.U64(f.decode_time);
            b.U64(f.moof_offset);
            b.U8(1); // traf_number
            b.U8(1); // trun_number
            b.U8(1); // sample_number: every fragment opens on its keyframe
        }
        b.End(tfra);
        const size_t mfro = b.BeginFull("mfro", 0, 0);
        const size_t mfro_size = b.Size();
        b.U32(0);
        b.End(mfro);
        b.End(mfra);
        b.PatchU32(mfro_size, static_cast<uint32_t>(b.Size()));
        ok = s.Write(b.Data(), b.Size(), error);
    }

    if (ok) {
        // mehd is patched last: until this point the file is a valid
        // fragmented MP4 with an unknown duration, which players handle.
        Bytes value;
        value.U64(s.DurationMs());
        LARGE_INTEGER at{};
        at.QuadPart = static_cast<LONGLONG>(s.mehd_value_offset);
        DWORD code = 0;
        if (!SetFilePointerEx(s.file.Get(), at, nullptr, FILE_BEGIN)) {
            error = s.Failure(L"SetFilePointerEx", GetLastError());
            ok = false;
        } else if (!WriteAll(s.file.Get(), value.Data(), value.Size(), code)) {
            error = s.Failure(L"WriteFile", code);
            ok = false;
        }
    }

    s.file.Reset();
    s.open = false;
    s.payload = {};
    s.sizes = {};
    for (Impl::AudioTrack& track : s.audio) {
        track.payload = {};
        track.sizes = {};
    }
    return ok;
}

uint64_t Mp4Writer::SampleCount() const
{
    std::lock_guard lock(impl_->mutex);
    return impl_->state.sample_count;
}

uint64_t Mp4Writer::BytesWritten() const
{
    std::lock_guard lock(impl_->mutex);
    return impl_->state.file_size;
}

} // namespace zcr
