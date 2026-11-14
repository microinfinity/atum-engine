#pragma once

#include <cstdint>
#include <cstddef>
#include <string_view>
#include <array>
#include <memory>
#include "atum/core/media_types.hpp"
#include "atum/surface/video_frame.hpp"

namespace atum {

/// Hardware acceleration backend interface.
enum class HwDeviceType : uint8_t {
    None = 0,
    DrmPrime,            ///< Linux DRM/KMS DMA-BUF / PRIME zero-copy scanout
    Vaapi,               ///< Video Acceleration API (Intel/AMD on Linux)
    V4l2M2m,             ///< Video4Linux2 Memory-to-Memory (Embedded / Raspberry Pi / NXP)
    Nvdec,               ///< NVIDIA Hardware Decoder
    AndroidMediaCodec,   ///< Android NDK AHardwareBuffer & MediaCodec
    Vulkan,              ///< Vulkan Graphics Interop & Vulkan Video (Cross-Platform)
    OpenGl,              ///< OpenGL / EGL Zero-Copy DMA-BUF Interop & Shader Presentation
};

constexpr std::string_view hw_device_type_to_string(HwDeviceType type) noexcept {
    switch (type) {
        case HwDeviceType::DrmPrime:          return "Linux DRM/KMS PRIME";
        case HwDeviceType::Vaapi:             return "VA-API";
        case HwDeviceType::V4l2M2m:           return "V4L2 M2M";
        case HwDeviceType::Nvdec:             return "NVDEC";
        case HwDeviceType::AndroidMediaCodec: return "Android MediaCodec";
        case HwDeviceType::Vulkan:            return "Vulkan";
        case HwDeviceType::OpenGl:            return "OpenGL / EGL";
        default:                              return "Software Fallback";
    }
}

/// Descriptor for direct zero-copy scanout or hardware buffer export/import.
struct HwBufferDescriptor {
    int32_t fds[4]{-1, -1, -1, -1};           ///< DMA-BUF file descriptors (unique object FDs; may be shared across planes)
    uint32_t strides[4]{0, 0, 0, 0};          ///< Hardware line pitch / stride per plane
    uint32_t offsets[4]{0, 0, 0, 0};          ///< Plane byte offsets
    uint64_t modifier{0};                     ///< Primary DRM format modifier (DRM_FORMAT_MOD_LINEAR)
    uint32_t drm_fourcc{0};                   ///< DRM format FourCC (DRM_FORMAT_XRGB8888, DRM_FORMAT_NV12)
    uint32_t width{0};
    uint32_t height{0};
    uint64_t modifiers[4]{0, 0, 0, 0};        ///< Per-plane DRM format modifiers (fallback to modifier if 0)
    uint32_t object_indices[4]{0, 1, 2, 3};   ///< Maps layer/plane i -> fds[object_indices[i]]
    uint32_t num_planes{0};                   ///< Number of populated layers/planes (0 = infer from strides/fds)

    [[nodiscard]] int32_t plane_fd(size_t plane_idx) const noexcept {
        if (plane_idx >= 4) return -1;
        uint32_t obj = object_indices[plane_idx];
        if (obj < 4 && fds[obj] >= 0) return fds[obj];
        if (fds[plane_idx] >= 0) return fds[plane_idx];
        // Common VA-API DRM_PRIME_2 case: 1 DRM object shared across 2 layers (fds[0] valid, fds[1] == -1)
        if (strides[plane_idx] > 0 && fds[0] >= 0) return fds[0];
        return -1;
    }

    [[nodiscard]] uint64_t plane_modifier(size_t plane_idx) const noexcept {
        if (plane_idx >= 4) return modifier;
        return modifiers[plane_idx] != 0 ? modifiers[plane_idx] : modifier;
    }
};

/// Hardware device abstraction for zero-copy scanout and hardware decoding.
class HwDevice {
public:
    virtual ~HwDevice() = default;

    [[nodiscard]] virtual HwDeviceType type() const noexcept = 0;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual bool is_available() const noexcept = 0;

    /// Exports a decoded VideoFrame directly into a hardware DMA-BUF descriptor.
    virtual MediaResult<void> export_buffer(const VideoFrame& frame, HwBufferDescriptor& desc) = 0;

    /// Imports a hardware buffer descriptor into a zero-copy VideoFrame.
    virtual MediaResult<VideoFrame> import_buffer(const HwBufferDescriptor& desc) = 0;

    /// Creates default hardware device (probes DRM/KMS or returns software fallback).
    static std::unique_ptr<HwDevice> create_default();
};

} // namespace atum
