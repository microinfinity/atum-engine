#include "atum/hwaccel/opengl/gl_device.hpp"

namespace atum::gl {

GlDevice::GlDevice() = default;

GlDevice::~GlDevice() = default;

GlDevice::GlDevice(GlDevice&& other) noexcept {
    *this = std::move(other);
}

GlDevice& GlDevice::operator=(GlDevice&& other) noexcept {
    if (this != &other) {
        dispatcher_ = std::move(other.dispatcher_);
        context_ = std::move(other.context_);
        importer_ = std::move(other.importer_);
        renderer_ = std::move(other.renderer_);
        name_ = std::move(other.name_);
    }
    return *this;
}

std::unique_ptr<GlDevice> GlDevice::create() {
    auto dev = std::unique_ptr<GlDevice>(new GlDevice());

    auto ctx_res = EglContext::create_headless(dev->dispatcher_);
    if (!ctx_res.is_ok()) {
        return nullptr;
    }

    dev->context_ = std::move(ctx_res.value());
    dev->importer_ = std::make_unique<GlImageImporter>(*dev->context_);
    dev->renderer_ = std::make_unique<GlRenderer>(*dev->context_);

    if (!dev->context_->gl_renderer().empty()) {
        dev->name_ = "OpenGL / EGL (" + std::string(dev->context_->gl_renderer()) + ")";
    } else {
        dev->name_ = "OpenGL / EGL";
    }

    return dev;
}

MediaResult<void> GlDevice::export_buffer(const VideoFrame& frame, HwBufferDescriptor& desc) {
    auto desc_res = GlImageImporter::descriptor_from_frame(frame);
    if (!desc_res.is_ok()) {
        return desc_res.error();
    }
    desc = desc_res.value();
    return MediaResult<void>::ok();
}

MediaResult<VideoFrame> GlDevice::import_buffer(const HwBufferDescriptor& desc) {
    if (!importer_) {
        return MediaError::InvalidState;
    }

    const bool is_planar_yuv =
        desc.drm_fourcc == DRM_FOURCC_NV12 ||
        desc.drm_fourcc == DRM_FOURCC_NV21 ||
        desc.drm_fourcc == DRM_FOURCC_P010 ||
        desc.drm_fourcc == DRM_FOURCC_YUV420;

    if (is_planar_yuv && renderer_) {
        auto planar_res = importer_->import_planar_dma_buf(desc);
        if (planar_res.is_ok()) {
            auto rgba_res = renderer_->render_to_texture(planar_res.value(), desc.width, desc.height);
            if (rgba_res.is_ok()) {
                return rgba_res.value().read_pixels_to_frame();
            }
        }
    }

    auto tex_res = importer_->import_dma_buf(desc);
    if (!tex_res.is_ok()) {
        return tex_res.error();
    }

    return tex_res.value().read_pixels_to_frame();
}

} // namespace atum::gl
