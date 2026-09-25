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

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "video_format.h"

namespace zcr {

/// Fragmented MP4 writer for one HEVC video track.
///
/// Why fragmented: every fragment is self-describing, so a crash, a kill or a
/// power cut costs at most the fragment in progress (one GOP, 2 s) and the file
/// still plays. A plain MP4 with the moov at the end is unplayable until the
/// very last write succeeds.
///
/// Layout:
///   ftyp (major isom, minor 0x200; compatible isom iso6 mp41)
///   moov
///     mvhd (timescale 1000, duration 0)
///     trak/tkhd, mdia/mdhd (timescale 90000), hdlr vide, minf/vmhd, dinf,
///       stbl/stsd/hvc1 { hvcC, colr nclx, pasp 1:1, and when hdr: mdcv, clli }
///       with empty stts/stsc/stsz/stco
///     mvex/mehd (64-bit fragment_duration, patched on Close), trex
///   moof/mdat pairs, one per GOP: a new fragment starts at every keyframe.
///     moof: mfhd, traf { tfhd (default-base-is-moof), tfdt (v1), trun (data
///     offset, per-sample size, per-sample flags; duration from tfhd default) }
///   mfra (tfra with one entry per fragment, mfro) appended on Close
///
/// Samples are converted from Annex B to 4-byte length-prefixed NAL units.
/// VPS/SPS/PPS NAL units (types 32/33/34) are stripped from samples because
/// the sample entry is 'hvc1', which puts all parameter sets in hvcC only.
/// SEI and slice NAL units are kept. AUD (35) is dropped.
///
/// hvcC fields are parsed from the SPS (profile_tier_level, chroma_format_idc,
/// bit depths), with emulation-prevention bytes removed before parsing.
///
/// Every sample has the same duration: 90000 / fps ticks (3000, 1500, 750).
///
/// Not thread-safe: one writer thread.
class Mp4Writer {
public:
    Mp4Writer();
    ~Mp4Writer();   // closes (finalizes) if still open
    Mp4Writer(const Mp4Writer&) = delete;
    Mp4Writer& operator=(const Mp4Writer&) = delete;

    /// Creates `path` (fails if it exists) and writes ftyp + moov.
    /// `parameter_sets` is Annex B VPS + SPS + PPS, as NvencEncoder::
    /// SequenceHeader returns it.
    bool Open(const std::wstring& path, const VideoFormat& format,
              const std::vector<uint8_t>& parameter_sets, std::wstring& error);

    /// Appends one access unit (Annex B). The first sample must be a keyframe.
    /// Buffers the current fragment in memory and writes it out (moof + mdat
    /// in one WriteFile) when the next keyframe arrives, so a fragment on disk
    /// is always complete.
    bool WriteSample(const uint8_t* annexb, size_t size, bool keyframe, std::wstring& error);

    /// Writes the pending fragment, patches mehd with the real duration,
    /// appends mfra, and closes the file. Idempotent.
    bool Close(std::wstring& error);

    [[nodiscard]] uint64_t SampleCount() const;
    [[nodiscard]] uint64_t BytesWritten() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace zcr
