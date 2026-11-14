#pragma once

#include <cstdint>
#include <memory>

#include "atum/surface/video_frame.hpp"
#include "atum/hwaccel/hwaccel.hpp"
#include "atum/hwaccel/opengl/gl_texture.hpp"
#include "atum/hwaccel/opengl/egl_context.hpp"

namespace atum::gl {

/// High-performance zero-copy DMA-BUF importer for OpenGL textures.
class GlImageImporter {
public:
    explicit GlImageImporter(const EglContext& context) noexcept;
    ~GlImageImporter() = default;

    // Non-copyable, movable
    GlImageImporter(const GlImageImporter&) = delete;
    GlImageImporter& operator=(const GlImageImporter&) = delete;
    GlImageImporter(GlImageImporter&& other) noexcept = default;
    GlImageImporter& operator=(GlImageImporter&& other) noexcept = default;

    /// Extracts a Linux DMA-BUF descriptor from a hardware-backed VideoFrame without downloading pixels.
    [[nodiscard]] static MediaResult<HwBufferDescriptor> descriptor_from_frame(const VideoFrame& frame);

    /// Imports an external Linux DMA-BUF descriptor into an OpenGL texture with zero-copy.
    MediaResult<GlTexture> import_dma_buf(const HwBufferDescriptor& desc);

    /// Imports each plane of a hardware DMA-BUF descriptor as independent GL_TEXTURE_2D textures
    /// (NV12/NV21 -> R8+GR88, P010 -> R16+GR1616, YUV420 -> 3x R8, RGBA/BGRA -> single texture).
    MediaResult<GlPlanarTextures> import_planar_dma_buf(const HwBufferDescriptor& desc);

    /// Imports a hardware VideoFrame into an OpenGL texture with zero-copy.
    MediaResult<GlTexture> import_frame(const VideoFrame& frame);

    /// Imports a hardware VideoFrame into per-plane GL_TEXTURE_2D textures for zero-copy shader CSC.
    MediaResult<GlPlanarTextures> import_frame_planes(const VideoFrame& frame);

    /// Creates an OpenGL texture from either a hardware DMA-BUF frame or host CPU memory.
    MediaResult<GlTexture> create_texture_from_frame(const VideoFrame& frame);

    [[nodiscard]] const EglContext& context() const noexcept { return *context_; }

private:
    const EglContext* context_{nullptr};
};

} // namespace atum::gl
