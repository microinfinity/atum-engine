#include "atum/hwaccel/opengl/gl_renderer.hpp"

#include <initializer_list>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace atum::gl {

namespace {

[[nodiscard]] GLint color_space_to_uniform(ColorSpace cs) noexcept {
    switch (cs) {
        case ColorSpace::BT709: return 1;
        case ColorSpace::BT2020: return 2;
        case ColorSpace::BT601:
        default:
            return 0;
    }
}

[[nodiscard]] std::string make_planar_fragment_shader(const char* prefix, bool use_modern_syntax) {
    std::string fs;
    fs.reserve(1536);
    fs += prefix;
    if (use_modern_syntax) {
        fs += R"(in vec2 vTexCoord;
out vec4 FragColor;
)";
    } else {
        fs += "varying vec2 vTexCoord;\n";
    }
    fs += R"(uniform sampler2D uTexY;
uniform sampler2D uTexU;
uniform sampler2D uTexV;
uniform int uPlaneMode;
uniform int uColorConversion;
uniform int uColorRange;

void main() {
)";
    const char* sample_fn = use_modern_syntax ? "texture" : "texture2D";
    fs += "    float y_raw = ";
    fs += sample_fn;
    fs += "(uTexY, vTexCoord).r;\n";
    fs += "    float u_raw = 0.5;\n";
    fs += "    float v_raw = 0.5;\n";
    fs += "    if (uPlaneMode == 0) {\n";
    fs += "        vec2 uv = ";
    fs += sample_fn;
    fs += "(uTexU, vTexCoord).rg;\n";
    fs += "        u_raw = uv.r;\n        v_raw = uv.g;\n";
    fs += "    } else if (uPlaneMode == 1) {\n";
    fs += "        vec2 uv = ";
    fs += sample_fn;
    fs += "(uTexU, vTexCoord).rg;\n";
    fs += "        u_raw = uv.g;\n        v_raw = uv.r;\n";
    fs += "    } else {\n";
    fs += "        u_raw = ";
    fs += sample_fn;
    fs += "(uTexU, vTexCoord).r;\n";
    fs += "        v_raw = ";
    fs += sample_fn;
    fs += "(uTexV, vTexCoord).r;\n";
    fs += "    }\n";
    fs += R"(    float u = u_raw - (128.0 / 255.0);
    float v = v_raw - (128.0 / 255.0);
    float y = (uColorRange == 1)
        ? (y_raw - (16.0 / 255.0)) * (298.0 / 256.0)
        : y_raw;
    vec3 rgb;
    if (uColorRange == 1) {
        if (uColorConversion == 1) {
            rgb = vec3(
                y + (459.0 / 256.0) * v,
                y - (55.0 / 256.0) * u - (136.0 / 256.0) * v,
                y + (541.0 / 256.0) * u
            );
        } else if (uColorConversion == 2) {
            rgb = vec3(
                y + (430.0 / 256.0) * v,
                y - (48.0 / 256.0) * u - (166.0 / 256.0) * v,
                y + (548.0 / 256.0) * u
            );
        } else {
            rgb = vec3(
                y + (409.0 / 256.0) * v,
                y - (100.0 / 256.0) * u - (208.0 / 256.0) * v,
                y + (516.0 / 256.0) * u
            );
        }
    } else {
        if (uColorConversion == 1) {
            rgb = vec3(
                y + (403.0 / 256.0) * v,
                y - (48.0 / 256.0) * u - (120.0 / 256.0) * v,
                y + (475.0 / 256.0) * u
            );
        } else if (uColorConversion == 2) {
            rgb = vec3(
                y + (377.0 / 256.0) * v,
                y - (42.0 / 256.0) * u - (146.0 / 256.0) * v,
                y + (481.0 / 256.0) * u
            );
        } else {
            rgb = vec3(
                y + (359.0 / 256.0) * v,
                y - (88.0 / 256.0) * u - (183.0 / 256.0) * v,
                y + (454.0 / 256.0) * u
            );
        }
    }
    vec4 out_color = vec4(clamp(rgb, 0.0, 1.0), 1.0);
)";
    fs += use_modern_syntax ? "    FragColor = out_color;\n}\n" : "    gl_FragColor = out_color;\n}\n";
    return fs;
}

} // namespace

GlRenderer::GlRenderer(const EglContext& context) noexcept
    : context_(&context) {}

