// Tamagotchi Classic compatibility engine.
// Runs the original 32-bit ARM game library (libAndroidGame001JNI.so) inside a Unicorn CPU emulator,
// so it works on 64-bit-only Android devices. Portable C++17: builds on Android (NDK) and desktop.
#pragma once
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <unicorn/unicorn.h>

namespace tg {

// ---- A Java value crossing the guest<->host boundary -------------------------------------------
struct JV {
    enum Type : uint8_t { Void, Int, Long, Float, Double, Str, Bytes, Null };
    Type t = Void;
    int64_t i = 0;
    double d = 0;
    std::string s;
    std::vector<uint8_t> b;
    static JV Int_(int64_t v) { JV j; j.t = Int; j.i = v; return j; }
    static JV Long_(int64_t v) { JV j; j.t = Long; j.i = v; return j; }
    static JV Flt(double v) { JV j; j.t = Float; j.d = v; return j; }
    static JV Dbl(double v) { JV j; j.t = Double; j.d = v; return j; }
    static JV Str_(std::string v) { JV j; j.t = Str; j.s = std::move(v); return j; }
    static JV Bytes_(std::vector<uint8_t> v) { JV j; j.t = Bytes; j.b = std::move(v); return j; }
    static JV Null_() { JV j; j.t = Null; return j; }
};

// ---- What the Java side (Kotlin on Android) must provide --------------------------------------
class Host {
public:
    virtual ~Host() = default;
    // Called for every static Java callback the engine makes (OnLoadTexture, OnPlaySound, ...).
    // `sig` is the JNI signature the engine asked for, e.g. "(Ljava/lang/String;)I".
    virtual JV invoke(const std::string& name, const std::string& sig, std::vector<JV>& args) = 0;
    virtual void log(const std::string& msg) { (void)msg; }
};

// ---- OpenGL ES 1.x calls the engine makes -----------------------------------------------------
enum GlFn : int {
    GL_BindTexture, GL_BlendFunc, GL_ClearStencil, GL_Color4f, GL_Color4ub, GL_ColorMask,
    GL_ColorPointer, GL_DeleteBuffers, GL_Disable, GL_DisableClientState, GL_DrawArrays,
    GL_Enable, GL_EnableClientState, GL_GenBuffers, GL_GetIntegerv, GL_LoadIdentity,
    GL_MatrixMode, GL_Orthof, GL_PopMatrix, GL_PushMatrix, GL_Rotatef, GL_Scalef,
    GL_StencilFunc, GL_StencilOp, GL_TexCoordPointer, GL_TexEnvi, GL_TexParameterf,
    GL_TexParameteri, GL_Translatef, GL_VertexPointer, GL_FN_COUNT
};
class Engine;
class GlBackend {
public:
    virtual ~GlBackend() = default;
    // a[0..5] are the raw 32-bit argument words (floats arrive as IEEE bits: soft-float ABI).
    virtual void call(Engine& e, GlFn fn, const uint32_t a[6]) = 0;
};

// ---- The engine --------------------------------------------------------------------------------
class Engine {
public:
    Engine(Host* host, GlBackend* gl);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    bool load(const uint8_t* so, size_t size, std::string& err);   // map + relocate the .so
    bool start(int width, int height, std::string& err);           // ctors + init(w,h) + game thread
    void runFrame();      // advance the game logic thread, then render one frame (call on GL thread)
    bool failed() const { return failed_; }
    const std::string& failure() const { return failure_; }

    // Entry points mirroring SampleGameNatives
    int  baseWidth();
    int  baseHeight();
    int  getPart();
    void setAppRequest(int v);
    void touch(int id, float x, float y, float hx, float hy);
    void release(int id, float x, float y);
    void makeAlarmInfo();
    void stopGame();
    void term();

    // Test hooks
    void setClock(std::function<uint64_t()> usNow) { clock_ = std::move(usNow); }
    uint64_t nowUs();
    size_t methodCount() const { return methodIds_.size(); }

    // Guest memory access (used by the GL backend)
    bool rd(uint32_t addr, void* dst, size_t n);
    bool wr(uint32_t addr, const void* src, size_t n);
    uint32_t r32(uint32_t addr);
    void w32(uint32_t addr, uint32_t v);
    std::string cstr(uint32_t addr, size_t maxLen = 1 << 20);

private:
    struct Obj { enum K { Class, String, Bytes } k; std::string s; std::vector<uint8_t> b; bool global = false; };
    struct Ctx { uint32_t r[13]; uint32_t sp, lr, cpsr, pc; };
    struct GThread { uint32_t entry = 0; uint32_t args[4] = {0, 0, 0, 0}; uint32_t spTop = 0; bool started = false, done = false; Ctx ctx{}; int yields = 0; bool yielded = false; };
    // Reads varargs / va_list following the ARM EABI (used by printf and JNI Call*Method)
    class ArgReader {
    public:
        ArgReader(Engine& e, int startReg) : e_(e) {
            for (int k = startReg; k < 4; k++) regs_[nregs_++] = e.reg(k);
            firstReg_ = startReg; sp_ = e.sp();
        }
        ArgReader(Engine& e, uint32_t ap) : e_(e), ap_(ap), useAp_(true) {}
        uint32_t word() {
            if (useAp_) { uint32_t v = e_.r32(ap_); ap_ += 4; return v; }
            if (ri_ < nregs_) return regs_[ri_++];
            return e_.r32(sp_ + 4 * (stackI_++));
        }
        void align8() {
            if (useAp_) { ap_ = (ap_ + 7) & ~7u; return; }
            if (ri_ < nregs_) { if ((firstReg_ + ri_) % 2 == 1) ri_++; return; }
            if (stackI_ % 2 == 1) stackI_++;
        }
        uint32_t i32() { return word(); }
        uint64_t i64() { align8(); uint64_t lo = word(); uint64_t hi = word(); return lo | (hi << 32); }
        double f64() { uint64_t v = i64(); double d; std::memcpy(&d, &v, 8); return d; }
    private:
        Engine& e_;
        uint32_t regs_[4]{}; int nregs_ = 0, ri_ = 0, firstReg_ = 0;
        uint32_t sp_ = 0, stackI_ = 0, ap_ = 0; bool useAp_ = false;
    };
    using Handler = std::function<void()>;

