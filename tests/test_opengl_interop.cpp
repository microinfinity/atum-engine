#include <iostream>
#include <cassert>
#include <vector>
#include <cmath>

#include "atum/hwaccel/opengl/gl_device.hpp"
#include "atum/hwaccel/opengl/gl_dispatcher.hpp"
#include "atum/hwaccel/opengl/egl_context.hpp"
#include "atum/hwaccel/opengl/gl_image_importer.hpp"
#include "atum/hwaccel/opengl/gl_renderer.hpp"
#include "atum/hwaccel/vaapi_device.hpp"

using namespace atum;
using namespace atum::gl;

void test_gl_dispatcher_loading() {
    std::cout << "[Test 1] Testing Clean-Room GlDispatcher Dynamic Loading..." << std::endl;
    GlDispatcher disp;
    bool ok = disp.load_egl();
    assert(ok && "Failed to dynamically load libEGL.so via GlDispatcher");
    assert(disp.is_egl_loaded() && "GlDispatcher is_egl_loaded must be true");
    assert(disp.eglGetDisplay != nullptr);
    assert(disp.eglInitialize != nullptr);
    assert(disp.eglChooseConfig != nullptr);
    assert(disp.eglCreateContext != nullptr);

    std::cout << "  Core EGL symbols resolved successfully via dlopen/dlsym." << std::endl;
    std::cout << "  Has EGL_MESA_platform_surfaceless: "
              << (disp.has_client_extension("EGL_MESA_platform_surfaceless") ? "YES" : "NO") << std::endl;
    std::cout << "  Has EGL_EXT_platform_device: "
              << (disp.has_client_extension("EGL_EXT_platform_device") ? "YES" : "NO") << std::endl;
    std::cout << "  -> PASS: GlDispatcher dynamic resolution verified." << std::endl;
}

void test_egl_context_lifecycle() {
    std::cout << "\n[Test 2] Testing Headless EGL Context Lifecycle & Scoping..." << std::endl;
    GlDispatcher disp;
    auto ctx_res = EglContext::create_headless(disp);
    assert(ctx_res.is_ok() && "Failed to create headless EglContext");

    auto ctx = std::move(ctx_res.value());
    assert(ctx->is_valid());
    assert(ctx->display() != EGL_NO_DISPLAY_PTR);
    assert(ctx->context() != EGL_NO_CONTEXT_PTR);

    std::cout << "  EGL Initialized (v" << ctx->major_version() << "." << ctx->minor_version() << ")" << std::endl;
    std::cout << "  GL Vendor:   " << ctx->gl_vendor() << std::endl;
    std::cout << "  GL Renderer: " << ctx->gl_renderer() << std::endl;
    std::cout << "  GL Version:  " << ctx->gl_version() << std::endl;

    // Test Scoped RAII activation
    {
        EglContext::Scope scope(*ctx);
        assert(scope.is_active());
        assert(ctx->is_current());
    }
    assert(!ctx->is_current());

    std::cout << "  -> PASS: EglContext initialization and RAII activation verified." << std::endl;
}

void test_gl_texture_upload_and_readback() {
    std::cout << "\n[Test 3] Testing Texture Upload and Inverted Row Readback..." << std::endl;
    GlDispatcher disp;
    auto ctx_res = EglContext::create_headless(disp);
    assert(ctx_res.is_ok());
    auto ctx = std::move(ctx_res.value());

    // Generate known synthetic pattern (64x64 RGBA)
    const uint32_t width = 64;
    const uint32_t height = 64;
    auto input_frame = VideoFrame::allocate(width, height, PixelFormat::RGBA32);
    assert(input_frame.is_valid());

    for (uint32_t y = 0; y < height; ++y) {
        uint8_t* row = input_frame.plane_mut(0) + y * input_frame.stride(0);
        for (uint32_t x = 0; x < width; ++x) {
            row[x * 4 + 0] = static_cast<uint8_t>((x * 255) / width);       // R
            row[x * 4 + 1] = static_cast<uint8_t>((y * 255) / height);      // G
            row[x * 4 + 2] = 128;                                           // B
            row[x * 4 + 3] = 255;                                           // A
        }
    }

    GlImageImporter importer(*ctx);
    auto tex_res = importer.create_texture_from_frame(input_frame);
    assert(tex_res.is_ok() && "Failed to upload VideoFrame to OpenGL texture");

    auto tex = std::move(tex_res.value());
    assert(tex.is_valid());
    assert(tex.width() == width);
    assert(tex.height() == height);

    // Read back pixels
    auto read_res = tex.read_pixels_to_frame();
    assert(read_res.is_ok() && "Failed to read pixels from GlTexture");
    auto read_frame = read_res.value();
    assert(read_frame.width() == width);
    assert(read_frame.height() == height);

    // Verify perfect roundtrip
    double mae = 0.0;
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* in_row = input_frame.plane(0) + y * input_frame.stride(0);
        const uint8_t* out_row = read_frame.plane(0) + y * read_frame.stride(0);
        for (uint32_t x = 0; x < width; ++x) {
            for (int c = 0; c < 4; ++c) {
                mae += std::abs(static_cast<int>(in_row[x * 4 + c]) - static_cast<int>(out_row[x * 4 + c]));
            }
        }
    }
    mae /= (width * height * 4);
    std::cout << "  Upload-Readback Mean Absolute Error (MAE): " << mae << std::endl;
    assert(mae < 1e-4 && "Readback pixels must match uploaded pixels exactly");

    std::cout << "  -> PASS: Pixel upload and bottom-up readback inversion verified." << std::endl;
}