GlRenderer::~GlRenderer() {
    reset();
}

GlRenderer::GlRenderer(GlRenderer&& other) noexcept {
    *this = std::move(other);
}

GlRenderer& GlRenderer::operator=(GlRenderer&& other) noexcept {
    if (this != &other) {
        reset();

        context_ = other.context_;
        program_ = other.program_;
        external_program_ = other.external_program_;
        planar_program_ = other.planar_program_;
        reusable_fbo_ = other.reusable_fbo_;
        vao_ = other.vao_;
        vbo_ = other.vbo_;
        u_texture_loc_ = other.u_texture_loc_;
        u_color_conv_loc_ = other.u_color_conv_loc_;
        u_color_range_loc_ = other.u_color_range_loc_;
        ext_u_texture_loc_ = other.ext_u_texture_loc_;
        ext_u_color_conv_loc_ = other.ext_u_color_conv_loc_;
        ext_u_color_range_loc_ = other.ext_u_color_range_loc_;
        planar_u_tex_y_loc_ = other.planar_u_tex_y_loc_;
        planar_u_tex_u_loc_ = other.planar_u_tex_u_loc_;
        planar_u_tex_v_loc_ = other.planar_u_tex_v_loc_;
        planar_u_plane_mode_loc_ = other.planar_u_plane_mode_loc_;
        planar_u_color_conv_loc_ = other.planar_u_color_conv_loc_;
        planar_u_color_range_loc_ = other.planar_u_color_range_loc_;

        other.context_ = nullptr;
        other.program_ = 0;
        other.external_program_ = 0;
        other.planar_program_ = 0;
        other.reusable_fbo_ = 0;
        other.vao_ = 0;
        other.vbo_ = 0;
        other.u_texture_loc_ = -1;
        other.u_color_conv_loc_ = -1;
        other.u_color_range_loc_ = -1;
        other.ext_u_texture_loc_ = -1;
        other.ext_u_color_conv_loc_ = -1;
        other.ext_u_color_range_loc_ = -1;
        other.planar_u_tex_y_loc_ = -1;
        other.planar_u_tex_u_loc_ = -1;
        other.planar_u_tex_v_loc_ = -1;
        other.planar_u_plane_mode_loc_ = -1;
        other.planar_u_color_conv_loc_ = -1;
        other.planar_u_color_range_loc_ = -1;
    }
    return *this;
}

void GlRenderer::reset() noexcept {
    if (!context_ || !context_->is_valid()) {
        program_ = 0;
        external_program_ = 0;
        planar_program_ = 0;
        reusable_fbo_ = 0;
        vao_ = 0;
        vbo_ = 0;
        return;
    }

    EglContext::Scope scope(*context_);
    const auto& disp = context_->dispatcher();

    if (reusable_fbo_ != 0 && disp.glDeleteFramebuffers) {
        disp.glDeleteFramebuffers(1, &reusable_fbo_);
        reusable_fbo_ = 0;
    }
    if (vbo_ != 0 && disp.glDeleteBuffers) {
        disp.glDeleteBuffers(1, &vbo_);
        vbo_ = 0;
    }
    if (vao_ != 0 && disp.glDeleteVertexArrays) {
        disp.glDeleteVertexArrays(1, &vao_);
        vao_ = 0;
    }
    if (program_ != 0 && disp.glDeleteProgram) {
        disp.glDeleteProgram(program_);
        program_ = 0;
    }
    if (external_program_ != 0 && disp.glDeleteProgram) {
        disp.glDeleteProgram(external_program_);
        external_program_ = 0;
    }
    if (planar_program_ != 0 && disp.glDeleteProgram) {
        disp.glDeleteProgram(planar_program_);
        planar_program_ = 0;
    }

    u_texture_loc_ = -1;
    u_color_conv_loc_ = -1;
    u_color_range_loc_ = -1;
    ext_u_texture_loc_ = -1;
    ext_u_color_conv_loc_ = -1;
    ext_u_color_range_loc_ = -1;
    planar_u_tex_y_loc_ = -1;
    planar_u_tex_u_loc_ = -1;
    planar_u_tex_v_loc_ = -1;
    planar_u_plane_mode_loc_ = -1;
    planar_u_color_conv_loc_ = -1;
    planar_u_color_range_loc_ = -1;
}