    // setup
    void setupMemory();
    void registerHandlers();
    void setupJni();
    bool fail(const std::string& why);

    // registers
    uint32_t reg(int n);
    void setReg(int n, uint32_t v);
    uint32_t sp();
    uint32_t stackArg(int i);

    // emulation
    uint32_t call(uint32_t fn, std::initializer_list<uint32_t> args, uint64_t maxInsn = 200000000ull);
    uint32_t guestCall(uint32_t fn, uint32_t a0, uint32_t a1);   // nested call from inside a hook
    void runSlice(GThread& t, uint64_t maxInsn);
    void tailCall(uint32_t fn);
    uint32_t sym(const char* name);
    uint32_t jniEntry(const char* name, std::initializer_list<uint32_t> args);
    static void codeHookThunk(uc_engine*, uint64_t addr, uint32_t size, void* user);
    static void kuserHookThunk(uc_engine*, uint64_t addr, uint32_t size, void* user);
    void onCode(uint64_t addr);
    void onKuser(uint64_t addr);

    // memory
    uint32_t gmalloc(uint32_t n);
    void gfree(uint32_t p);
    uint32_t dataAlloc(uint32_t n, uint32_t align = 8);
    uint32_t importAddr(const std::string& name);
    uint32_t dataImport(const std::string& name);

    // libc helpers
    std::string format(const std::string& fmt, ArgReader& a);
    void out(const std::string& s);
    double dbl(int lo, int hi);
    void retDbl(double v);

    // JNI
    uint32_t newHandle(Obj o);
    void jniDispatch(int idx);
    void vmDispatch(int idx);
    void jniCallStatic(int idx);
    void releaseLocalHandles();

    Host* host_;
    GlBackend* gl_;
    uc_engine* uc_ = nullptr;
    bool failed_ = false;
    std::string failure_;
    bool loaded_ = false;
    std::function<uint64_t()> clock_;

    // ELF
    std::vector<uint8_t> image_;
    std::unordered_map<std::string, uint32_t> exports_;
    uint32_t exidxAddr_ = 0, exidxCount_ = 0;
    std::vector<std::string> imports_;
    std::unordered_map<std::string, uint32_t> importIdx_;
    std::unordered_map<std::string, uint32_t> dataSyms_;
    std::vector<Handler> impl_;
    std::vector<int> importGl_;      // GlFn or -1
    std::unordered_map<std::string, Handler> handlers_;
    uint32_t initArray_ = 0, initArraySz_ = 0;

    // heap
    uint32_t heapPtr_;
    std::unordered_map<uint32_t, uint32_t> blockCap_;
    std::unordered_map<uint32_t, std::vector<uint32_t>> freeLists_;
    uint32_t dataPtr_;
    uint32_t tlsBlock_ = 0, errnoAddr_ = 0, tmAddr_ = 0, strerrAddr_ = 0, cLocaleAddr_ = 0;
    uint32_t qsA_ = 0, qsB_ = 0;
    uint32_t nextKey_ = 1;
    uint32_t strtokSave_ = 0;
    std::unordered_map<uint32_t, uint32_t> tls_;

    // JNI
    uint32_t envAddr_ = 0, vmAddr_ = 0, jniTable_ = 0, vmTable_ = 0;
    std::unordered_map<uint32_t, Obj> handles_;
    uint32_t nextHandle_ = 0x1000;
    std::vector<uint32_t> localHandles_;
    std::unordered_map<uint32_t, std::pair<std::string, std::string>> methodIds_;
    uint32_t nextMid_ = 1;
    std::unordered_map<std::string, uint32_t> midByKey_;   // "name|sig" -> method id (the engine re-queries constantly)
    std::unordered_map<uint32_t, uint32_t> pinned_;   // guest ptr -> array handle
    uint32_t clsHandle_ = 0;
    int callDepth_ = 0;
    std::unordered_set<int> warnedJni_;
    std::unordered_set<std::string> warnedImports_;

    // threads
    std::vector<GThread> threads_;
    int curThread_ = -1;
    uint64_t sliceBudget_ = 20000000ull;
    bool started_ = false;
};

}  // namespace tg