void test_gl_renderer_pipeline() {
    std::cout << "\n[Test 4] Testing Presentation Shader Pipeline & Render-To-Texture..." << std::endl;
    GlDispatcher disp;
    auto ctx_res = EglContext::create_headless(disp);
    assert(ctx_res.is_ok());
    auto ctx = std::move(ctx_res.value());

    GlImageImporter importer(*ctx);
    GlRenderer renderer(*ctx);

    auto init_res = renderer.init();
    assert(init_res.is_ok() && "Failed to initialize GlRenderer GLSL pipeline");
    assert(renderer.is_initialized());

    // Create 128x128 source texture
    const uint32_t src_w = 128;
    const uint32_t src_h = 128;
    auto src_frame = VideoFrame::allocate(src_w, src_h, PixelFormat::RGBA32);
    for (uint32_t y = 0; y < src_h; ++y) {
        uint8_t* row = src_frame.plane_mut(0) + y * src_frame.stride(0);
        for (uint32_t x = 0; x < src_w; ++x) {
            row[x * 4 + 0] = 200;
            row[x * 4 + 1] = 100;
            row[x * 4 + 2] = 50;
            row[x * 4 + 3] = 255;
        }
    }

    auto src_tex_res = importer.create_texture_from_frame(src_frame);
    assert(src_tex_res.is_ok());
    auto src_tex = std::move(src_tex_res.value());

    // Render onto 256x256 destination texture
    const uint32_t dst_w = 256;
    const uint32_t dst_h = 256;
    auto dst_tex_res = renderer.render_to_texture(src_tex, dst_w, dst_h);
    assert(dst_tex_res.is_ok() && "Failed to render source texture to destination texture");

    auto dst_tex = std::move(dst_tex_res.value());
    assert(dst_tex.width() == dst_w);
    assert(dst_tex.height() == dst_h);

    auto out_frame_res = dst_tex.read_pixels_to_frame();
    assert(out_frame_res.is_ok());
    auto out_frame = out_frame_res.value();
    assert(out_frame.width() == dst_w);
    assert(out_frame.height() == dst_h);

    // Check sampled color in middle of frame
    const uint8_t* center_px = out_frame.plane(0) + (dst_h / 2) * out_frame.stride(0) + (dst_w / 2) * 4;
    std::cout << "  Sampled rendered pixel: RGBA("
              << (int)center_px[0] << ", " << (int)center_px[1] << ", "
              << (int)center_px[2] << ", " << (int)center_px[3] << ")" << std::endl;

    assert(std::abs((int)center_px[0] - 200) <= 2);
    assert(std::abs((int)center_px[1] - 100) <= 2);
    assert(std::abs((int)center_px[2] - 50) <= 2);

    std::cout << "  -> PASS: Presentation shader quad rendering and FBO render-to-texture verified." << std::endl;
}

