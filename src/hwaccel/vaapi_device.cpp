#include "atum/hwaccel/vaapi_device.hpp"
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#include <dlfcn.h>
#endif
#include <cstring>
#include <iostream>
#include <algorithm>

#if __has_include(<va/va.h>) && __has_include(<va/va_drm.h>)
#include <va/va.h>
#include <va/va_drm.h>
#if __has_include(<va/va_drmcommon.h>)
#include <va/va_drmcommon.h>
#endif
#define NISABA_HAS_VAAPI_HEADERS 1
#endif

namespace atum {

#if defined(NISABA_HAS_VAAPI_HEADERS)
namespace {

// Function pointer signatures for dynamic libva loading
typedef VADisplay (*FnVaGetDisplayDRM)(int fd);
typedef VAStatus (*FnVaInitialize)(VADisplay dpy, int* major_version, int* minor_version);
typedef VAStatus (*FnVaTerminate)(VADisplay dpy);
typedef const char* (*FnVaErrorStr)(VAStatus error_status);
typedef int (*FnVaMaxNumProfiles)(VADisplay dpy);
typedef VAStatus (*FnVaQueryConfigProfiles)(VADisplay dpy, VAProfile* profile_list, int* num_profiles);
typedef VAStatus (*FnVaCreateConfig)(VADisplay dpy, VAProfile profile, VAEntrypoint entrypoint, VAConfigAttrib* attrib_list, int num_attribs, VAConfigID* config_id);
typedef VAStatus (*FnVaDestroyConfig)(VADisplay dpy, VAConfigID config_id);
typedef VAStatus (*FnVaCreateSurfaces)(VADisplay dpy, unsigned int format, unsigned int width, unsigned int height, VASurfaceID* surfaces, unsigned int num_surfaces, VASurfaceAttrib* attrib_list, unsigned int num_attribs);
typedef VAStatus (*FnVaDestroySurfaces)(VADisplay dpy, VASurfaceID* surface_list, int num_surfaces);
typedef VAStatus (*FnVaCreateContext)(VADisplay dpy, VAConfigID config_id, int picture_width, int picture_height, int flag, VASurfaceID* render_targets, int num_render_targets, VAContextID* context);
typedef VAStatus (*FnVaDestroyContext)(VADisplay dpy, VAContextID context);
typedef VAStatus (*FnVaCreateBuffer)(VADisplay dpy, VAContextID context, VABufferType type, unsigned int size, unsigned int num_elements, void* data, VABufferID* buf_id);
typedef VAStatus (*FnVaDestroyBuffer)(VADisplay dpy, VABufferID buffer_id);
typedef VAStatus (*FnVaBeginPicture)(VADisplay dpy, VAContextID context, VASurfaceID render_target);
typedef VAStatus (*FnVaRenderPicture)(VADisplay dpy, VAContextID context, VABufferID* buffers, int num_buffers);
typedef VAStatus (*FnVaEndPicture)(VADisplay dpy, VAContextID context);
typedef VAStatus (*FnVaSyncSurface)(VADisplay dpy, VASurfaceID render_target);
typedef VAStatus (*FnVaDeriveImage)(VADisplay dpy, VASurfaceID surface, VAImage* image);
typedef VAStatus (*FnVaDestroyImage)(VADisplay dpy, VAImageID image);
typedef VAStatus (*FnVaCreateImage)(VADisplay dpy, VAImageFormat* format, int width, int height, VAImage* image);
typedef VAStatus (*FnVaGetImage)(VADisplay dpy, VASurfaceID surface, int x, int y, unsigned int width, unsigned int height, VAImageID image);
typedef VAStatus (*FnVaPutImage)(VADisplay dpy, VASurfaceID surface, VAImageID image, int src_x, int src_y, unsigned int src_width, unsigned int src_height, int dest_x, int dest_y, unsigned int dest_width, unsigned int dest_height);
typedef VAStatus (*FnVaMapBuffer)(VADisplay dpy, VABufferID buf_id, void** pbuf);
typedef VAStatus (*FnVaUnmapBuffer)(VADisplay dpy, VABufferID buf_id);
typedef VAStatus (*FnVaExportSurfaceHandle)(VADisplay dpy, VASurfaceID surface_id, uint32_t mem_type, uint32_t flags, void* descriptor);

struct VaSymbols {
    FnVaGetDisplayDRM vaGetDisplayDRM{nullptr};
    FnVaInitialize vaInitialize{nullptr};
    FnVaTerminate vaTerminate{nullptr};
    FnVaErrorStr vaErrorStr{nullptr};
    FnVaMaxNumProfiles vaMaxNumProfiles{nullptr};
    FnVaQueryConfigProfiles vaQueryConfigProfiles{nullptr};
    FnVaCreateConfig vaCreateConfig{nullptr};
    FnVaDestroyConfig vaDestroyConfig{nullptr};
    FnVaCreateSurfaces vaCreateSurfaces{nullptr};
    FnVaDestroySurfaces vaDestroySurfaces{nullptr};
    FnVaCreateContext vaCreateContext{nullptr};
    FnVaDestroyContext vaDestroyContext{nullptr};
    FnVaCreateBuffer vaCreateBuffer{nullptr};
    FnVaDestroyBuffer vaDestroyBuffer{nullptr};
    FnVaBeginPicture vaBeginPicture{nullptr};
    FnVaRenderPicture vaRenderPicture{nullptr};
    FnVaEndPicture vaEndPicture{nullptr};
    FnVaSyncSurface vaSyncSurface{nullptr};
    FnVaDeriveImage vaDeriveImage{nullptr};
    FnVaDestroyImage vaDestroyImage{nullptr};
    FnVaCreateImage vaCreateImage{nullptr};
    FnVaGetImage vaGetImage{nullptr};
    FnVaPutImage vaPutImage{nullptr};
    FnVaMapBuffer vaMapBuffer{nullptr};
    FnVaUnmapBuffer vaUnmapBuffer{nullptr};
    FnVaExportSurfaceHandle vaExportSurfaceHandle{nullptr};

