// OpenGL ES 1.x fixed-function emulation on top of OpenGL ES 2.0+, covering exactly what the Tamagotchi engine uses.
#pragma once
#include <cstdint>
#include <vector>
#include "engine.h"

namespace tg {

class Gles1 : public GlBackend {
public:
    bool init();        // compile shaders; call on the GL thread once a context is current
    void shutdown();
    // Mirrors what the original Java renderer did before each frame, plus letterboxing.
    // surface = full surface size; (vx,vy,vw,vh) = game viewport (origin bottom-left).
    void beginFrame(int surfaceW, int surfaceH, int vx, int vy, int vw, int vh);
    void call(Engine& e, GlFn fn, const uint32_t a[6]) override;
    // Reads the game viewport as RGBA, top row first. Call after Engine::runFrame().
    bool readViewport(std::vector<uint8_t>& rgba, int& w, int& h);

private:
    struct Mat { float m[16]; };
    struct ArrayState { bool enabled = false; int size = 0; uint32_t type = 0x1406, stride = 0, ptr = 0; };
    static Mat identity();
    static Mat mul(const Mat& a, const Mat& b);
    std::vector<Mat>& cur() { return stacks_[mode_ == 0x1701 ? 1 : mode_ == 0x1702 ? 2 : 0]; }
    void draw(Engine& e, uint32_t mode, int first, int count);
    bool readArray(Engine& e, const ArrayState& as, int first, int count, std::vector<float>& out, int comps, bool normalizeBytes);

    unsigned prog_ = 0, vbo_ = 0;
    int locPos_ = -1, locUV_ = -1, locCol_ = -1, locMVP_ = -1, locTexM_ = -1, locTex_ = -1, locUseTex_ = -1, locReplace_ = -1;
    std::vector<Mat> stacks_[3];
    uint32_t mode_ = 0x1700;
    float color_[4] = {1, 1, 1, 1};
    ArrayState vert_, tex_, col_;
    bool tex2d_ = false;
    uint32_t texEnv_ = 0x1E01;
    int surfaceW_ = 0, surfaceH_ = 0, vx_ = 0, vy_ = 0, vw_ = 0, vh_ = 0;
    std::vector<uint8_t> raw_;
    std::vector<float> pos_, uv_, colv_, packed_;
};

}  // namespace tg