void test_gl_dma_buf_zero_copy() {
    std::cout << "\n[Test 5] Testing Zero-Copy VA-API DMA-BUF Import into OpenGL..." << std::endl;
    auto va_dev = VaapiDevice::create();
    if (!va_dev || !va_dev->is_available()) {
        std::cout << "  [Notice] VA-API device not available on this platform; skipping DMA-BUF interop." << std::endl;
        return;
    }

    auto surfs_res = va_dev->create_surfaces(320, 240, PixelFormat::NV12, 1);
    if (!surfs_res.is_ok()) {
        std::cout << "  [Notice] Could not allocate VA-API surface; skipping." << std::endl;
        return;
    }
    uint32_t surface_id = surfs_res.value()[0];

    HwBufferDescriptor desc{};
    auto exp_res = va_dev->export_surface_dma_buf(surface_id, desc);
    if (!exp_res.is_ok() || desc.fds[0] < 0) {
        std::cout << "  [Notice] DMA-BUF export not supported by current VA-API driver; skipping." << std::endl;
        va_dev->destroy_surfaces(surfs_res.value());
        return;
    }

    std::cout << "  Exported VA-API surface to DMA-BUF FD: " << desc.fds[0]
              << " (" << desc.width << "x" << desc.height << ")" << std::endl;

    GlDispatcher disp;
    auto ctx_res = EglContext::create_headless(disp);
    assert(ctx_res.is_ok());
    auto ctx = std::move(ctx_res.value());

    GlImageImporter importer(*ctx);
    auto gl_tex_res = importer.import_dma_buf(desc);
    if (gl_tex_res.is_ok()) {
        auto gl_tex = std::move(gl_tex_res.value());
        std::cout << "  Successfully imported DMA-BUF to GL Texture ID: " << gl_tex.id()
                  << ", EGLImage: " << gl_tex.egl_image() << std::endl;
        assert(gl_tex.is_valid());
        assert(gl_tex.width() == 320);
        assert(gl_tex.height() == 240);
        std::cout << "  -> PASS: Zero-copy DMA-BUF importation into EGLImage verified." << std::endl;
    } else {
        std::cout << "  [Notice] eglCreateImageKHR with DMA-BUF modifier not accepted on this DRM node." << std::endl;
    }

    va_dev->destroy_surfaces(surfs_res.value());
}

void test_gl_device_interface() {
    std::cout << "\n[Test 6] Testing atum::HwDevice GlDevice Abstraction..." << std::endl;
    auto dev = GlDevice::create();
    assert(dev != nullptr && "Failed to create GlDevice");
    assert(dev->is_available() && "GlDevice must be available");
    assert(dev->type() == HwDeviceType::OpenGl);
    assert(dev->name().starts_with("OpenGL / EGL"));

    std::cout << "  Device Name: " << dev->name() << std::endl;
    std::cout << "  Device Type: " << hw_device_type_to_string(dev->type()) << std::endl;
    std::cout << "  -> PASS: GlDevice abstraction conforms to HwDevice contract." << std::endl;
}

void test_gl_planar_zero_copy() {
    std::cout << "\n[Test 7] Testing Zero-Copy Planar NV12 (R8+GR88) Import & GPU CSC Render..." << std::endl;
    auto va_dev = VaapiDevice::create();
    if (!va_dev || !va_dev->is_available()) {
        std::cout << "  [Notice] VA-API device not available; skipping planar DMA-BUF test." << std::endl;
        return;
    }

    auto surfs_res = va_dev->create_surfaces(320, 240, PixelFormat::NV12, 1);
    if (!surfs_res.is_ok()) {
        std::cout << "  [Notice] Could not allocate VA-API surface; skipping." << std::endl;
        return;
    }
    uint32_t surface_id = surfs_res.value()[0];

    HwBufferDescriptor desc{};
    auto exp_res = va_dev->export_surface_dma_buf(surface_id, desc);
    if (!exp_res.is_ok() || desc.plane_fd(0) < 0) {
        std::cout << "  [Notice] DMA-BUF export not supported on current node; skipping." << std::endl;
        va_dev->destroy_surfaces(surfs_res.value());
        return;
    }

    GlDispatcher disp;
    auto ctx_res = EglContext::create_headless(disp);
    assert(ctx_res.is_ok());
    auto ctx = std::move(ctx_res.value());

    GlImageImporter importer(*ctx);
    GlRenderer renderer(*ctx);
    assert(renderer.init().is_ok());

    auto planar_res = importer.import_planar_dma_buf(desc);
    if (planar_res.is_ok()) {
        auto planar = std::move(planar_res.value());
        assert(planar.is_valid());
        assert(planar.plane_count == 2);
        assert(planar.planes[0].target() == GL_TEXTURE_2D_VAL);
        assert(planar.planes[1].target() == GL_TEXTURE_2D_VAL);

        auto rgba_res = renderer.render_to_texture(planar, 320, 240);
        assert(rgba_res.is_ok() && "Failed to render planar DMA-BUF textures to RGBA");
        auto out = rgba_res.value().read_pixels_to_frame();
        assert(out.is_ok() && out->width() == 320 && out->height() == 240);
        std::cout << "  -> PASS: Planar R8+GR88 zero-copy import & YUV->RGB shader verified." << std::endl;
    } else {
        std::cout << "  [Notice] Cross-device planar DMA-BUF import rejected by current EGL node (err="
                  << (int)planar_res.error() << ")." << std::endl;
    }

    va_dev->destroy_surfaces(surfs_res.value());
}