    bool load(void* libva, void* libva_drm) noexcept {
        if (!libva || !libva_drm) return false;

        vaGetDisplayDRM = reinterpret_cast<FnVaGetDisplayDRM>(dlsym(libva_drm, "vaGetDisplayDRM"));
        vaInitialize = reinterpret_cast<FnVaInitialize>(dlsym(libva, "vaInitialize"));
        vaTerminate = reinterpret_cast<FnVaTerminate>(dlsym(libva, "vaTerminate"));
        vaErrorStr = reinterpret_cast<FnVaErrorStr>(dlsym(libva, "vaErrorStr"));
        vaMaxNumProfiles = reinterpret_cast<FnVaMaxNumProfiles>(dlsym(libva, "vaMaxNumProfiles"));
        vaQueryConfigProfiles = reinterpret_cast<FnVaQueryConfigProfiles>(dlsym(libva, "vaQueryConfigProfiles"));
        vaCreateConfig = reinterpret_cast<FnVaCreateConfig>(dlsym(libva, "vaCreateConfig"));
        vaDestroyConfig = reinterpret_cast<FnVaDestroyConfig>(dlsym(libva, "vaDestroyConfig"));
        vaCreateSurfaces = reinterpret_cast<FnVaCreateSurfaces>(dlsym(libva, "vaCreateSurfaces"));
        vaDestroySurfaces = reinterpret_cast<FnVaDestroySurfaces>(dlsym(libva, "vaDestroySurfaces"));
        vaCreateContext = reinterpret_cast<FnVaCreateContext>(dlsym(libva, "vaCreateContext"));
        vaDestroyContext = reinterpret_cast<FnVaDestroyContext>(dlsym(libva, "vaDestroyContext"));
        vaCreateBuffer = reinterpret_cast<FnVaCreateBuffer>(dlsym(libva, "vaCreateBuffer"));
        vaDestroyBuffer = reinterpret_cast<FnVaDestroyBuffer>(dlsym(libva, "vaDestroyBuffer"));
        vaBeginPicture = reinterpret_cast<FnVaBeginPicture>(dlsym(libva, "vaBeginPicture"));
        vaRenderPicture = reinterpret_cast<FnVaRenderPicture>(dlsym(libva, "vaRenderPicture"));
        vaEndPicture = reinterpret_cast<FnVaEndPicture>(dlsym(libva, "vaEndPicture"));
        vaSyncSurface = reinterpret_cast<FnVaSyncSurface>(dlsym(libva, "vaSyncSurface"));
        vaDeriveImage = reinterpret_cast<FnVaDeriveImage>(dlsym(libva, "vaDeriveImage"));
        vaDestroyImage = reinterpret_cast<FnVaDestroyImage>(dlsym(libva, "vaDestroyImage"));
        vaCreateImage = reinterpret_cast<FnVaCreateImage>(dlsym(libva, "vaCreateImage"));
        vaGetImage = reinterpret_cast<FnVaGetImage>(dlsym(libva, "vaGetImage"));
        vaPutImage = reinterpret_cast<FnVaPutImage>(dlsym(libva, "vaPutImage"));
        vaMapBuffer = reinterpret_cast<FnVaMapBuffer>(dlsym(libva, "vaMapBuffer"));
        vaUnmapBuffer = reinterpret_cast<FnVaUnmapBuffer>(dlsym(libva, "vaUnmapBuffer"));
        vaExportSurfaceHandle = reinterpret_cast<FnVaExportSurfaceHandle>(dlsym(libva, "vaExportSurfaceHandle"));

        return vaGetDisplayDRM && vaInitialize && vaTerminate && vaErrorStr &&
               vaMaxNumProfiles && vaQueryConfigProfiles;
    }
};

static VaSymbols g_va_syms{};
static std::mutex g_va_syms_mutex;

class VaSurfaceBacking final : public HwFrameBacking {
public:
    VaSurfaceBacking(
        VaapiDevice* device,
        std::shared_ptr<VaDeviceLifetime> lifetime,
        uint32_t surface_id,
        uint32_t width,
        uint32_t height,
        PixelFormat format,
        std::shared_ptr<void> surface_lease
    )
        : device_(device)
        , lifetime_(std::move(lifetime))
        , surface_id_(surface_id)
        , width_(width)
        , height_(height)
        , format_(format)
        , surface_lease_(std::move(surface_lease)) {}

    ~VaSurfaceBacking() override {
        for (size_t i = 0; i < dma_layout_.fds.size(); ++i) {
            int fd = dma_layout_.fds[i];
            if (fd < 0) continue;
            bool duplicate = false;
            for (size_t j = 0; j < i; ++j) {
                if (dma_layout_.fds[j] == fd) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) {
                ::close(fd);
            }
            dma_layout_.fds[i] = -1;
        }
    }

    [[nodiscard]] uint32_t surface_id() const noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        return surface_id_;
    }

    bool ensure_cpu_mapped(
        PixelFormat& inout_format,
        std::array<uint8_t*, 4>& out_planes,
        std::array<size_t, 4>& out_strides
    ) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        return ensure_cpu_mapped_locked(inout_format, out_planes, out_strides);
    }

    [[nodiscard]] const HwFrameBufferLayout* export_dma_buf(bool sync_first) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (dma_exported_) {
            return &dma_layout_;
        }
        if (surface_id_ == 0xFFFFFFFF || !device_ || !lifetime_) {
            return nullptr;
        }

        std::lock_guard<std::mutex> dev_lock(lifetime_->mutex);
        if (!lifetime_->alive) {
            return nullptr;
        }

        if (sync_first && !synced_) {
            if (device_->sync_surface(surface_id_).is_ok()) {
                synced_ = true;
            }
        }

        HwBufferDescriptor desc{};
        if (!device_->export_surface_dma_buf(surface_id_, desc).is_ok()) {
            return nullptr;
        }

        dma_layout_.width = desc.width;
        dma_layout_.height = desc.height;
        dma_layout_.drm_fourcc = desc.drm_fourcc;
        dma_layout_.num_planes = desc.num_planes;
        for (size_t i = 0; i < 4; ++i) {
            dma_layout_.fds[i] = desc.fds[i];
            dma_layout_.strides[i] = desc.strides[i];
            dma_layout_.offsets[i] = desc.offsets[i];
            dma_layout_.modifiers[i] = desc.modifiers[i];
            dma_layout_.object_indices[i] = desc.object_indices[i];
        }
        dma_layout_.modifier = desc.modifier;