MediaResult<void> GlRenderer::init() {
    if (!context_ || !context_->is_valid()) {
        return MediaError::InvalidState;
    }

    EglContext::Scope scope(*context_);
    const auto& disp = context_->dispatcher();

    if (!disp.glCreateShader || !disp.glCompileShader || !disp.glCreateProgram || !disp.glLinkProgram) {
        return MediaError::HardwareError;
    }

    auto compile_stage = [&](GLenum type, const char* src) -> GLuint {
        GLuint shader = disp.glCreateShader(type);
        if (shader == 0) return 0;
        disp.glShaderSource(shader, 1, &src, nullptr);
        disp.glCompileShader(shader);

        GLint compiled = 0;
        disp.glGetShaderiv(shader, GL_COMPILE_STATUS_VAL, &compiled);
        if (!compiled) {
            disp.glDeleteShader(shader);
            return 0;
        }
        return shader;
    };

    auto build_program = [&](const char* vs_src, const char* fs_src) -> GLuint {
        GLuint vs = compile_stage(GL_VERTEX_SHADER_VAL, vs_src);
        GLuint fs = compile_stage(GL_FRAGMENT_SHADER_VAL, fs_src);
        if (vs == 0 || fs == 0) {
            if (vs) disp.glDeleteShader(vs);
            if (fs) disp.glDeleteShader(fs);
            return 0;
        }
        GLuint prog = disp.glCreateProgram();
        disp.glAttachShader(prog, vs);
        disp.glAttachShader(prog, fs);
        disp.glLinkProgram(prog);
        disp.glDeleteShader(vs);
        disp.glDeleteShader(fs);

        GLint linked = 0;
        disp.glGetProgramiv(prog, GL_LINK_STATUS_VAL, &linked);
        if (!linked) {
            disp.glDeleteProgram(prog);
            return 0;
        }
        return prog;
    };

    // Standard sampler2D shader pairs (Desktop GL 3.3 Core -> GLSL 1.30 -> GLES/compat fallback)
    const char* vs_330 = R"(#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;
out vec2 vTexCoord;

void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    vTexCoord = aTexCoord;
}
)";
    const char* fs_330 = R"(#version 330 core
in vec2 vTexCoord;
out vec4 FragColor;

uniform sampler2D uTexture;
uniform int uColorConversion;
uniform int uColorRange;

void main() {
    FragColor = texture(uTexture, vTexCoord);
}
)";

    const char* vs_130 = R"(#version 130
in vec2 aPos;
in vec2 aTexCoord;
out vec2 vTexCoord;

void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    vTexCoord = aTexCoord;
}
)";
    const char* fs_130 = R"(#version 130
in vec2 vTexCoord;
out vec4 FragColor;

uniform sampler2D uTexture;
uniform int uColorConversion;
uniform int uColorRange;

void main() {
    FragColor = texture(uTexture, vTexCoord);
}
)";

    const char* vs_300es = R"(#version 300 es
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;
out vec2 vTexCoord;

void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    vTexCoord = aTexCoord;
}
)";
    const char* fs_300es = R"(#version 300 es
precision mediump float;
in vec2 vTexCoord;
out vec4 FragColor;

uniform sampler2D uTexture;
uniform int uColorConversion;
uniform int uColorRange;

void main() {
    FragColor = texture(uTexture, vTexCoord);
}
)";

    const char* vs_compat = R"(
attribute vec2 aPos;
attribute vec2 aTexCoord;
varying vec2 vTexCoord;

void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    vTexCoord = aTexCoord;
}
)";
    const char* fs_compat = R"(
#ifdef GL_ES
precision mediump float;
#endif
varying vec2 vTexCoord;
uniform sampler2D uTexture;

