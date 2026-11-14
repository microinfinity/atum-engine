#include "atum/hwaccel/opengl/gl_image_importer.hpp"
#include "atum/surface/color_convert.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>
#include <iostream>

namespace atum::gl {

namespace {

struct DmaBufPlaneAttribIds {
    EGLint fd;
    EGLint offset;
    EGLint pitch;
    EGLint mod_lo;
    EGLint mod_hi;
};

constexpr std::array<DmaBufPlaneAttribIds, 4> kPlaneAttribIds = {{
    {
        EGL_DMA_BUF_PLANE0_FD_EXT_VAL,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT_VAL,
        EGL_DMA_BUF_PLANE0_PITCH_EXT_VAL,
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT_VAL,
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT_VAL
    },
    {
        EGL_DMA_BUF_PLANE1_FD_EXT_VAL,
        EGL_DMA_BUF_PLANE1_OFFSET_EXT_VAL,
        EGL_DMA_BUF_PLANE1_PITCH_EXT_VAL,
        EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT_VAL,
        EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT_VAL
    },
    {
        EGL_DMA_BUF_PLANE2_FD_EXT_VAL,
        EGL_DMA_BUF_PLANE2_OFFSET_EXT_VAL,
        EGL_DMA_BUF_PLANE2_PITCH_EXT_VAL,
        EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT_VAL,
        EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT_VAL
    },
    {
        EGL_DMA_BUF_PLANE3_FD_EXT_VAL,
        EGL_DMA_BUF_PLANE3_OFFSET_EXT_VAL,
        EGL_DMA_BUF_PLANE3_PITCH_EXT_VAL,
        EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT_VAL,
        EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT_VAL
    }
}};

[[nodiscard]] bool is_planar_yuv_fourcc(uint32_t fourcc) noexcept {
    return fourcc == DRM_FOURCC_NV12 ||
           fourcc == DRM_FOURCC_NV21 ||
           fourcc == DRM_FOURCC_P010 ||
           fourcc == DRM_FOURCC_YUV420;
}

} // namespace

GlImageImporter::GlImageImporter(const EglContext& context) noexcept
    : context_(&context) {}

MediaResult<GlTexture> GlImageImporter::import_dma_buf(const HwBufferDescriptor& desc) {
    if (!context_ || !context_->is_valid()) {
        return MediaError::InvalidState;
    }

    const auto& disp = context_->dispatcher();
    if (!disp.eglCreateImageKHR || !disp.glEGLImageTargetTexture2DOES) {
        return MediaError::HardwareError;
    }

    if (desc.plane_fd(0) < 0 || desc.width == 0 || desc.height == 0) {
        return MediaError::InvalidData;
    }

    std::vector<EGLint> attribs;
    attribs.reserve(48);

    attribs.push_back(EGL_WIDTH_VAL);
    attribs.push_back(static_cast<EGLint>(desc.width));

    attribs.push_back(EGL_HEIGHT_VAL);
    attribs.push_back(static_cast<EGLint>(desc.height));

    attribs.push_back(EGL_LINUX_DRM_FOURCC_EXT_VAL);
    attribs.push_back(static_cast<EGLint>(desc.drm_fourcc));

    const uint32_t max_planes = desc.num_planes > 0 ? std::min<uint32_t>(desc.num_planes, 4U) : 4U;
    for (uint32_t p = 0; p < max_planes; ++p) {
        const int32_t pfd = desc.plane_fd(p);
        if (p > 0) {
            if (pfd < 0 || desc.strides[p] == 0) {
                if (desc.num_planes == 0) break;
                continue;
            }
        }

        const auto& ids = kPlaneAttribIds[p];
        attribs.push_back(ids.fd);
        attribs.push_back(pfd);

        attribs.push_back(ids.offset);
        attribs.push_back(static_cast<EGLint>(desc.offsets[p]));

        attribs.push_back(ids.pitch);
        attribs.push_back(static_cast<EGLint>(desc.strides[p]));

        const uint64_t mod = desc.plane_modifier(p);
        if (mod != DRM_FORMAT_MOD_INVALID_VAL && mod != 0) {
            attribs.push_back(ids.mod_lo);
            attribs.push_back(static_cast<EGLint>(mod & 0xFFFFFFFFULL));
            attribs.push_back(ids.mod_hi);
            attribs.push_back(static_cast<EGLint>((mod >> 32) & 0xFFFFFFFFULL));
        }
    }

    attribs.push_back(EGL_NONE_VAL);

    EglContext::Scope scope(*context_);

    EGLImageKHR image = disp.eglCreateImageKHR(
        context_->display(),
        EGL_NO_CONTEXT_PTR,
        EGL_LINUX_DMA_BUF_EXT_VAL,
        nullptr,
        attribs.data()
    );

    if (image == EGL_NO_IMAGE_KHR_PTR) {
        return MediaError::UnsupportedFormat;
    }

    if (disp.glGetError) {
        while (disp.glGetError() != GL_NO_ERROR_VAL) {}
    }

    const GLenum primary_target = is_planar_yuv_fourcc(desc.drm_fourcc)
        ? GL_TEXTURE_EXTERNAL_OES_VAL
        : GL_TEXTURE_2D_VAL;
    const GLenum fallback_target = (primary_target == GL_TEXTURE_EXTERNAL_OES_VAL)
        ? GL_TEXTURE_2D_VAL
        : GL_TEXTURE_EXTERNAL_OES_VAL;

    GLuint texture_id = 0;
    GLenum target = primary_target;
    disp.glGenTextures(1, &texture_id);
    disp.glBindTexture(target, texture_id);
    disp.glEGLImageTargetTexture2DOES(target, image);

    GLenum gl_err = disp.glGetError ? disp.glGetError() : GL_NO_ERROR_VAL;
    if (gl_err != GL_NO_ERROR_VAL) {
        disp.glBindTexture(target, 0);
        disp.glDeleteTextures(1, &texture_id);
        texture_id = 0;

        // Drain error queue (e.g. invalid GL_TEXTURE_EXTERNAL_OES on desktop GL Core) before fallback.
        if (disp.glGetError) {
            while (disp.glGetError() != GL_NO_ERROR_VAL) {}
        }

        // Allocate a fresh GL texture name when switching targets to obey GL target binding rules.
        disp.glGenTextures(1, &texture_id);
        target = fallback_target;
        disp.glBindTexture(target, texture_id);
        disp.glEGLImageTargetTexture2DOES(target, image);

        gl_err = disp.glGetError ? disp.glGetError() : GL_NO_ERROR_VAL;
        if (gl_err != GL_NO_ERROR_VAL) {
            disp.glBindTexture(target, 0);
            disp.glDeleteTextures(1, &texture_id);
            if (disp.eglDestroyImageKHR) {
                disp.eglDestroyImageKHR(context_->display(), image);
            }
            return MediaError::UnsupportedFormat;
        }
    }

    disp.glTexParameteri(target, GL_TEXTURE_MIN_FILTER_VAL, GL_LINEAR_VAL);
    disp.glTexParameteri(target, GL_TEXTURE_MAG_FILTER_VAL, GL_LINEAR_VAL);
    disp.glTexParameteri(target, GL_TEXTURE_WRAP_S_VAL, GL_CLAMP_TO_EDGE_VAL);
    disp.glTexParameteri(target, GL_TEXTURE_WRAP_T_VAL, GL_CLAMP_TO_EDGE_VAL);

    disp.glBindTexture(target, 0);

    GlTexture texture(*context_);
    texture.texture_ = texture_id;
    texture.target_ = target;
    texture.egl_image_ = image;
    texture.width_ = desc.width;
    texture.height_ = desc.height;
    texture.drm_fourcc_ = desc.drm_fourcc;

    return texture;
}

MediaResult<HwBufferDescriptor> GlImageImporter::descriptor_from_frame(const VideoFrame& frame) {
    if (!frame.is_valid() || !frame.is_hw_frame()) {
        return MediaError::InvalidData;
    }

    if (const auto& backing = frame.hw_backing()) {
        const HwFrameBufferLayout* layout = backing->export_dma_buf(true);
        if (layout && layout->plane_fd(0) >= 0) {
            HwBufferDescriptor desc{};
            desc.width = layout->width > 0 ? layout->width : frame.width();
            desc.height = layout->height > 0 ? layout->height : frame.height();
            desc.drm_fourcc = layout->drm_fourcc;
            desc.modifier = layout->modifier;
            desc.num_planes = layout->num_planes;
            for (size_t i = 0; i < 4; ++i) {
                desc.fds[i] = layout->fds[i];
                desc.strides[i] = layout->strides[i];
                desc.offsets[i] = layout->offsets[i];
                desc.modifiers[i] = layout->modifiers[i];
                desc.object_indices[i] = layout->object_indices[i];
            }
            return desc;
        }
    }

    const int32_t fd = frame.dma_buf_fd();
    if (fd < 0) {
        return MediaError::InvalidData;
    }

    HwBufferDescriptor desc{};
    desc.fds[0] = fd;
    desc.width = frame.width();
    desc.height = frame.height();
    desc.strides[0] = static_cast<uint32_t>(frame.stride(0));
    desc.offsets[0] = 0;
    desc.modifier = DRM_FORMAT_MOD_LINEAR_VAL;

    const uint32_t uv_w = (frame.width() + 1U) / 2U;
    const uint32_t uv_h = (frame.height() + 1U) / 2U;

    switch (frame.pixel_format()) {
        case PixelFormat::NV12:
            desc.drm_fourcc = DRM_FOURCC_NV12;
            desc.fds[1] = fd;
            desc.offsets[1] = static_cast<uint32_t>(frame.height() * desc.strides[0]);
            desc.strides[1] = static_cast<uint32_t>(frame.stride(1) > 0 ? frame.stride(1) : desc.strides[0]);
            desc.num_planes = 2;
            break;
        case PixelFormat::NV21:
            desc.drm_fourcc = DRM_FOURCC_NV21;
            desc.fds[1] = fd;
            desc.offsets[1] = static_cast<uint32_t>(frame.height() * desc.strides[0]);
            desc.strides[1] = static_cast<uint32_t>(frame.stride(1) > 0 ? frame.stride(1) : desc.strides[0]);
            desc.num_planes = 2;
            break;
        case PixelFormat::P010:
            desc.drm_fourcc = DRM_FOURCC_P010;
            desc.fds[1] = fd;
            desc.offsets[1] = static_cast<uint32_t>(frame.height() * desc.strides[0]);
            desc.strides[1] = static_cast<uint32_t>(frame.stride(1) > 0 ? frame.stride(1) : desc.strides[0]);
            desc.num_planes = 2;
            break;
        case PixelFormat::YUV420P:
            desc.drm_fourcc = DRM_FOURCC_YUV420;
            desc.fds[1] = fd;
            desc.fds[2] = fd;
            desc.strides[1] = static_cast<uint32_t>(frame.stride(1) > 0 ? frame.stride(1) : uv_w);
            desc.strides[2] = static_cast<uint32_t>(frame.stride(2) > 0 ? frame.stride(2) : uv_w);
            desc.offsets[1] = static_cast<uint32_t>(frame.height() * desc.strides[0]);
            desc.offsets[2] = desc.offsets[1] + static_cast<uint32_t>(uv_h * desc.strides[1]);
            desc.num_planes = 3;
            break;
        case PixelFormat::RGBA32:
            desc.drm_fourcc = DRM_FOURCC_RGBA8888;
            desc.num_planes = 1;
            break;
        case PixelFormat::BGRA32:
            desc.drm_fourcc = DRM_FOURCC_ARGB8888;
            desc.num_planes = 1;
            break;
        default:
            return MediaError::UnsupportedFormat;
    }

    return desc;
}

MediaResult<GlPlanarTextures> GlImageImporter::import_planar_dma_buf(const HwBufferDescriptor& desc) {
    if (!context_ || !context_->is_valid()) {
        return MediaError::InvalidState;
    }
    if (desc.plane_fd(0) < 0 || desc.width == 0 || desc.height == 0) {
        return MediaError::InvalidData;
    }

    EglContext::Scope scope(*context_);

    if (!is_planar_yuv_fourcc(desc.drm_fourcc)) {
        auto single_res = import_dma_buf(desc);
        if (!single_res.is_ok()) {
            return single_res.error();
        }
        GlPlanarTextures result{};
        result.planes[0] = std::move(single_res.value());
        result.plane_count = 1;
        result.drm_fourcc = desc.drm_fourcc;
        result.width = desc.width;
        result.height = desc.height;
        return result;
    }

    struct PlaneSpec {
        uint32_t width{0};
        uint32_t height{0};
        uint32_t fourcc{0};
    };

    const uint32_t uv_w = (desc.width + 1U) / 2U;
    const uint32_t uv_h = (desc.height + 1U) / 2U;
    std::array<PlaneSpec, 3> specs{};
    uint32_t expected_planes = 0;

    if (desc.drm_fourcc == DRM_FOURCC_NV12 || desc.drm_fourcc == DRM_FOURCC_NV21) {
        specs[0] = {desc.width, desc.height, DRM_FOURCC_R8};
        specs[1] = {uv_w, uv_h, DRM_FOURCC_GR88};
        expected_planes = 2;
    } else if (desc.drm_fourcc == DRM_FOURCC_P010) {
        specs[0] = {desc.width, desc.height, DRM_FOURCC_R16};
        specs[1] = {uv_w, uv_h, DRM_FOURCC_GR1616};
        expected_planes = 2;
    } else if (desc.drm_fourcc == DRM_FOURCC_YUV420) {
        specs[0] = {desc.width, desc.height, DRM_FOURCC_R8};
        specs[1] = {uv_w, uv_h, DRM_FOURCC_R8};
        specs[2] = {uv_w, uv_h, DRM_FOURCC_R8};
        expected_planes = 3;
    } else {
        return MediaError::UnsupportedFormat;
    }

    GlPlanarTextures result{};
    result.plane_count = expected_planes;
    result.drm_fourcc = desc.drm_fourcc;
    result.width = desc.width;
    result.height = desc.height;

    for (uint32_t p = 0; p < expected_planes; ++p) {
        const int32_t pfd = desc.plane_fd(p);
        if (pfd < 0 || desc.strides[p] == 0) {
            return MediaError::InvalidData;
        }

        HwBufferDescriptor plane_desc{};
        plane_desc.fds[0] = pfd;
        plane_desc.width = specs[p].width;
        plane_desc.height = specs[p].height;
        plane_desc.strides[0] = desc.strides[p];
        plane_desc.offsets[0] = desc.offsets[p];
        plane_desc.modifiers[0] = desc.plane_modifier(p);
        plane_desc.modifier = plane_desc.modifiers[0];
        plane_desc.drm_fourcc = specs[p].fourcc;
        plane_desc.num_planes = 1;

        auto tex_res = import_dma_buf(plane_desc);
        if (!tex_res.is_ok()) {
            return tex_res.error();
        }
        result.planes[p] = std::move(tex_res.value());
    }

    return result;
}

MediaResult<GlTexture> GlImageImporter::import_frame(const VideoFrame& frame) {
    auto desc_res = descriptor_from_frame(frame);
    if (!desc_res.is_ok()) {
        return desc_res.error();
    }
    return import_dma_buf(desc_res.value());
}

MediaResult<GlPlanarTextures> GlImageImporter::import_frame_planes(const VideoFrame& frame) {
    auto desc_res = descriptor_from_frame(frame);
    if (!desc_res.is_ok()) {
        return desc_res.error();
    }
    auto res = import_planar_dma_buf(desc_res.value());
    if (!res.is_ok()) {
        return res.error();
    }
    res.value().color_space = frame.color_space();
    res.value().color_range = frame.color_range();
    return res;
}

MediaResult<GlTexture> GlImageImporter::create_texture_from_frame(const VideoFrame& frame) {
    if (!frame.is_valid()) {
        return MediaError::InvalidData;
    }

    // Direct hardware zero-copy path
    if (frame.is_hw_frame()) {
        auto hw_res = import_frame(frame);
        if (hw_res.is_ok()) {
            return hw_res;
        }
    }

    // Host CPU memory upload path
    if (!context_ || !context_->is_valid()) {
        return MediaError::InvalidState;
    }

    EglContext::Scope scope(*context_);
    const auto& disp = context_->dispatcher();

    std::vector<uint8_t> converted_rgba;
    const uint8_t* upload_ptr = nullptr;
    GLenum gl_fmt = GL_RGBA_VAL;

    auto pack_tight_rows = [&](size_t bpp) {
        const size_t row_bytes = static_cast<size_t>(frame.width()) * bpp;
        const size_t src_stride = frame.stride(0);
        const uint8_t* src = frame.plane(0);
        if (src_stride == row_bytes || src_stride == 0) {
            return src;
        }
        converted_rgba.resize(row_bytes * frame.height());
        for (uint32_t y = 0; y < frame.height(); ++y) {
            std::memcpy(converted_rgba.data() + static_cast<size_t>(y) * row_bytes,
                        src + static_cast<size_t>(y) * src_stride,
                        row_bytes);
        }
        return static_cast<const uint8_t*>(converted_rgba.data());
    };

    if (frame.pixel_format() == PixelFormat::RGBA32) {
        upload_ptr = pack_tight_rows(4);
        gl_fmt = GL_RGBA_VAL;
    } else if (frame.pixel_format() == PixelFormat::RGB24) {
        upload_ptr = pack_tight_rows(3);
        gl_fmt = GL_RGB_VAL;
    } else if (frame.pixel_format() == PixelFormat::NV12) {
        converted_rgba.resize(static_cast<size_t>(frame.width()) * frame.height() * 4);
        ColorConverter::nv12_to_rgba(
            frame.plane(0), frame.stride(0),
            frame.plane(1), frame.stride(1),
            converted_rgba.data(), frame.width() * 4,
            frame.width(), frame.height(),
            frame.color_space(), frame.color_range()
        );
        upload_ptr = converted_rgba.data();
        gl_fmt = GL_RGBA_VAL;
    } else if (frame.pixel_format() == PixelFormat::YUV420P) {
        converted_rgba.resize(static_cast<size_t>(frame.width()) * frame.height() * 4);
        ColorConverter::yuv420p_to_rgba(
            frame.plane(0), frame.stride(0),
            frame.plane(1), frame.stride(1),
            frame.plane(2), frame.stride(2),
            converted_rgba.data(), frame.width() * 4,
            frame.width(), frame.height(),
            frame.color_space(), frame.color_range()
        );
        upload_ptr = converted_rgba.data();
        gl_fmt = GL_RGBA_VAL;
    } else {
        return MediaError::UnsupportedFormat;
    }

    GLuint texture_id = 0;
    disp.glGenTextures(1, &texture_id);
    disp.glBindTexture(GL_TEXTURE_2D_VAL, texture_id);

    disp.glTexImage2D(
        GL_TEXTURE_2D_VAL,
        0,
        static_cast<GLint>(gl_fmt),
        static_cast<GLsizei>(frame.width()),
        static_cast<GLsizei>(frame.height()),
        0,
        gl_fmt,
        GL_UNSIGNED_BYTE_VAL,
        upload_ptr
    );

    disp.glTexParameteri(GL_TEXTURE_2D_VAL, GL_TEXTURE_MIN_FILTER_VAL, GL_LINEAR_VAL);
    disp.glTexParameteri(GL_TEXTURE_2D_VAL, GL_TEXTURE_MAG_FILTER_VAL, GL_LINEAR_VAL);
    disp.glTexParameteri(GL_TEXTURE_2D_VAL, GL_TEXTURE_WRAP_S_VAL, GL_CLAMP_TO_EDGE_VAL);
    disp.glTexParameteri(GL_TEXTURE_2D_VAL, GL_TEXTURE_WRAP_T_VAL, GL_CLAMP_TO_EDGE_VAL);

    disp.glBindTexture(GL_TEXTURE_2D_VAL, 0);

    GlTexture texture(*context_);
    texture.texture_ = texture_id;
    texture.target_ = GL_TEXTURE_2D_VAL;
    texture.width_ = frame.width();
    texture.height_ = frame.height();

    return texture;
}

} // namespace atum::gl
