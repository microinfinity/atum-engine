#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <queue>
#include <memory>
#include <optional>
#include "atum/surface/video_frame.hpp"

namespace atum {

/// Reference picture entry inside the Decoded Picture Buffer.
struct DpbEntry {
    VideoFrame frame;
    int32_t poc{0};
    uint32_t frame_num{0};
    bool is_long_term{false};
    bool is_keyframe{false};
};

/// Sovereign Decoded Picture Buffer (DPB) for reference picture management.
class DecodedPictureBuffer {
public:
    explicit DecodedPictureBuffer(size_t max_ref_frames = 16) noexcept
        : max_ref_frames_(max_ref_frames) {}

    /// Inserts a newly decoded frame into the reference buffer.
    void insert(VideoFrame frame, int32_t poc, uint32_t frame_num, bool is_keyframe = false);

    /// Finds a reference frame by POC.
    [[nodiscard]] const VideoFrame* find_by_poc(int32_t poc) const noexcept;

    /// Finds a reference frame by index (0 = most recent).
    [[nodiscard]] const VideoFrame* get_ref(size_t index) const noexcept;

    /// Clears all reference frames (e.g. upon IDR / keyframe).
    void clear() noexcept;

    [[nodiscard]] size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
    [[nodiscard]] const std::vector<DpbEntry>& entries() const noexcept { return entries_; }
    void set_max_ref_frames(size_t max_refs) noexcept { max_ref_frames_ = max_refs; }
    [[nodiscard]] size_t max_ref_frames() const noexcept { return max_ref_frames_; }

private:
    size_t max_ref_frames_{16};
    std::vector<DpbEntry> entries_;
};

/// Shared POC/PTS display-order reorder queue with zero latency on low-delay/Baseline streams.
class FrameReorderQueue {
public:
    void clear() noexcept {
        items_.clear();
        prev_poc_msb_ = 0;
        prev_poc_lsb_ = 0;
        gop_epoch_ = 0;
        decode_seq_ = 0;
        sps_reorder_depth_ = 0;
        observed_reorder_depth_ = 0;
    }

    [[nodiscard]] int64_t unwrap_poc(uint32_t poc_lsb, uint32_t max_poc_lsb, bool is_idr, bool is_ref) noexcept {
        if (is_idr) {
            prev_poc_msb_ = 0;
            prev_poc_lsb_ = 0;
            return static_cast<int64_t>(poc_lsb);
        }
        if (max_poc_lsb == 0) {
            return static_cast<int64_t>(poc_lsb);
        }
        int64_t poc_msb = prev_poc_msb_;
        if (poc_lsb < prev_poc_lsb_ && (prev_poc_lsb_ - poc_lsb) >= (max_poc_lsb / 2)) {
            poc_msb += static_cast<int64_t>(max_poc_lsb);
        } else if (poc_lsb > prev_poc_lsb_ && (poc_lsb - prev_poc_lsb_) > (max_poc_lsb / 2)) {
            poc_msb -= static_cast<int64_t>(max_poc_lsb);
        }
        if (is_ref) {
            prev_poc_msb_ = poc_msb;
            prev_poc_lsb_ = poc_lsb;
        }
        return poc_msb + static_cast<int64_t>(poc_lsb);
    }

    void set_sps_reorder_depth(size_t depth) noexcept {
        sps_reorder_depth_ = std::max(sps_reorder_depth_, depth);
    }

    void note_b_frame_or_dts_lag() noexcept {
        if (sps_reorder_depth_ == 0 && observed_reorder_depth_ < 6) {
            observed_reorder_depth_ = 6;
        }
    }

    [[nodiscard]] size_t effective_depth() const noexcept {
        return sps_reorder_depth_ > 0 ? sps_reorder_depth_ : observed_reorder_depth_;
    }

    void push_and_emit(VideoFrame frame, int64_t poc, bool is_idr, std::queue<VideoFrame>& out_ready) {
        if (is_idr) {
            drain_to(out_ready);
            ++gop_epoch_;
            // IDR picture has POC=0 within the new closed GOP: emit immediately for zero-latency single-frame decode.
            out_ready.push(std::move(frame));
            return;
        }

        const size_t depth = effective_depth();
        if (depth == 0 && items_.empty()) {
            out_ready.push(std::move(frame));
            return;
        }

        ReorderItem item;
        item.gop_epoch = gop_epoch_;
        item.poc = poc;
        item.pts = frame.pts();
        item.decode_seq = decode_seq_++;
        item.frame = std::move(frame);
        items_.push_back(std::move(item));

        while (items_.size() > depth) {
            emit_min(out_ready);
        }
    }

    void drain_to(std::queue<VideoFrame>& out_ready) {
        while (!items_.empty()) {
            emit_min(out_ready);
        }
    }

    [[nodiscard]] bool contains_surface(uint32_t surface_id) const noexcept {
        if (surface_id == 0xFFFFFFFF) return false;
        for (const auto& item : items_) {
            if (item.frame.hw_surface_id() == surface_id) return true;
        }
        return false;
    }

private:
    struct ReorderItem {
        uint64_t gop_epoch{0};
        int64_t poc{0};
        int64_t pts{0};
        uint64_t decode_seq{0};
        VideoFrame frame;
    };

    static bool comes_before(const ReorderItem& a, const ReorderItem& b) noexcept {
        if (a.gop_epoch != b.gop_epoch) return a.gop_epoch < b.gop_epoch;
        if (a.poc != b.poc) return a.poc < b.poc;
        if (a.pts != b.pts) return a.pts < b.pts;
        return a.decode_seq < b.decode_seq;
    }

    void emit_min(std::queue<VideoFrame>& out_ready) {
        auto best = items_.begin();
        for (auto it = std::next(items_.begin()); it != items_.end(); ++it) {
            if (comes_before(*it, *best)) best = it;
        }
        out_ready.push(std::move(best->frame));
        items_.erase(best);
    }

    std::vector<ReorderItem> items_;
    int64_t prev_poc_msb_{0};
    uint32_t prev_poc_lsb_{0};
    uint64_t gop_epoch_{0};
    uint64_t decode_seq_{0};
    size_t sps_reorder_depth_{0};
    size_t observed_reorder_depth_{0};
};

} // namespace atum