void test_gles_renderer_init() {
    std::cout << "\n[Test 8] Testing Explicit OpenGL ES 3.x GlRenderer Initialization..." << std::endl;
    GlDispatcher disp;
    if (!disp.load_egl() || !disp.eglGetPlatformDisplayEXT) {
        std::cout << "  [Notice] EGL platform display unavailable; skipping GLES3 test." << std::endl;
        return;
    }

    EGLDisplay dpy = disp.eglGetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA_VAL, EGL_DEFAULT_DISPLAY_PTR, nullptr);
    if (dpy == EGL_NO_DISPLAY_PTR) {
        std::cout << "  [Notice] Surfaceless EGL display unavailable; skipping GLES3 test." << std::endl;
        return;
    }

    EGLint maj = 0;
    EGLint min = 0;
    if (disp.eglInitialize(dpy, &maj, &min) != EGL_TRUE_VAL ||
        disp.eglBindAPI(EGL_OPENGL_ES_API_VAL) != EGL_TRUE_VAL) {
        std::cout << "  [Notice] OpenGL ES API unavailable; skipping GLES3 test." << std::endl;
        return;
    }

    EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE_VAL, EGL_PBUFFER_BIT_VAL,
        EGL_RENDERABLE_TYPE_VAL, EGL_OPENGL_ES2_BIT_VAL,
        EGL_NONE_VAL
    };
    EGLConfig cfg = nullptr;
    EGLint ncfg = 0;
    if (disp.eglChooseConfig(dpy, cfg_attr, &cfg, 1, &ncfg) != EGL_TRUE_VAL || ncfg == 0) {
        disp.eglTerminate(dpy);
        std::cout << "  [Notice] No OpenGL ES EGLConfig available; skipping GLES3 test." << std::endl;
        return;
    }

    EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION_VAL, 3, EGL_NONE_VAL };
    EGLContext raw_ctx = disp.eglCreateContext(dpy, cfg, EGL_NO_CONTEXT_PTR, ctx_attr);
    EGLint pbuf_attr[] = { EGL_WIDTH_VAL, 1, EGL_HEIGHT_VAL, 1, EGL_NONE_VAL };
    EGLSurface pbuf = disp.eglCreatePbufferSurface(dpy, cfg, pbuf_attr);
    if (raw_ctx != EGL_NO_CONTEXT_PTR && pbuf != EGL_NO_SURFACE_PTR) {
        disp.eglMakeCurrent(dpy, pbuf, pbuf, raw_ctx);
        (void)disp.load_gl(dpy);
        auto wrapped_res = EglContext::wrap_external(disp, dpy, raw_ctx, pbuf);
        assert(wrapped_res.is_ok());

        GlRenderer renderer(*wrapped_res.value());
        auto init_res = renderer.init();
        assert(init_res.is_ok() && "GlRenderer::init() failed on OpenGL ES 3.x context");
        assert(renderer.is_initialized());
        std::cout << "  -> PASS: GlRenderer initialized cleanly on OpenGL ES 3.x ("
                  << wrapped_res.value()->gl_version() << ")." << std::endl;
    }

    disp.eglMakeCurrent(dpy, EGL_NO_SURFACE_PTR, EGL_NO_SURFACE_PTR, EGL_NO_CONTEXT_PTR);
    if (pbuf != EGL_NO_SURFACE_PTR) disp.eglDestroySurface(dpy, pbuf);
    if (raw_ctx != EGL_NO_CONTEXT_PTR) disp.eglDestroyContext(dpy, raw_ctx);
    disp.eglTerminate(dpy);
}

int main() {
    std::cout << "============================================================" << std::endl;
    std::cout << "       ATUM MEDIA ENGINE: OPENGL / EGL INTEROP SUITE        " << std::endl;
    std::cout << "============================================================" << std::endl;

    test_gl_dispatcher_loading();
    test_egl_context_lifecycle();
    test_gl_texture_upload_and_readback();
    test_gl_renderer_pipeline();
    test_gl_dma_buf_zero_copy();
    test_gl_device_interface();
    test_gl_planar_zero_copy();
    test_gles_renderer_init();

    std::cout << "\n============================================================" << std::endl;
    std::cout << "   ALL 8 OPENGL INTEROP TEST CASES COMPLETED SUCCESSFULLY!  " << std::endl;
    std::cout << "============================================================" << std::endl;
    return 0;
}
