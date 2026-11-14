#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <span>
#include <array>
#include <memory>
#include "atum/core/frame.hpp"
#include "atum/core/media_types.hpp"
#include "atum/surface/pixel_format.hpp"
#include "atum/color/hdr_metadata.hpp"

namespace atum {

/// Exported DRM PRIME DMA-BUF plane & modifier descriptor owned by HwFrameBacking.
struct HwFrameBufferLayout {
    uint32_t width{0};
    uint32_t height{0};
    uint32_t drm_fourcc{0};
    uint32_t num_planes{0};
    uint32_t num_objects{0};
    std::array<int32_t, 4> fds{-1, -1, -1, -1};
    std::array<uint32_t, 4> strides{};
    std::array<uint32_t, 4> offsets{};
    std::array<uint64_t, 4> modifiers{};
    std::array<uint32_t, 4> object_indices{};
    uint64_t modifier{0};

    [[nodiscard]] int32_t plane_fd(size_t plane_idx) const noexcept {
        if (plane_idx >= 4) return -1;
        uint32_t obj = object_indices[plane_idx];
        if (obj < 4 && fds[obj] >= 0) return fds[obj];
        return fds[plane_idx] >= 0 ? fds[plane_idx] : fds[0];
    }

    [[nodiscard]] uint64_t plane_modifier(size_t plane_idx) const noexcept {
        if (plane_idx >= 4) return modifier;
        return modifiers[plane_idx] != 0 ? modifiers[plane_idx] : modifier;
    }
};

/// Abstract lazy GPU-surface backing (defers vaSyncSurface / vaGetImage / vaExportSurfaceHandle until needed).
class HwFrameBacking {
public:
    virtual ~HwFrameBacking() = default;

    [[nodiscard]] virtual uint32_t surface_id() const noexcept = 0;

    /// Lazily synchronizes and downloads GPU surface pixels into CPU planes/strides once.
    virtual bool ensure_cpu_mapped(
        PixelFormat& inout_format,
        std::array<uint8_t*, 4>& out_planes,
        std::array<size_t, 4>& out_strides
    ) noexcept = 0;

    /// Lazily exports PRIME DMA-BUF FDs (owned by backing and closed in destructor).
    [[nodiscard]] virtual const HwFrameBufferLayout* export_dma_buf(bool sync_first = true) noexcept = 0;

    /// Returns already-exported primary DMA-BUF FD without triggering a new export syscall.
    [[nodiscard]] virtual int32_t cached_dma_buf_fd() const noexcept = 0;

    /// Materializes CPU pixels if needed and releases the underlying surface pool lease.
    virtual void detach_surface_to_cpu() noexcept = 0;
};

/// Decoded uncompressed multi-planar video frame.
class VideoFrame : public Frame {
public:
    static constexpr size_t MAX_PLANES = 4;

    VideoFrame() noexcept;
    ~VideoFrame() override = default;

    VideoFrame(const VideoFrame& other);
    VideoFrame& operator=(const VideoFrame& other);

    VideoFrame(VideoFrame&& other) noexcept;
    VideoFrame& operator=(VideoFrame&& other) noexcept;

    /// Allocates an owned video surface with 32-byte SIMD row alignment.
    static VideoFrame allocate(uint32_t width, uint32_t height, PixelFormat format);

    /// Wraps external hardware/memory planes without copying (borrowed zero-copy surface).
    static VideoFrame from_raw_planes(
        uint32_t width,
        uint32_t height,
        PixelFormat format,
        const std::array<uint8_t*, MAX_PLANES>& planes,
        const std::array<size_t, MAX_PLANES>& strides
    ) noexcept;

    /// Read-only external plane wrapper.
    static VideoFrame from_raw_planes_const(
        uint32_t width,
        uint32_t height,
        PixelFormat format,
        const std::array<const uint8_t*, MAX_PLANES>& planes,
        const std::array<size_t, MAX_PLANES>& strides
    ) noexcept;

    /// Creates a lazy GPU-backed VideoFrame without downloading pixels to CPU.
    static VideoFrame from_hw_backing(
        uint32_t width,
        uint32_t height,
        PixelFormat format,
        std::shared_ptr<HwFrameBacking> backing
    ) noexcept;

    [[nodiscard]] bool is_valid() const noexcept override {
        return width_ > 0 && height_ > 0 && format_ != PixelFormat::Unknown &&
               (planes_[0] != nullptr || hw_backing_ != nullptr);
    }

    [[nodiscard]] uint32_t width() const noexcept { return width_; }
    [[nodiscard]] uint32_t height() const noexcept { return height_; }
    [[nodiscard]] PixelFormat pixel_format() const noexcept { return format_; }
    [[nodiscard]] uint32_t bit_depth() const noexcept { return pixel_format_bit_depth(format_); }
    [[nodiscard]] size_t bytes_per_sample() const noexcept { return pixel_format_bytes_per_sample(format_); }
    [[nodiscard]] bool is_high_bit_depth() const noexcept { return pixel_format_is_high_bit_depth(format_); }