void main() {
    gl_FragColor = texture2D(uTexture, vTexCoord);
}
)";

    struct ShaderPair {
        const char* vs;
        const char* fs;
    };

    auto compile_first = [&](std::initializer_list<ShaderPair> candidates) -> GLuint {
        for (const auto& pair : candidates) {
            if (GLuint id = build_program(pair.vs, pair.fs); id != 0) {
                return id;
            }
        }
        return 0;
    };

    // Detect OpenGL ES vs Desktop OpenGL so incompatible GLSL versions (e.g. #version 330 core
    // under Mesa OpenGL ES) are never sent to the wrong compiler stage.
    bool is_gles = false;
    if (!context_->gl_version().empty()) {
        is_gles = (context_->gl_version().find("OpenGL ES") != std::string::npos);
    } else if (disp.glGetString) {
        if (const auto* ver = reinterpret_cast<const char*>(disp.glGetString(0x1F02))) {
            is_gles = (std::string_view(ver).find("OpenGL ES") != std::string_view::npos);
        }
    } else if (disp.eglQueryAPI) {
        is_gles = (disp.eglQueryAPI() == EGL_OPENGL_ES_API_VAL);
    }

    if (program_ == 0) {
        program_ = is_gles
            ? compile_first({{vs_300es, fs_300es}, {vs_compat, fs_compat}})
            : compile_first({{vs_330, fs_330}, {vs_130, fs_130}, {vs_compat, fs_compat}});
        if (program_ == 0) {
            return MediaError::HardwareError;
        }
        u_texture_loc_ = disp.glGetUniformLocation(program_, "uTexture");
        u_color_conv_loc_ = disp.glGetUniformLocation(program_, "uColorConversion");
        u_color_range_loc_ = disp.glGetUniformLocation(program_, "uColorRange");
    }

    // External OES sampler shader pairs for zero-copy YUV/DMA-BUF textures (GL_TEXTURE_EXTERNAL_OES)
    if (external_program_ == 0) {
        const char* fs_ext_130 = R"(#version 130
#extension GL_OES_EGL_image_external : require
in vec2 vTexCoord;
out vec4 FragColor;

uniform samplerExternalOES uTexture;
uniform int uColorConversion;
uniform int uColorRange;

void main() {
    FragColor = texture2D(uTexture, vTexCoord);
}
)";
        const char* fs_ext_300es = R"(#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
precision mediump float;
in vec2 vTexCoord;
out vec4 FragColor;

uniform samplerExternalOES uTexture;
uniform int uColorConversion;
uniform int uColorRange;

void main() {
    FragColor = texture(uTexture, vTexCoord);
}
)";
        const char* fs_ext_compat = R"(#extension GL_OES_EGL_image_external : require
#ifdef GL_ES
precision mediump float;
#endif
varying vec2 vTexCoord;
uniform samplerExternalOES uTexture;

void main() {
    gl_FragColor = texture2D(uTexture, vTexCoord);
}
)";
        external_program_ = is_gles
            ? compile_first({{vs_300es, fs_ext_300es}, {vs_compat, fs_ext_compat}})
            : compile_first({{vs_130, fs_ext_130}, {vs_compat, fs_ext_compat}});

        if (external_program_ != 0) {
            ext_u_texture_loc_ = disp.glGetUniformLocation(external_program_, "uTexture");
            ext_u_color_conv_loc_ = disp.glGetUniformLocation(external_program_, "uColorConversion");
            ext_u_color_range_loc_ = disp.glGetUniformLocation(external_program_, "uColorRange");
        }
    }

    // Planar YUV->RGB shader pipeline (zero-copy Desktop OpenGL 3.3 Core & ES GL_TEXTURE_2D planes)
    if (planar_program_ == 0) {
        if (is_gles) {
            const std::string fs_planar_300es = make_planar_fragment_shader(
                "#version 300 es\n#ifdef GL_FRAGMENT_PRECISION_HIGH\nprecision highp float;\n#else\nprecision mediump float;\n#endif\n",
                true);
            const std::string fs_planar_compat = make_planar_fragment_shader(
                "#ifdef GL_ES\nprecision mediump float;\n#endif\n",
                false);
            planar_program_ = compile_first({
                {vs_300es, fs_planar_300es.c_str()},
                {vs_compat, fs_planar_compat.c_str()}
            });
        } else {
            const std::string fs_planar_330 = make_planar_fragment_shader("#version 330 core\n", true);
            const std::string fs_planar_130 = make_planar_fragment_shader("#version 130\n", true);
            const std::string fs_planar_compat = make_planar_fragment_shader("", false);
            planar_program_ = compile_first({
                {vs_330, fs_planar_330.c_str()},
                {vs_130, fs_planar_130.c_str()},
                {vs_compat, fs_planar_compat.c_str()}
            });
        }

        if (planar_program_ != 0) {
            planar_u_tex_y_loc_ = disp.glGetUniformLocation(planar_program_, "uTexY");
            planar_u_tex_u_loc_ = disp.glGetUniformLocation(planar_program_, "uTexU");
            planar_u_tex_v_loc_ = disp.glGetUniformLocation(planar_program_, "uTexV");
            planar_u_plane_mode_loc_ = disp.glGetUniformLocation(planar_program_, "uPlaneMode");
            planar_u_color_conv_loc_ = disp.glGetUniformLocation(planar_program_, "uColorConversion");
            planar_u_color_range_loc_ = disp.glGetUniformLocation(planar_program_, "uColorRange");
        }
    }

    // Setup geometry (Fullscreen Quad)
    const float quad_vertices[] = {
        // X,      Y,       U,    V
        -1.0f, -1.0f,    0.0f, 0.0f,
         1.0f, -1.0f,    1.0f, 0.0f,
         1.0f,  1.0f,    1.0f, 1.0f,

        -1.0f, -1.0f,    0.0f, 0.0f,
         1.0f,  1.0f,    1.0f, 1.0f,
        -1.0f,  1.0f,    0.0f, 1.0f
    };

    if (disp.glGenVertexArrays) {
        disp.glGenVertexArrays(1, &vao_);
        disp.glBindVertexArray(vao_);
    }

    if (disp.glGenBuffers) {
        disp.glGenBuffers(1, &vbo_);
        disp.glBindBuffer(GL_ARRAY_BUFFER_VAL, vbo_);
        disp.glBufferData(GL_ARRAY_BUFFER_VAL, sizeof(quad_vertices), quad_vertices, GL_STATIC_DRAW_VAL);

        disp.glEnableVertexAttribArray(0);
        disp.glVertexAttribPointer(0, 2, GL_FLOAT_VAL, GL_FALSE_VAL, 4 * sizeof(float), reinterpret_cast<void*>(0));

        disp.glEnableVertexAttribArray(1);
        disp.glVertexAttribPointer(1, 2, GL_FLOAT_VAL, GL_FALSE_VAL, 4 * sizeof(float), reinterpret_cast<void*>(2 * sizeof(float)));

        if (disp.glBindVertexArray) {
            disp.glBindVertexArray(0);
        }
        disp.glBindBuffer(GL_ARRAY_BUFFER_VAL, 0);
    }

    return MediaResult<void>::ok();
}

