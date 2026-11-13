#include "gles1.h"

#include <cmath>
#include <cstring>
#include <GLES2/gl2.h>

namespace tg {

namespace {
inline float f32(uint32_t b) { float f; std::memcpy(&f, &b, 4); return f; }

const char* kVert =
    "attribute vec3 aPos;\n"
    "attribute vec2 aUV;\n"
    "attribute vec4 aCol;\n"
    "uniform mat4 uMVP;\n"
    "uniform mat4 uTexM;\n"
    "varying vec2 vUV;\n"
    "varying vec4 vCol;\n"
    "void main() {\n"
    "  gl_Position = uMVP * vec4(aPos, 1.0);\n"
    "  vUV = (uTexM * vec4(aUV, 0.0, 1.0)).xy;\n"
    "  vCol = aCol;\n"
    "}\n";

const char* kFrag =
    "precision mediump float;\n"
    "varying vec2 vUV;\n"
    "varying vec4 vCol;\n"
    "uniform sampler2D uTex;\n"
    "uniform float uUseTex;\n"
    "uniform float uReplace;\n"
    "void main() {\n"
    "  vec4 c = vCol;\n"
    "  if (uUseTex > 0.5) {\n"
    "    vec4 t = texture2D(uTex, vUV);\n"
    "    c = (uReplace > 0.5) ? t : t * c;\n"
    "  }\n"
    "  gl_FragColor = c;\n"
    "}\n";

unsigned compile(GLenum type, const char* src) {
    unsigned s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { glDeleteShader(s); return 0; }
    return s;
}
}  // namespace

Gles1::Mat Gles1::identity() { Mat r{}; r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1; return r; }
Gles1::Mat Gles1::mul(const Mat& a, const Mat& b) {   // column-major: r = a * b
    Mat r{};
    for (int c = 0; c < 4; c++)
        for (int rr = 0; rr < 4; rr++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a.m[k * 4 + rr] * b.m[c * 4 + k];
            r.m[c * 4 + rr] = s;
        }
    return r;
}

bool Gles1::init() {
    unsigned vs = compile(GL_VERTEX_SHADER, kVert), fs = compile(GL_FRAGMENT_SHADER, kFrag);
    if (!vs || !fs) return false;
    prog_ = glCreateProgram();
    glAttachShader(prog_, vs); glAttachShader(prog_, fs);
    glBindAttribLocation(prog_, 0, "aPos"); glBindAttribLocation(prog_, 1, "aUV"); glBindAttribLocation(prog_, 2, "aCol");
    glLinkProgram(prog_);
    GLint ok = 0; glGetProgramiv(prog_, GL_LINK_STATUS, &ok);
    glDeleteShader(vs); glDeleteShader(fs);
    if (!ok) return false;
    locMVP_ = glGetUniformLocation(prog_, "uMVP"); locTexM_ = glGetUniformLocation(prog_, "uTexM");
    locTex_ = glGetUniformLocation(prog_, "uTex"); locUseTex_ = glGetUniformLocation(prog_, "uUseTex");
    locReplace_ = glGetUniformLocation(prog_, "uReplace");
    glGenBuffers(1, &vbo_);
    for (auto& s : stacks_) s.assign(1, identity());
    return true;
}

void Gles1::shutdown() {
    if (prog_) glDeleteProgram(prog_);
    if (vbo_) glDeleteBuffers(1, &vbo_);
    prog_ = vbo_ = 0;
}

void Gles1::beginFrame(int sw, int sh, int vx, int vy, int vw, int vh) {
    surfaceW_ = sw; surfaceH_ = sh; vx_ = vx; vy_ = vy; vw_ = vw; vh_ = vh;
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glStencilMask(0xFF);
    glViewport(0, 0, sw, sh);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glViewport(vx, vy, vw, vh);
    // Java shell state per frame: ortho(0,320,480,0,-1,1), blend(ONE, ONE_MINUS_SRC_ALPHA)
    for (auto& s : stacks_) s.assign(1, identity());
    Mat o = identity();
    const float l = 0, r = 320, b = 480, t = 0, n = -1, f = 1;
    o.m[0] = 2 / (r - l); o.m[5] = 2 / (t - b); o.m[10] = -2 / (f - n);
    o.m[12] = -(r + l) / (r - l); o.m[13] = -(t + b) / (t - b); o.m[14] = -(f + n) / (f - n);
    stacks_[1][0] = o;
    mode_ = 0x1700;
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    tex2d_ = false; texEnv_ = 0x1E01;
    color_[0] = color_[1] = color_[2] = color_[3] = 1;
    vert_ = ArrayState(); tex_ = ArrayState(); col_ = ArrayState();
}

bool Gles1::readArray(Engine& e, const ArrayState& as, int first, int count, std::vector<float>& out, int comps, bool normBytes) {
    int tsz;
    switch (as.type) { case 0x1400: case 0x1401: tsz = 1; break; case 0x1402: case 0x1403: tsz = 2; break; case 0x1406: case 0x140C: tsz = 4; break; default: return false; }
    int size = as.size < comps ? as.size : comps;
    if (size <= 0) return false;
    uint32_t stride = as.stride ? as.stride : uint32_t(as.size * tsz);
    size_t bytes = size_t(stride) * (count - 1) + size_t(as.size) * tsz;
    raw_.resize(bytes);
    if (!e.rd(as.ptr + uint32_t(first) * stride, raw_.data(), bytes)) return false;
    out.assign(size_t(count) * comps, 0.0f);
    for (int i = 0; i < count; i++) {
        const uint8_t* p = raw_.data() + size_t(i) * stride;
        for (int k = 0; k < size; k++) {
            float v = 0;
            switch (as.type) {
                case 0x1400: v = float(int8_t(p[k])); if (normBytes) v /= 127.f; break;
                case 0x1401: v = float(p[k]); if (normBytes) v /= 255.f; break;
                case 0x1402: { int16_t s; std::memcpy(&s, p + 2 * k, 2); v = float(s); } break;
                case 0x1403: { uint16_t s; std::memcpy(&s, p + 2 * k, 2); v = float(s); } break;
                case 0x1406: std::memcpy(&v, p + 4 * k, 4); break;
                case 0x140C: { int32_t s; std::memcpy(&s, p + 4 * k, 4); v = float(s) / 65536.f; } break;
            }
            out[size_t(i) * comps + k] = v;
        }
    }
    return true;
}

void Gles1::draw(Engine& e, uint32_t mode, int first, int count) {
    if (count < 1 || !vert_.enabled || !vert_.ptr || mode > 6) return;
    if (!readArray(e, vert_, first, count, pos_, 3, false)) return;
    bool useTex = tex2d_ && tex_.enabled && tex_.ptr;
    if (useTex) { if (!readArray(e, tex_, first, count, uv_, 2, false)) useTex = false; }
    bool useCol = col_.enabled && col_.ptr;
    if (useCol) { if (!readArray(e, col_, first, count, colv_, 4, true)) useCol = false; }
    if (useCol && col_.size == 3) for (int i = 0; i < count; i++) colv_[size_t(i) * 4 + 3] = 1.0f;
    packed_.resize(size_t(count) * 9);
    for (int i = 0; i < count; i++) {
        float* d = &packed_[size_t(i) * 9];
        d[0] = pos_[size_t(i) * 3]; d[1] = pos_[size_t(i) * 3 + 1]; d[2] = vert_.size >= 3 ? pos_[size_t(i) * 3 + 2] : 0.f;
        d[3] = useTex ? uv_[size_t(i) * 2] : 0.f; d[4] = useTex ? uv_[size_t(i) * 2 + 1] : 0.f;
        for (int k = 0; k < 4; k++) d[5 + k] = useCol ? colv_[size_t(i) * 4 + k] : color_[k];
    }
    Mat mvp = mul(stacks_[1].back(), stacks_[0].back());
    glUseProgram(prog_);
    glUniformMatrix4fv(locMVP_, 1, GL_FALSE, mvp.m);
    glUniformMatrix4fv(locTexM_, 1, GL_FALSE, stacks_[2].back().m);
    glUniform1i(locTex_, 0);
    glUniform1f(locUseTex_, useTex ? 1.f : 0.f);
    glUniform1f(locReplace_, texEnv_ == 0x1E01 ? 1.f : 0.f);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(packed_.size() * 4), packed_.data(), GL_STREAM_DRAW);
    glEnableVertexAttribArray(0); glEnableVertexAttribArray(1); glEnableVertexAttribArray(2);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 36, reinterpret_cast<void*>(0));
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 36, reinterpret_cast<void*>(12));
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 36, reinterpret_cast<void*>(20));
    glDrawArrays(mode, 0, count);
    glDisableVertexAttribArray(0); glDisableVertexAttribArray(1); glDisableVertexAttribArray(2);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void Gles1::call(Engine& e, GlFn fn, const uint32_t a[6]) {
    switch (fn) {
        case GL_MatrixMode: mode_ = a[0]; break;
        case GL_LoadIdentity: cur().back() = identity(); break;
        case GL_PushMatrix: { auto& s = cur(); if (s.size() < 32) s.push_back(s.back()); } break;
        case GL_PopMatrix: { auto& s = cur(); if (s.size() > 1) s.pop_back(); } break;
        case GL_Orthof: {
            float l = f32(a[0]), r = f32(a[1]), b = f32(a[2]), t = f32(a[3]), n = f32(a[4]), f = f32(a[5]);
            Mat o = identity();
            o.m[0] = 2 / (r - l); o.m[5] = 2 / (t - b); o.m[10] = -2 / (f - n);
            o.m[12] = -(r + l) / (r - l); o.m[13] = -(t + b) / (t - b); o.m[14] = -(f + n) / (f - n);
            cur().back() = mul(cur().back(), o);
        } break;
        case GL_Translatef: { Mat t = identity(); t.m[12] = f32(a[0]); t.m[13] = f32(a[1]); t.m[14] = f32(a[2]); cur().back() = mul(cur().back(), t); } break;
        case GL_Scalef: { Mat s = identity(); s.m[0] = f32(a[0]); s.m[5] = f32(a[1]); s.m[10] = f32(a[2]); cur().back() = mul(cur().back(), s); } break;
        case GL_Rotatef: {
            float ang = f32(a[0]) * 3.14159265358979f / 180.f, x = f32(a[1]), y = f32(a[2]), z = f32(a[3]);
            float len = std::sqrt(x * x + y * y + z * z); if (len == 0) break;
            x /= len; y /= len; z /= len;
            float c = std::cos(ang), s = std::sin(ang), k = 1 - c;
            Mat r = identity();
            r.m[0] = c + x * x * k;     r.m[4] = x * y * k - z * s; r.m[8]  = x * z * k + y * s;
            r.m[1] = y * x * k + z * s; r.m[5] = c + y * y * k;     r.m[9]  = y * z * k - x * s;
            r.m[2] = z * x * k - y * s; r.m[6] = z * y * k + x * s; r.m[10] = c + z * z * k;
            cur().back() = mul(cur().back(), r);
        } break;
        case GL_BindTexture: glBindTexture(GL_TEXTURE_2D, a[1]); break;
        case GL_Color4f: for (int i = 0; i < 4; i++) color_[i] = f32(a[i]); break;
        case GL_Color4ub: for (int i = 0; i < 4; i++) color_[i] = float(a[i] & 0xFF) / 255.f; break;
        case GL_Enable: case GL_Disable: {
            bool on = fn == GL_Enable;
            switch (a[0]) {
                case 0x0DE1: tex2d_ = on; break;                       // GL_TEXTURE_2D is a state flag in ES1 only
                case 0x0BE2: case 0x0B90: case 0x0B71: case 0x0B44: case 0x0C11: case 0x0BD0: case 0x8037:
                    if (on) glEnable(a[0]); else glDisable(a[0]); break; // blend, stencil, depth, cull, scissor, dither, polygon offset
                default: break;                                        // alpha test, lighting, fog, ... unused by this game
            }
        } break;
        case GL_EnableClientState: case GL_DisableClientState: {
            bool on = fn == GL_EnableClientState;
            if (a[0] == 0x8074) vert_.enabled = on; else if (a[0] == 0x8078) tex_.enabled = on; else if (a[0] == 0x8076) col_.enabled = on;
        } break;
        case GL_VertexPointer: vert_.size = int(a[0]); vert_.type = a[1]; vert_.stride = a[2]; vert_.ptr = a[3]; break;
        case GL_TexCoordPointer: tex_.size = int(a[0]); tex_.type = a[1]; tex_.stride = a[2]; tex_.ptr = a[3]; break;
        case GL_ColorPointer: col_.size = int(a[0]); col_.type = a[1]; col_.stride = a[2]; col_.ptr = a[3]; break;
        case GL_BlendFunc: glBlendFunc(a[0], a[1]); break;
        case GL_ColorMask: glColorMask(a[0] & 0xFF, a[1] & 0xFF, a[2] & 0xFF, a[3] & 0xFF); break;
        case GL_StencilFunc: glStencilFunc(a[0], int32_t(a[1]), a[2]); break;
        case GL_StencilOp: glStencilOp(a[0], a[1], a[2]); break;
        case GL_ClearStencil: glClearStencil(int32_t(a[0])); break;
        case GL_TexEnvi: if (a[0] == 0x2300 && a[1] == 0x2200) texEnv_ = a[2]; break;
        case GL_TexParameteri: if (a[1] != 0x8191) glTexParameteri(a[0], a[1], int32_t(a[2])); break;       // skip GL_GENERATE_MIPMAP
        case GL_TexParameterf: if (a[1] != 0x8191) glTexParameterf(a[0], a[1], f32(a[2])); break;
        case GL_DrawArrays: draw(e, a[0], int32_t(a[1]), int32_t(a[2])); break;
        case GL_GenBuffers: {
            std::vector<unsigned> ids(a[0] > 64 ? 64 : a[0]);
            if (!ids.empty()) glGenBuffers(GLsizei(ids.size()), ids.data());
            for (size_t i = 0; i < ids.size(); i++) e.w32(a[1] + 4 * uint32_t(i), ids[i]);
        } break;
        case GL_DeleteBuffers: {
            std::vector<unsigned> ids(a[0] > 64 ? 64 : a[0]);
            for (size_t i = 0; i < ids.size(); i++) ids[i] = e.r32(a[1] + 4 * uint32_t(i));
            if (!ids.empty()) glDeleteBuffers(GLsizei(ids.size()), ids.data());
        } break;
        case GL_GetIntegerv: {
            GLint v[16] = {0};
            while (glGetError() != GL_NO_ERROR) {}
            glGetIntegerv(a[0], v);
            if (glGetError() != GL_NO_ERROR) v[0] = 0;
            e.w32(a[1], uint32_t(v[0]));
        } break;
        default: break;
    }
}

bool Gles1::readViewport(std::vector<uint8_t>& rgba, int& w, int& h) {
    if (vw_ <= 0 || vh_ <= 0) return false;
    w = vw_; h = vh_;
    std::vector<uint8_t> tmp(size_t(w) * h * 4);
    glReadPixels(vx_, vy_, w, h, GL_RGBA, GL_UNSIGNED_BYTE, tmp.data());
    rgba.resize(tmp.size());
    for (int y = 0; y < h; y++) std::memcpy(&rgba[size_t(y) * w * 4], &tmp[size_t(h - 1 - y) * w * 4], size_t(w) * 4);
    return true;
}

}  // namespace tg