    [[nodiscard]] ColorRange color_range() const noexcept { return range_; }
    void set_color_range(ColorRange r) noexcept { range_ = r; }

    [[nodiscard]] ColorSpace color_space() const noexcept { return space_; }
    void set_color_space(ColorSpace s) noexcept { space_ = s; }

    [[nodiscard]] const HdrMetadata& hdr_metadata() const noexcept { return hdr_meta_; }
    void set_hdr_metadata(const HdrMetadata& meta) noexcept { hdr_meta_ = meta; }

    [[nodiscard]] size_t plane_count() const noexcept {
        return pixel_format_plane_count(format_);
    }

    [[nodiscard]] const uint8_t* plane(size_t index) const noexcept {
        if (index >= MAX_PLANES) return nullptr;
        if (!planes_[0] && hw_backing_) materialize_cpu_backing();
        return planes_[index];
    }

    [[nodiscard]] uint8_t* plane_mut(size_t index) noexcept {
        ensure_owned();
        return index < MAX_PLANES ? planes_[index] : nullptr;
    }

    /// Access typed 16-bit plane pointer for 10-bit and 12-bit formats.
    [[nodiscard]] const uint16_t* plane16(size_t index) const noexcept {
        return reinterpret_cast<const uint16_t*>(plane(index));
    }

    [[nodiscard]] uint16_t* plane16_mut(size_t index) noexcept {
        return reinterpret_cast<uint16_t*>(plane_mut(index));
    }

    [[nodiscard]] size_t stride(size_t index) const noexcept {
        if (index >= MAX_PLANES) return 0;
        if (strides_[index] == 0 && !planes_[0] && hw_backing_) materialize_cpu_backing();
        return strides_[index];
    }

    [[nodiscard]] uint32_t plane_width(size_t index) const noexcept;
    [[nodiscard]] uint32_t plane_height(size_t index) const noexcept;

    void ensure_owned();
    [[nodiscard]] VideoFrame clone() const;
    [[nodiscard]] VideoFrame shallow_hw_ref() const;

    /// Saves frame directly to an image file (.png, .jpg, .webp, .qoi, .bmp) using sovereign Nisaba engine.
    [[nodiscard]] MediaResult<void> save_image(const std::string& path, int quality_or_compression = 90) const;

    /// Saves frame directly to a PNG file using sovereign Nisaba engine.
    [[nodiscard]] MediaResult<void> save_png(const std::string& path, int compression_level = 6) const;

    // --- Hardware Acceleration & Zero-Copy Properties ---
    [[nodiscard]] bool is_hw_frame() const noexcept { return is_hw_frame_ || hw_backing_ != nullptr; }
    void set_hw_frame(bool hw) noexcept { is_hw_frame_ = hw; }

    [[nodiscard]] uint32_t hw_surface_id() const noexcept {
        return hw_backing_ ? hw_backing_->surface_id() : hw_surface_id_;
    }
    void set_hw_surface_id(uint32_t id) noexcept { hw_surface_id_ = id; if (id != 0xFFFFFFFF) is_hw_frame_ = true; }

    [[nodiscard]] int32_t dma_buf_fd() const noexcept {
        if (dma_buf_fd_ >= 0) return dma_buf_fd_;
        if (hw_backing_) {
            const HwFrameBufferLayout* layout = hw_backing_->export_dma_buf(true);
            return layout ? layout->plane_fd(0) : -1;
        }
        return -1;
    }
    void set_dma_buf_fd(int32_t fd) noexcept { dma_buf_fd_ = fd; if (fd >= 0) is_hw_frame_ = true; }

    [[nodiscard]] uint64_t hw_opaque_handle() const noexcept { return hw_opaque_handle_; }
    void set_hw_opaque_handle(uint64_t handle) noexcept { hw_opaque_handle_ = handle; if (handle != 0) is_hw_frame_ = true; }

    [[nodiscard]] const std::shared_ptr<HwFrameBacking>& hw_backing() const noexcept { return hw_backing_; }
    void set_hw_backing(std::shared_ptr<HwFrameBacking> backing) noexcept {
        hw_backing_ = std::move(backing);
        if (hw_backing_) {
            is_hw_frame_ = true;
            hw_surface_id_ = hw_backing_->surface_id();
        }
    }

private:
    void materialize_cpu_backing() const noexcept;

    uint32_t width_{0};
    uint32_t height_{0};
    mutable PixelFormat format_{PixelFormat::Unknown};
    ColorRange range_{ColorRange::Studio};
    ColorSpace space_{ColorSpace::BT709};
    HdrMetadata hdr_meta_{};

    mutable std::array<uint8_t*, MAX_PLANES> planes_{};
    mutable std::array<size_t, MAX_PLANES> strides_{};

    std::vector<uint8_t> owned_buffer_;
    bool is_owned_{false};

    bool is_hw_frame_{false};
    uint32_t hw_surface_id_{0xFFFFFFFF};
    int32_t dma_buf_fd_{-1};
    uint64_t hw_opaque_handle_{0};
    std::shared_ptr<HwFrameBacking> hw_backing_;
};

} // namespace atum