void GlRenderer::draw_quad() const noexcept {
    const auto& disp = context_->dispatcher();
    if (disp.glBindVertexArray && vao_ != 0) {
        disp.glBindVertexArray(vao_);
        disp.glDrawArrays(GL_TRIANGLES_VAL, 0, 6);
        disp.glBindVertexArray(0);
    } else if (vbo_ != 0) {
        disp.glBindBuffer(GL_ARRAY_BUFFER_VAL, vbo_);
        disp.glEnableVertexAttribArray(0);
        disp.glVertexAttribPointer(0, 2, GL_FLOAT_VAL, GL_FALSE_VAL, 4 * sizeof(float), reinterpret_cast<void*>(0));
        disp.glEnableVertexAttribArray(1);
        disp.glVertexAttribPointer(1, 2, GL_FLOAT_VAL, GL_FALSE_VAL, 4 * sizeof(float), reinterpret_cast<void*>(2 * sizeof(float)));
        disp.glDrawArrays(GL_TRIANGLES_VAL, 0, 6);
        disp.glBindBuffer(GL_ARRAY_BUFFER_VAL, 0);
    }
}

MediaResult<void> GlRenderer::render_to_fbo(
    const GlTexture& src,
    GLuint dst_fbo,
    uint32_t dst_width,
    uint32_t dst_height,
    ColorSpace color_space,
    ColorRange color_range
) {
    if (!context_ || !context_->is_valid()) {
        return MediaError::InvalidState;
    }

    if (!is_initialized()) {
        auto init_res = init();
        if (!init_res.is_ok()) {
            return init_res;
        }
    }

    if (!src.is_valid()) {
        return MediaError::InvalidData;
    }

    const bool is_external = (src.target() == GL_TEXTURE_EXTERNAL_OES_VAL);
    const GLuint active_prog = is_external ? external_program_ : program_;
    if (active_prog == 0) {
        return MediaError::UnsupportedFormat;
    }

    EglContext::Scope scope(*context_);
    const auto& disp = context_->dispatcher();

    disp.glBindFramebuffer(GL_FRAMEBUFFER_VAL, dst_fbo);
    disp.glViewport(0, 0, static_cast<GLsizei>(dst_width), static_cast<GLsizei>(dst_height));

    disp.glUseProgram(active_prog);

    // Bind texture to unit 0
    if (disp.glActiveTexture) {
        disp.glActiveTexture(GL_TEXTURE0_VAL);
    }
    disp.glBindTexture(src.target(), src.id());

    const GLint tex_loc = is_external ? ext_u_texture_loc_ : u_texture_loc_;
    const GLint conv_loc = is_external ? ext_u_color_conv_loc_ : u_color_conv_loc_;
    const GLint range_loc = is_external ? ext_u_color_range_loc_ : u_color_range_loc_;

    if (tex_loc >= 0) {
        disp.glUniform1i(tex_loc, 0);
    }
    if (conv_loc >= 0) {
        disp.glUniform1i(conv_loc, color_space_to_uniform(color_space));
    }
    if (range_loc >= 0) {
        disp.glUniform1i(range_loc, color_range == ColorRange::Studio ? 1 : 0);
    }

    draw_quad();

    disp.glBindTexture(src.target(), 0);
    disp.glUseProgram(0);
    disp.glBindFramebuffer(GL_FRAMEBUFFER_VAL, 0);

    return MediaResult<void>::ok();
}