        uint32_t obj_cnt = 0;
        for (int32_t fd : desc.fds) {
            if (fd >= 0) ++obj_cnt;
        }
        dma_layout_.num_objects = obj_cnt;
        dma_exported_ = true;
        return &dma_layout_;
    }

    [[nodiscard]] int32_t cached_dma_buf_fd() const noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        return dma_exported_ ? dma_layout_.plane_fd(0) : -1;
    }

    void detach_surface_to_cpu() noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        PixelFormat fmt = format_;
        std::array<uint8_t*, 4> planes{};
        std::array<size_t, 4> strides{};
        (void)ensure_cpu_mapped_locked(fmt, planes, strides);
        surface_id_ = 0xFFFFFFFF;
        surface_lease_.reset();
    }

private:
    bool ensure_cpu_mapped_locked(
        PixelFormat& inout_format,
        std::array<uint8_t*, 4>& out_planes,
        std::array<size_t, 4>& out_strides
    ) noexcept {
        if (!cpu_mapped_) {
            if (surface_id_ == 0xFFFFFFFF || !device_ || !lifetime_) {
                return false;
            }
            std::lock_guard<std::mutex> dev_lock(lifetime_->mutex);
            if (!lifetime_->alive) {
                return false;
            }
            if (!synced_) {
                if (device_->sync_surface(surface_id_).is_ok()) {
                    synced_ = true;
                }
            }
            auto derived = device_->derive_surface_to_frame(surface_id_, width_, height_, format_);
            if (!derived.is_ok() || !derived->is_valid()) {
                return false;
            }
            cpu_frame_ = std::move(*derived);
            cpu_mapped_ = true;
        }

        if (!cpu_frame_.is_valid()) return false;
        inout_format = cpu_frame_.pixel_format();
        for (size_t i = 0; i < 4; ++i) {
            out_planes[i] = const_cast<uint8_t*>(cpu_frame_.plane(i));
            out_strides[i] = cpu_frame_.stride(i);
        }
        return out_planes[0] != nullptr;
    }

    VaapiDevice* device_{nullptr};
    std::shared_ptr<VaDeviceLifetime> lifetime_;
    uint32_t surface_id_{0xFFFFFFFF};
    uint32_t width_{0};
    uint32_t height_{0};
    PixelFormat format_{PixelFormat::NV12};
    std::shared_ptr<void> surface_lease_;

    mutable std::mutex mutex_;
    bool synced_{false};
    bool cpu_mapped_{false};
    VideoFrame cpu_frame_;
    bool dma_exported_{false};
    HwFrameBufferLayout dma_layout_{};
};

} // anonymous namespace
#endif

VaapiDevice::VaapiDevice()
    : lifetime_(std::make_shared<VaDeviceLifetime>()) {
    init_internal();
}

VaapiDevice::~VaapiDevice() {
    close_internal();
}