MediaResult<void> GlRenderer::render_to_fbo(
    const GlPlanarTextures& src,
    GLuint dst_fbo,
    uint32_t dst_width,
    uint32_t dst_height
) {
    if (!context_ || !context_->is_valid()) {
        return MediaError::InvalidState;
    }
    if (!src.is_valid()) {
        return MediaError::InvalidData;
    }

    if (src.plane_count == 1) {
        return render_to_fbo(src.planes[0], dst_fbo, dst_width, dst_height, src.color_space, src.color_range);
    }

    if (!is_initialized()) {
        auto init_res = init();
        if (!init_res.is_ok()) {
            return init_res;
        }
    }

    if (planar_program_ == 0 || src.plane_count < 2 || !src.planes[1].is_valid()) {
        return MediaError::UnsupportedFormat;
    }

    GLint plane_mode = 0;
    if (src.drm_fourcc == DRM_FOURCC_NV21) {
        plane_mode = 1;
    } else if (src.drm_fourcc == DRM_FOURCC_YUV420 || src.plane_count >= 3) {
        if (src.plane_count < 3 || !src.planes[2].is_valid()) {
            return MediaError::InvalidData;
        }
        plane_mode = 2;
    }

    EglContext::Scope scope(*context_);
    const auto& disp = context_->dispatcher();

    disp.glBindFramebuffer(GL_FRAMEBUFFER_VAL, dst_fbo);
    disp.glViewport(0, 0, static_cast<GLsizei>(dst_width), static_cast<GLsizei>(dst_height));
    disp.glUseProgram(planar_program_);

    const auto& tex_y = src.planes[0];
    const auto& tex_u = src.planes[1];
    const auto& tex_v = (plane_mode == 2) ? src.planes[2] : src.planes[1];

    if (disp.glActiveTexture) disp.glActiveTexture(GL_TEXTURE0_VAL);
    disp.glBindTexture(tex_y.target(), tex_y.id());
    if (disp.glActiveTexture) disp.glActiveTexture(GL_TEXTURE1_VAL);
    disp.glBindTexture(tex_u.target(), tex_u.id());
    if (disp.glActiveTexture) disp.glActiveTexture(GL_TEXTURE2_VAL);
    disp.glBindTexture(tex_v.target(), tex_v.id());

    if (planar_u_tex_y_loc_ >= 0) disp.glUniform1i(planar_u_tex_y_loc_, 0);
    if (planar_u_tex_u_loc_ >= 0) disp.glUniform1i(planar_u_tex_u_loc_, 1);
    if (planar_u_tex_v_loc_ >= 0) disp.glUniform1i(planar_u_tex_v_loc_, 2);
    if (planar_u_plane_mode_loc_ >= 0) disp.glUniform1i(planar_u_plane_mode_loc_, plane_mode);
    if (planar_u_color_conv_loc_ >= 0) disp.glUniform1i(planar_u_color_conv_loc_, color_space_to_uniform(src.color_space));
    if (planar_u_color_range_loc_ >= 0) disp.glUniform1i(planar_u_color_range_loc_, src.color_range == ColorRange::Studio ? 1 : 0);

    draw_quad();

    if (disp.glActiveTexture) disp.glActiveTexture(GL_TEXTURE2_VAL);
    disp.glBindTexture(tex_v.target(), 0);
    if (disp.glActiveTexture) disp.glActiveTexture(GL_TEXTURE1_VAL);
    disp.glBindTexture(tex_u.target(), 0);
    if (disp.glActiveTexture) disp.glActiveTexture(GL_TEXTURE0_VAL);
    disp.glBindTexture(tex_y.target(), 0);

    disp.glUseProgram(0);
    disp.glBindFramebuffer(GL_FRAMEBUFFER_VAL, 0);

    return MediaResult<void>::ok();
}