void VaapiDevice::init_internal() noexcept {
#if defined(NISABA_HAS_VAAPI_HEADERS)
    // 1. Attempt to dynamic-load libva and libva-drm
    libva_handle_ = dlopen("libva.so.2", RTLD_NOW);
    if (!libva_handle_) {
        libva_handle_ = dlopen("libva.so", RTLD_NOW);
    }
    libva_drm_handle_ = dlopen("libva-drm.so.2", RTLD_NOW);
    if (!libva_drm_handle_) {
        libva_drm_handle_ = dlopen("libva-drm.so", RTLD_NOW);
    }

    if (!libva_handle_ || !libva_drm_handle_) {
        return;
    }

    {
        std::lock_guard<std::mutex> sym_lock(g_va_syms_mutex);
        if (!g_va_syms.load(libva_handle_, libva_drm_handle_)) {
            return;
        }
    }

    // 2. Probe Linux DRM render nodes and validate VA-API initialization on each candidate
    const char* render_nodes[] = {
        "/dev/dri/renderD128",
        "/dev/dri/renderD129",
        "/dev/dri/renderD130",
        "/dev/dri/renderD131",
        "/dev/dri/renderD132",
        "/dev/dri/renderD133",
        "/dev/dri/renderD134",
        "/dev/dri/renderD135",
        "/dev/dri/card0",
        "/dev/dri/card1"
    };

    for (const char* node : render_nodes) {
        int fd = open(node, O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;

        VADisplay dpy = g_va_syms.vaGetDisplayDRM(fd);
        if (!dpy) {
            close(fd);
            continue;
        }

        int major = 0, minor = 0;
        VAStatus status = g_va_syms.vaInitialize(dpy, &major, &minor);
        if (status == VA_STATUS_SUCCESS) {
            drm_fd_ = fd;
            va_display_ = dpy;
            is_initialized_ = true;
            if (lifetime_) {
                std::lock_guard<std::mutex> lock(lifetime_->mutex);
                lifetime_->alive = true;
            }
            return;
        }

        if (g_va_syms.vaTerminate) {
            g_va_syms.vaTerminate(dpy);
        }
        close(fd);
    }
#endif
}

void VaapiDevice::close_internal() noexcept {
#if defined(NISABA_HAS_VAAPI_HEADERS)
    if (lifetime_) {
        std::lock_guard<std::mutex> lock(lifetime_->mutex);
        lifetime_->alive = false;
    }
    if (is_initialized_ && va_display_ && g_va_syms.vaTerminate) {
        g_va_syms.vaTerminate(reinterpret_cast<VADisplay>(va_display_));
        va_display_ = nullptr;
    }
    if (drm_fd_ >= 0) {
        close(drm_fd_);
        drm_fd_ = -1;
    }
    if (libva_drm_handle_) {
        dlclose(libva_drm_handle_);
        libva_drm_handle_ = nullptr;
    }
    if (libva_handle_) {
        dlclose(libva_handle_);
        libva_handle_ = nullptr;
    }
    is_initialized_ = false;
#endif
}

bool VaapiDevice::is_available() const noexcept {
    return is_initialized_ && va_display_ != nullptr;
}

bool VaapiDevice::supports_codec(CodecId codec) const noexcept {
#if defined(NISABA_HAS_VAAPI_HEADERS)
    if (!is_available()) return false;

    VADisplay dpy = reinterpret_cast<VADisplay>(va_display_);
    if (!g_va_syms.vaMaxNumProfiles || !g_va_syms.vaQueryConfigProfiles) return false;
    int max_profiles = g_va_syms.vaMaxNumProfiles(dpy);
    if (max_profiles <= 0) {
        return false;
    }

    std::vector<VAProfile> profiles(static_cast<size_t>(max_profiles));
    int actual_num = 0;
    if (g_va_syms.vaQueryConfigProfiles(dpy, profiles.data(), &actual_num) != VA_STATUS_SUCCESS) {
        return false;
    }

    for (int i = 0; i < actual_num; ++i) {
        VAProfile p = profiles[static_cast<size_t>(i)];
        if (codec == CodecId::H264) {
            if (p == VAProfileH264Main ||
                p == VAProfileH264High || p == VAProfileH264ConstrainedBaseline) {
                return true;
            }
        } else if (codec == CodecId::H265 || codec == CodecId::HEVC) {
            if (p == VAProfileHEVCMain || p == VAProfileHEVCMain10) {
                return true;
            }
        } else if (codec == CodecId::VP9) {
            if (p == VAProfileVP9Profile0 || p == VAProfileVP9Profile2) {
                return true;
            }
        } else if (codec == CodecId::AV1) {
            if (p == VAProfileAV1Profile0) {
                return true;
            }
        }
    }
#else
    (void)codec;
#endif
    return false;
}

MediaResult<uint32_t> VaapiDevice::create_config(CodecId codec, bool is_encoder, uint32_t rate_control) {
#if defined(NISABA_HAS_VAAPI_HEADERS)
    if (!is_available() || !g_va_syms.vaCreateConfig) return MediaError::HardwareError;

    VADisplay dpy = reinterpret_cast<VADisplay>(va_display_);
    VAProfile profile = VAProfileNone;

    if (codec == CodecId::H264) profile = VAProfileH264High;
    else if (codec == CodecId::H265 || codec == CodecId::HEVC) profile = VAProfileHEVCMain;
    else if (codec == CodecId::VP9) profile = VAProfileVP9Profile0;
    else if (codec == CodecId::AV1) profile = VAProfileAV1Profile0;
    else return MediaError::UnsupportedCodec;

    VAEntrypoint entrypoint = is_encoder ? VAEntrypointEncSlice : VAEntrypointVLD;
    VAConfigAttrib attribs[2];
    int num_attribs = 0;
    if (is_encoder) {
        attribs[0].type = VAConfigAttribRateControl;
        attribs[0].value = (rate_control != 0) ? rate_control : VA_RC_CQP;
        num_attribs = 1;
    }

    VAConfigID cfg = VA_INVALID_ID;
    VAStatus st = g_va_syms.vaCreateConfig(dpy, profile, entrypoint, num_attribs > 0 ? attribs : nullptr, num_attribs, &cfg);
    if (st != VA_STATUS_SUCCESS) {
        // Fallback for H.264
        if (codec == CodecId::H264) {
            st = g_va_syms.vaCreateConfig(dpy, VAProfileH264Main, entrypoint, num_attribs > 0 ? attribs : nullptr, num_attribs, &cfg);
            if (st != VA_STATUS_SUCCESS && is_encoder) {
                st = g_va_syms.vaCreateConfig(dpy, VAProfileH264ConstrainedBaseline, entrypoint, num_attribs > 0 ? attribs : nullptr, num_attribs, &cfg);
            }
        }
    }

    if (st != VA_STATUS_SUCCESS || cfg == VA_INVALID_ID) {
        return MediaError::HardwareError;
    }
    return static_cast<uint32_t>(cfg);
#else
    (void)codec; (void)is_encoder; (void)rate_control;
    return MediaError::NotImplemented;
#endif
}

MediaResult<void> VaapiDevice::destroy_config(uint32_t config_id) {
#if defined(NISABA_HAS_VAAPI_HEADERS)
    if (!is_available() || !g_va_syms.vaDestroyConfig) return MediaError::HardwareError;
    VADisplay dpy = reinterpret_cast<VADisplay>(va_display_);
    VAStatus st = g_va_syms.vaDestroyConfig(dpy, static_cast<VAConfigID>(config_id));
    if (st != VA_STATUS_SUCCESS) return MediaError::HardwareError;
    return MediaResult<void>::ok();
#else
    (void)config_id;
    return MediaError::NotImplemented;
#endif
}

MediaResult<std::vector<uint32_t>> VaapiDevice::create_surfaces(uint32_t width, uint32_t height, PixelFormat format, uint32_t count) {
#if defined(NISABA_HAS_VAAPI_HEADERS)
    if (!is_available() || !g_va_syms.vaCreateSurfaces || count == 0) return MediaError::HardwareError;

    VADisplay dpy = reinterpret_cast<VADisplay>(va_display_);
    unsigned int rt_format = VA_RT_FORMAT_YUV420;
    if (format == PixelFormat::P010 || format == PixelFormat::YUV420P10) {
        rt_format = VA_RT_FORMAT_YUV420_10;
    }

    std::vector<VASurfaceID> va_surfaces(count, VA_INVALID_ID);
    VAStatus st = g_va_syms.vaCreateSurfaces(
        dpy, rt_format, width, height,
        va_surfaces.data(), count,
        nullptr, 0
    );

    if (st != VA_STATUS_SUCCESS) {
        return MediaError::HardwareError;
    }

    std::vector<uint32_t> result;
    result.reserve(count);
    for (auto s : va_surfaces) {
        result.push_back(static_cast<uint32_t>(s));
    }
    return result;
#else
    (void)width; (void)height; (void)format; (void)count;
    return MediaError::NotImplemented;
#endif
}

MediaResult<void> VaapiDevice::destroy_surfaces(const std::vector<uint32_t>& surfaces) {
#if defined(NISABA_HAS_VAAPI_HEADERS)
    if (!is_available() || !g_va_syms.vaDestroySurfaces || surfaces.empty()) return MediaError::HardwareError;
    VADisplay dpy = reinterpret_cast<VADisplay>(va_display_);

    std::vector<VASurfaceID> va_surfaces;
    va_surfaces.reserve(surfaces.size());
    for (auto s : surfaces) va_surfaces.push_back(static_cast<VASurfaceID>(s));

    VAStatus st = g_va_syms.vaDestroySurfaces(dpy, va_surfaces.data(), static_cast<int>(va_surfaces.size()));
    if (st != VA_STATUS_SUCCESS) return MediaError::HardwareError;
    return MediaResult<void>::ok();
#else
    (void)surfaces;
    return MediaError::NotImplemented;
#endif
}

MediaResult<uint32_t> VaapiDevice::create_context(uint32_t config_id, uint32_t width, uint32_t height, const std::vector<uint32_t>& surfaces) {
#if defined(NISABA_HAS_VAAPI_HEADERS)
    if (!is_available() || !g_va_syms.vaCreateContext) return MediaError::HardwareError;
    VADisplay dpy = reinterpret_cast<VADisplay>(va_display_);

    std::vector<VASurfaceID> va_surfaces;
    va_surfaces.reserve(surfaces.size());
    for (auto s : surfaces) va_surfaces.push_back(static_cast<VASurfaceID>(s));

    VAContextID ctx = VA_INVALID_ID;
    VAStatus st = g_va_syms.vaCreateContext(
        dpy,
        static_cast<VAConfigID>(config_id),
        static_cast<int>(width),
        static_cast<int>(height),
        VA_PROGRESSIVE,
        va_surfaces.empty() ? nullptr : va_surfaces.data(),
        static_cast<int>(va_surfaces.size()),
        &ctx
    );

    if (st != VA_STATUS_SUCCESS || ctx == VA_INVALID_ID) {
        return MediaError::HardwareError;
    }
    return static_cast<uint32_t>(ctx);
#else
    (void)config_id; (void)width; (void)height; (void)surfaces;
    return MediaError::NotImplemented;
#endif
}

MediaResult<void> VaapiDevice::destroy_context(uint32_t context_id) {
#if defined(NISABA_HAS_VAAPI_HEADERS)
    if (!is_available() || !g_va_syms.vaDestroyContext) return MediaError::HardwareError;
    VADisplay dpy = reinterpret_cast<VADisplay>(va_display_);
    VAStatus st = g_va_syms.vaDestroyContext(dpy, static_cast<VAContextID>(context_id));
    if (st != VA_STATUS_SUCCESS) return MediaError::HardwareError;
    return MediaResult<void>::ok();
#else
    (void)context_id;
    return MediaError::NotImplemented;
#endif
}

MediaResult<uint32_t> VaapiDevice::create_buffer(uint32_t context_id, int buffer_type, uint32_t size, uint32_t num_elements, const void* data) {
#if defined(NISABA_HAS_VAAPI_HEADERS)
    if (!is_available() || !g_va_syms.vaCreateBuffer) return MediaError::HardwareError;
    VADisplay dpy = reinterpret_cast<VADisplay>(va_display_);

    VABufferID buf_id = VA_INVALID_ID;
    VAStatus st = g_va_syms.vaCreateBuffer(
        dpy,
        static_cast<VAContextID>(context_id),
        static_cast<VABufferType>(buffer_type),
        size,
        num_elements,
        const_cast<void*>(data),
        &buf_id
    );

    if (st != VA_STATUS_SUCCESS || buf_id == VA_INVALID_ID) {
        return MediaError::HardwareError;
    }
    return static_cast<uint32_t>(buf_id);
#else
    (void)context_id; (void)buffer_type; (void)size; (void)num_elements; (void)data;
    return MediaError::NotImplemented;
#endif
}

MediaResult<void> VaapiDevice::destroy_buffer(uint32_t buffer_id) {
#if defined(NISABA_HAS_VAAPI_HEADERS)
    if (!is_available() || !g_va_syms.vaDestroyBuffer) return MediaError::HardwareError;
    VADisplay dpy = reinterpret_cast<VADisplay>(va_display_);
    VAStatus st = g_va_syms.vaDestroyBuffer(dpy, static_cast<VABufferID>(buffer_id));
    if (st != VA_STATUS_SUCCESS) return MediaError::HardwareError;
    return MediaResult<void>::ok();
#else
    (void)buffer_id;
    return MediaError::NotImplemented;
#endif
}

MediaResult<uint32_t> VaapiDevice::create_coded_buffer(uint32_t context_id, uint32_t max_size) {
#if defined(NISABA_HAS_VAAPI_HEADERS)
    return create_buffer(context_id, VAEncCodedBufferType, max_size, 1, nullptr);
#else
    (void)context_id; (void)max_size;
    return MediaError::NotImplemented;
#endif
}

MediaResult<void*> VaapiDevice::map_buffer(uint32_t buffer_id) {
#if defined(NISABA_HAS_VAAPI_HEADERS)
    if (!is_available() || !g_va_syms.vaMapBuffer || buffer_id == 0xFFFFFFFF) return MediaError::HardwareError;
    VADisplay dpy = reinterpret_cast<VADisplay>(va_display_);
    void* pbuf = nullptr;
    VAStatus st = g_va_syms.vaMapBuffer(dpy, static_cast<VABufferID>(buffer_id), &pbuf);
    if (st != VA_STATUS_SUCCESS || !pbuf) return MediaError::HardwareError;
    return pbuf;
#else
    (void)buffer_id;
    return MediaError::NotImplemented;
#endif
}

MediaResult<void> VaapiDevice::unmap_buffer(uint32_t buffer_id) {
#if defined(NISABA_HAS_VAAPI_HEADERS)
    if (!is_available() || !g_va_syms.vaUnmapBuffer || buffer_id == 0xFFFFFFFF) return MediaError::HardwareError;
    VADisplay dpy = reinterpret_cast<VADisplay>(va_display_);
    VAStatus st = g_va_syms.vaUnmapBuffer(dpy, static_cast<VABufferID>(buffer_id));
    if (st != VA_STATUS_SUCCESS) return MediaError::HardwareError;
    return MediaResult<void>::ok();
#else
    (void)buffer_id;
    return MediaError::NotImplemented;
#endif
}

MediaResult<void> VaapiDevice::read_coded_buffer(uint32_t coded_buf_id, std::vector<uint8_t>& out_bytes) {
#if defined(NISABA_HAS_VAAPI_HEADERS)
    auto map_res = map_buffer(coded_buf_id);
    if (!map_res.is_ok()) return map_res.error();

    auto* seg = reinterpret_cast<VACodedBufferSegment*>(*map_res);
    while (seg) {
        if (seg->buf && seg->size > 0) {
            const auto* data = reinterpret_cast<const uint8_t*>(seg->buf);
            out_bytes.insert(out_bytes.end(), data, data + seg->size);
        }
        seg = reinterpret_cast<VACodedBufferSegment*>(seg->next);
    }

    (void)unmap_buffer(coded_buf_id);
    return MediaResult<void>::ok();
#else
    (void)coded_buf_id; (void)out_bytes;
    return MediaError::NotImplemented;
#endif
}

MediaResult<void> VaapiDevice::upload_frame_to_surface(uint32_t surface_id, const VideoFrame& frame) {
#if defined(NISABA_HAS_VAAPI_HEADERS)
    if (!is_available() || surface_id == 0xFFFFFFFF || !frame.is_valid()) return MediaError::InvalidData;
    VADisplay dpy = reinterpret_cast<VADisplay>(va_display_);

    if (g_va_syms.vaCreateImage && g_va_syms.vaMapBuffer && g_va_syms.vaUnmapBuffer &&
        g_va_syms.vaPutImage && g_va_syms.vaDestroyImage) {
        VAImageFormat fmt{};
        fmt.fourcc = VA_FOURCC_NV12;
        fmt.byte_order = VA_LSB_FIRST;
        fmt.bits_per_pixel = 12;

        VAImage img{};
        VAStatus st = g_va_syms.vaCreateImage(dpy, &fmt, frame.width(), frame.height(), &img);
        if (st == VA_STATUS_SUCCESS) {
            void* pbuf = nullptr;
            st = g_va_syms.vaMapBuffer(dpy, img.buf, &pbuf);
            if (st == VA_STATUS_SUCCESS && pbuf) {
                uint8_t* dst_base = reinterpret_cast<uint8_t*>(pbuf);
                uint32_t w = frame.width();
                uint32_t h = frame.height();

                if (frame.pixel_format() == PixelFormat::NV12) {
                    for (uint32_t p = 0; p < std::min(size_t{2}, frame.plane_count()); ++p) {
                        const uint8_t* src = frame.plane(p);
                        uint8_t* dst = dst_base + img.offsets[p];
                        size_t src_stride = frame.stride(p);
                        size_t dst_stride = img.pitches[p];
                        uint32_t plane_h = (p == 0) ? h : (h / 2);
                        size_t row_bytes = std::min(src_stride, dst_stride);
                        for (uint32_t y = 0; y < plane_h; ++y) {
                            std::memcpy(dst + y * dst_stride, src + y * src_stride, row_bytes);
                        }
                    }
                } else if (frame.pixel_format() == PixelFormat::YUV420P) {
                    // Copy Y
                    const uint8_t* src_y = frame.plane(0);
                    uint8_t* dst_y = dst_base + img.offsets[0];
                    size_t src_y_stride = frame.stride(0);
                    size_t dst_y_stride = img.pitches[0];
                    for (uint32_t y = 0; y < h; ++y) {
                        std::memcpy(dst_y + y * dst_y_stride, src_y + y * src_y_stride, std::min(static_cast<size_t>(w), dst_y_stride));
                    }
                    // Interleave U and V into UV
                    const uint8_t* src_u = frame.plane(1);
                    const uint8_t* src_v = frame.plane(2);
                    uint8_t* dst_uv = dst_base + img.offsets[1];
                    size_t src_u_stride = frame.stride(1);
                    size_t src_v_stride = frame.stride(2);
                    size_t dst_uv_stride = img.pitches[1];
                    for (uint32_t y = 0; y < h / 2; ++y) {
                        for (uint32_t x = 0; x < w / 2; ++x) {
                            dst_uv[y * dst_uv_stride + x * 2 + 0] = src_u[y * src_u_stride + x];
                            dst_uv[y * dst_uv_stride + x * 2 + 1] = src_v[y * src_v_stride + x];
                        }
                    }
                }

                g_va_syms.vaUnmapBuffer(dpy, img.buf);
                g_va_syms.vaPutImage(dpy, static_cast<VASurfaceID>(surface_id), img.image_id, 0, 0, w, h, 0, 0, w, h);
            }
            g_va_syms.vaDestroyImage(dpy, img.image_id);
            return MediaResult<void>::ok();
        }
    }
    return MediaError::HardwareError;
#else
    (void)surface_id; (void)frame;
    return MediaError::NotImplemented;
#endif
}

MediaResult<void> VaapiDevice::begin_picture(uint32_t context_id, uint32_t surface_id) {
#if defined(NISABA_HAS_VAAPI_HEADERS)
    if (!is_available() || !g_va_syms.vaBeginPicture) return MediaError::HardwareError;
    VADisplay dpy = reinterpret_cast<VADisplay>(va_display_);
    VAStatus st = g_va_syms.vaBeginPicture(dpy, static_cast<VAContextID>(context_id), static_cast<VASurfaceID>(surface_id));
    if (st != VA_STATUS_SUCCESS) return MediaError::HardwareError;
    return MediaResult<void>::ok();
#else
    (void)context_id; (void)surface_id;
    return MediaError::NotImplemented;
#endif
}

MediaResult<void> VaapiDevice::render_picture(uint32_t context_id, const uint32_t* buffers, uint32_t num_buffers) {
#if defined(NISABA_HAS_VAAPI_HEADERS)
    if (!is_available() || !g_va_syms.vaRenderPicture || !buffers || num_buffers == 0) return MediaError::HardwareError;
    VADisplay dpy = reinterpret_cast<VADisplay>(va_display_);

    std::vector<VABufferID> va_bufs;
    va_bufs.reserve(num_buffers);
    for (uint32_t i = 0; i < num_buffers; ++i) va_bufs.push_back(static_cast<VABufferID>(buffers[i]));

    VAStatus st = g_va_syms.vaRenderPicture(dpy, static_cast<VAContextID>(context_id), va_bufs.data(), static_cast<int>(num_buffers));
    if (st != VA_STATUS_SUCCESS) return MediaError::HardwareError;
    return MediaResult<void>::ok();
#else
    (void)context_id; (void)buffers; (void)num_buffers;
    return MediaError::NotImplemented;
#endif
}

MediaResult<void> VaapiDevice::end_picture(uint32_t context_id) {
#if defined(NISABA_HAS_VAAPI_HEADERS)
    if (!is_available() || !g_va_syms.vaEndPicture) return MediaError::HardwareError;
    VADisplay dpy = reinterpret_cast<VADisplay>(va_display_);
    VAStatus st = g_va_syms.vaEndPicture(dpy, static_cast<VAContextID>(context_id));
    if (st != VA_STATUS_SUCCESS) return MediaError::HardwareError;
    return MediaResult<void>::ok();
#else
    (void)context_id;
    return MediaError::NotImplemented;
#endif
}

MediaResult<void> VaapiDevice::sync_surface(uint32_t surface_id) {
#if defined(NISABA_HAS_VAAPI_HEADERS)
    if (!is_available() || !g_va_syms.vaSyncSurface) return MediaError::HardwareError;
    VADisplay dpy = reinterpret_cast<VADisplay>(va_display_);
    VAStatus st = g_va_syms.vaSyncSurface(dpy, static_cast<VASurfaceID>(surface_id));
    if (st != VA_STATUS_SUCCESS) {
        std::cerr << "vaSyncSurface failed on surface " << surface_id << ": " << st << " ("
                  << (g_va_syms.vaErrorStr ? g_va_syms.vaErrorStr(st) : "unknown") << ")" << std::endl;
        return MediaError::HardwareError;
    }
    return MediaResult<void>::ok();
#else
    (void)surface_id;
    return MediaError::NotImplemented;
#endif
}

MediaResult<void> VaapiDevice::export_surface_dma_buf(uint32_t surface_id, HwBufferDescriptor& desc) {
#if defined(NISABA_HAS_VAAPI_HEADERS)
    if (!is_available() || surface_id == 0xFFFFFFFF) return MediaError::InvalidData;
    VADisplay dpy = reinterpret_cast<VADisplay>(va_display_);

    if (g_va_syms.vaExportSurfaceHandle) {
#if defined(VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2)
        VADRMPRIMESurfaceDescriptor prime_desc{};
        VAStatus st = g_va_syms.vaExportSurfaceHandle(
            dpy,
            static_cast<VASurfaceID>(surface_id),
            VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
            VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS,
            &prime_desc
        );
        if (st == VA_STATUS_SUCCESS) {
            desc = HwBufferDescriptor{};
            desc.width = prime_desc.width;
            desc.height = prime_desc.height;
            desc.drm_fourcc = prime_desc.fourcc;

            uint32_t obj_count = std::min(prime_desc.num_objects, 4u);
            for (uint32_t i = 0; i < obj_count; ++i) {
                desc.fds[i] = prime_desc.objects[i].fd;
            }
            if (obj_count > 0) {
                desc.modifier = prime_desc.objects[0].drm_format_modifier;
            }

            uint32_t plane_idx = 0;
            uint32_t layer_count = std::min(prime_desc.num_layers, 4u);
            for (uint32_t l = 0; l < layer_count && plane_idx < 4; ++l) {
                uint32_t lp_count = std::min(prime_desc.layers[l].num_planes, 4u);
                for (uint32_t p = 0; p < lp_count && plane_idx < 4; ++p) {
                    desc.strides[plane_idx] = prime_desc.layers[l].pitch[p];
                    desc.offsets[plane_idx] = prime_desc.layers[l].offset[p];
                    uint32_t obj_idx = prime_desc.layers[l].object_index[p];
                    if (obj_idx >= obj_count) obj_idx = 0;
                    desc.object_indices[plane_idx] = obj_idx;
                    desc.modifiers[plane_idx] = (obj_idx < obj_count)
                        ? prime_desc.objects[obj_idx].drm_format_modifier
                        : desc.modifier;
                    ++plane_idx;
                }
            }
            desc.num_planes = plane_idx;
            return MediaResult<void>::ok();
        }
#endif
    }
#else
    (void)surface_id; (void)desc;
#endif
    return MediaError::HardwareError;
}

MediaResult<VideoFrame> VaapiDevice::derive_surface_to_frame(uint32_t surface_id, uint32_t width, uint32_t height, PixelFormat preferred_format) {
    if (!is_available() || surface_id == 0xFFFFFFFF) return MediaError::InvalidData;

#if defined(NISABA_HAS_VAAPI_HEADERS)
    VADisplay dpy = reinterpret_cast<VADisplay>(va_display_);

    bool got_pixels = false;
    VideoFrame frame;

    auto copy_plane_rows = [](const uint8_t* src_plane, size_t src_stride, uint8_t* dst_plane, size_t dst_stride, uint32_t plane_h) {
        size_t copy_bytes = std::min(src_stride, dst_stride);
        if (src_stride == dst_stride && copy_bytes == src_stride) {
            std::memcpy(dst_plane, src_plane, src_stride * plane_h);
        } else {
            for (uint32_t y = 0; y < plane_h; ++y) {
                std::memcpy(dst_plane + y * dst_stride, src_plane + y * src_stride, copy_bytes);
            }
        }
    };

    // Strategy 1: vaGetImage (hardware driver untiles GPU surface into clean linear memory)
    if (!got_pixels && g_va_syms.vaCreateImage && g_va_syms.vaGetImage && g_va_syms.vaMapBuffer && g_va_syms.vaUnmapBuffer && g_va_syms.vaDestroyImage) {
        const bool prefer_10bit = (preferred_format == PixelFormat::P010 || preferred_format == PixelFormat::YUV420P10);
        VAImageFormat img_fmt{};
        img_fmt.fourcc = prefer_10bit ? VA_FOURCC_P010 : VA_FOURCC_NV12;
        img_fmt.byte_order = VA_LSB_FIRST;
        img_fmt.bits_per_pixel = prefer_10bit ? 24 : 12;
        PixelFormat target_fmt = prefer_10bit ? PixelFormat::P010 : PixelFormat::NV12;

        VAImage img{};
        VAStatus st = g_va_syms.vaCreateImage(dpy, &img_fmt, width, height, &img);
        if (st != VA_STATUS_SUCCESS && prefer_10bit) {
            img_fmt.fourcc = VA_FOURCC_NV12;
            img_fmt.bits_per_pixel = 12;
            target_fmt = PixelFormat::NV12;
            st = g_va_syms.vaCreateImage(dpy, &img_fmt, width, height, &img);
        }
        if (st == VA_STATUS_SUCCESS) {
            st = g_va_syms.vaGetImage(dpy, static_cast<VASurfaceID>(surface_id), 0, 0, width, height, img.image_id);
            if (st == VA_STATUS_SUCCESS) {
                void* pbuf = nullptr;
                st = g_va_syms.vaMapBuffer(dpy, img.buf, &pbuf);
                if (st == VA_STATUS_SUCCESS && pbuf) {
                    frame = VideoFrame::allocate(width, height, target_fmt);
                    const uint8_t* src_base = reinterpret_cast<const uint8_t*>(pbuf);

                    for (uint32_t i = 0; i < img.num_planes && i < frame.plane_count(); ++i) {
                        copy_plane_rows(
                            src_base + img.offsets[i], img.pitches[i],
                            frame.plane_mut(i), frame.stride(i),
                            frame.plane_height(i)
                        );
                    }

                    g_va_syms.vaUnmapBuffer(dpy, img.buf);
                    got_pixels = true;
                }
            }
            g_va_syms.vaDestroyImage(dpy, img.image_id);
        }
    }

    // Strategy 2: Fast direct derivation fallback
    if (!got_pixels && g_va_syms.vaDeriveImage && g_va_syms.vaMapBuffer && g_va_syms.vaUnmapBuffer && g_va_syms.vaDestroyImage) {
        VAImage image{};
        VAStatus st = g_va_syms.vaDeriveImage(dpy, static_cast<VASurfaceID>(surface_id), &image);
        if (st == VA_STATUS_SUCCESS) {
            void* pbuf = nullptr;
            st = g_va_syms.vaMapBuffer(dpy, image.buf, &pbuf);
            if (st == VA_STATUS_SUCCESS && pbuf) {
                PixelFormat fmt = PixelFormat::NV12;
                if (image.format.fourcc == VA_FOURCC_YV12) fmt = PixelFormat::YUV420P;
                else if (image.format.fourcc == VA_FOURCC_P010) fmt = PixelFormat::P010;

                frame = VideoFrame::allocate(width, height, fmt);
                const uint8_t* src_base = reinterpret_cast<const uint8_t*>(pbuf);

                for (uint32_t i = 0; i < image.num_planes && i < frame.plane_count(); ++i) {
                    copy_plane_rows(
                        src_base + image.offsets[i], image.pitches[i],
                        frame.plane_mut(i), frame.stride(i),
                        frame.plane_height(i)
                    );
                }

                g_va_syms.vaUnmapBuffer(dpy, image.buf);
                g_va_syms.vaDestroyImage(dpy, image.image_id);
                got_pixels = true;
            } else {
                g_va_syms.vaDestroyImage(dpy, image.image_id);
            }
        }
    }

    if (!got_pixels) {
        frame = VideoFrame::allocate(width, height, preferred_format == PixelFormat::P010 ? PixelFormat::P010 : PixelFormat::NV12);
    }

    frame.set_hw_frame(true);
    frame.set_hw_surface_id(surface_id);
    return frame;
#else
    (void)width; (void)height; (void)preferred_format;
    return MediaError::NotImplemented;
#endif
}

MediaResult<VideoFrame> VaapiDevice::create_lazy_hw_frame(
    uint32_t surface_id,
    uint32_t width,
    uint32_t height,
    PixelFormat format,
    std::shared_ptr<void> surface_lease
) {
    if (!is_available() || surface_id == 0xFFFFFFFF || width == 0 || height == 0) {
        return MediaError::InvalidData;
    }
#if defined(NISABA_HAS_VAAPI_HEADERS)
    auto backing = std::make_shared<VaSurfaceBacking>(
        this, lifetime_, surface_id, width, height, format, std::move(surface_lease)
    );
    return VideoFrame::from_hw_backing(width, height, format, std::move(backing));
#else
    (void)format; (void)surface_lease;
    return MediaError::NotImplemented;
#endif
}

MediaResult<void> VaapiDevice::export_buffer(const VideoFrame& frame, HwBufferDescriptor& desc) {
    if (!frame.is_valid()) return MediaError::InvalidData;

    if (const auto& backing = frame.hw_backing()) {
        if (const HwFrameBufferLayout* layout = backing->export_dma_buf(true)) {
            desc = HwBufferDescriptor{};
            desc.width = layout->width;
            desc.height = layout->height;
            desc.drm_fourcc = layout->drm_fourcc;
            desc.num_planes = layout->num_planes;
            for (size_t i = 0; i < 4; ++i) {
                desc.fds[i] = layout->fds[i];
                desc.strides[i] = layout->strides[i];
                desc.offsets[i] = layout->offsets[i];
                desc.modifiers[i] = layout->modifiers[i];
                desc.object_indices[i] = layout->object_indices[i];
            }
            desc.modifier = layout->modifier;
            return MediaResult<void>::ok();
        }
    }

    uint32_t sid = frame.hw_surface_id();
    if (sid != 0xFFFFFFFF) {
        if (export_surface_dma_buf(sid, desc).is_ok()) {
            return MediaResult<void>::ok();
        }
    }

    desc.width = frame.width();
    desc.height = frame.height();
    desc.num_planes = static_cast<uint32_t>(frame.plane_count());
    if (frame.dma_buf_fd() >= 0) {
        desc.fds[0] = frame.dma_buf_fd();
    }
    for (size_t i = 0; i < frame.plane_count() && i < 4; ++i) {
        desc.strides[i] = static_cast<uint32_t>(frame.stride(i));
        desc.offsets[i] = 0;
    }
    return MediaResult<void>::ok();
}

MediaResult<VideoFrame> VaapiDevice::import_buffer(const HwBufferDescriptor& desc) {
    if (desc.width == 0 || desc.height == 0) return MediaError::InvalidData;
    VideoFrame frame = VideoFrame::allocate(desc.width, desc.height, PixelFormat::NV12);
    frame.set_hw_frame(true);
    if (desc.fds[0] >= 0) {
        frame.set_dma_buf_fd(desc.fds[0]);
    }
    return frame;
}

std::unique_ptr<VaapiDevice> VaapiDevice::create() {
    auto dev = std::make_unique<VaapiDevice>();
    return dev;
}

} // namespace atum