template <typename RenderFn>
MediaResult<GlTexture> GlRenderer::render_to_texture_common(
    uint32_t dst_width,
    uint32_t dst_height,
    RenderFn&& render_fn
) {
    if (!context_ || !context_->is_valid()) {
        return MediaError::InvalidState;
    }

    EglContext::Scope scope(*context_);
    const auto& disp = context_->dispatcher();

    GLuint dst_tex = 0;
    disp.glGenTextures(1, &dst_tex);
    disp.glBindTexture(GL_TEXTURE_2D_VAL, dst_tex);
    disp.glTexImage2D(
        GL_TEXTURE_2D_VAL,
        0,
        static_cast<GLint>(GL_RGBA8_VAL),
        static_cast<GLsizei>(dst_width),
        static_cast<GLsizei>(dst_height),
        0,
        GL_RGBA_VAL,
        GL_UNSIGNED_BYTE_VAL,
        nullptr
    );
    disp.glTexParameteri(GL_TEXTURE_2D_VAL, GL_TEXTURE_MIN_FILTER_VAL, GL_LINEAR_VAL);
    disp.glTexParameteri(GL_TEXTURE_2D_VAL, GL_TEXTURE_MAG_FILTER_VAL, GL_LINEAR_VAL);
    disp.glTexParameteri(GL_TEXTURE_2D_VAL, GL_TEXTURE_WRAP_S_VAL, GL_CLAMP_TO_EDGE_VAL);
    disp.glTexParameteri(GL_TEXTURE_2D_VAL, GL_TEXTURE_WRAP_T_VAL, GL_CLAMP_TO_EDGE_VAL);

    if (reusable_fbo_ == 0) {
        disp.glGenFramebuffers(1, &reusable_fbo_);
    }
    disp.glBindFramebuffer(GL_FRAMEBUFFER_VAL, reusable_fbo_);
    disp.glFramebufferTexture2D(GL_FRAMEBUFFER_VAL, GL_COLOR_ATTACHMENT0_VAL, GL_TEXTURE_2D_VAL, dst_tex, 0);

    auto render_res = render_fn(reusable_fbo_);

    disp.glBindFramebuffer(GL_FRAMEBUFFER_VAL, reusable_fbo_);
    disp.glFramebufferTexture2D(GL_FRAMEBUFFER_VAL, GL_COLOR_ATTACHMENT0_VAL, GL_TEXTURE_2D_VAL, 0, 0);
    disp.glBindFramebuffer(GL_FRAMEBUFFER_VAL, 0);
    disp.glBindTexture(GL_TEXTURE_2D_VAL, 0);

    if (!render_res.is_ok()) {
        disp.glDeleteTextures(1, &dst_tex);
        return render_res.error();
    }

    GlTexture result(*context_);
    result.texture_ = dst_tex;
    result.target_ = GL_TEXTURE_2D_VAL;
    result.width_ = dst_width;
    result.height_ = dst_height;
    result.drm_fourcc_ = DRM_FOURCC_RGBA8888;

    return result;
}

MediaResult<GlTexture> GlRenderer::render_to_texture(
    const GlTexture& src,
    uint32_t dst_width,
    uint32_t dst_height,
    ColorSpace color_space,
    ColorRange color_range
) {
    return render_to_texture_common(dst_width, dst_height, [&](GLuint fbo) {
        return render_to_fbo(src, fbo, dst_width, dst_height, color_space, color_range);
    });
}

MediaResult<GlTexture> GlRenderer::render_to_texture(
    const GlPlanarTextures& src,
    uint32_t dst_width,
    uint32_t dst_height
) {
    return render_to_texture_common(dst_width, dst_height, [&](GLuint fbo) {
        return render_to_fbo(src, fbo, dst_width, dst_height);
    });
}

} // namespace atum::gl
