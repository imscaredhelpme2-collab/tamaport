// JNI half of the engine: a fake JNIEnv/JavaVM that the guest library calls into.
#include "engine.h"

#include <algorithm>
#include <cstring>

namespace tg {

namespace {
constexpr uint32_t BX_LR = 0xE12FFF1E;
constexpr uint32_t JNI_BASE = 0x31000000;
constexpr uint32_t JNI_SLOTS = 240;
constexpr uint32_t VM_BASE = 0x31100000;
constexpr uint32_t THREAD_STACK_BASE = 0x7a000000;
constexpr uint32_t THREAD_STACK_SIZE = 0x100000;
inline int32_t s32(uint32_t x) { return static_cast<int32_t>(x); }

// Splits a JNI method signature into argument type codes ('I','J','F','D','Z','B','C','S','L','[') and a return code.
void parseSig(const std::string& sig, std::vector<char>& args, char& ret) {
    size_t i = 1;
    args.clear();
    while (i < sig.size() && sig[i] != ')') {
        char c = sig[i];
        if (c == 'L') { i = sig.find(';', i) + 1; args.push_back('L'); }
        else if (c == '[') {
            while (sig[i] == '[') i++;
            if (sig[i] == 'L') i = sig.find(';', i) + 1; else i++;
            args.push_back('[');
        } else { args.push_back(c); i++; }
    }
    ret = (i + 1 < sig.size()) ? sig[i + 1] : 'V';
}
}  // namespace

void Engine::setupJni() {
    jniTable_ = dataAlloc(4 * JNI_SLOTS);
    uint32_t bx = BX_LR;
    for (uint32_t i = 0; i < JNI_SLOTS; i++) {
        w32(jniTable_ + 4 * i, JNI_BASE + 4 * i);
        uc_mem_write(uc_, JNI_BASE + 4 * i, &bx, 4);
    }
    envAddr_ = dataAlloc(4); w32(envAddr_, jniTable_);
    vmTable_ = dataAlloc(4 * 8);
    for (uint32_t i = 0; i < 8; i++) {
        w32(vmTable_ + 4 * i, VM_BASE + 4 * i);
        uc_mem_write(uc_, VM_BASE + 4 * i, &bx, 4);
    }
    vmAddr_ = dataAlloc(4); w32(vmAddr_, vmTable_);
}

uint32_t Engine::newHandle(Obj o) {
    uint32_t h = nextHandle_; nextHandle_ += 4;
    if (!o.global && o.k != Obj::Class) localHandles_.push_back(h);
    handles_[h] = std::move(o);
    return h;
}

void Engine::releaseLocalHandles() {
    for (uint32_t h : localHandles_) {
        auto it = handles_.find(h);
        if (it != handles_.end() && !it->second.global) handles_.erase(it);
    }
    localHandles_.clear();
}

void Engine::vmDispatch(int idx) {
    if (idx == 4 || idx == 7 || idx == 6) { w32(reg(1), envAddr_); setReg(0, 0); }   // Attach*/GetEnv
    else setReg(0, 0);
}

void Engine::jniDispatch(int idx) {
    auto ret = [this](uint32_t v) { setReg(0, v); };
    auto getObj = [this](uint32_t h) -> Obj* { auto it = handles_.find(h); return it == handles_.end() ? nullptr : &it->second; };
    switch (idx) {
        case 4: ret(0x00010006); break;                                           // GetVersion
        case 6: ret(newHandle(Obj{Obj::Class, cstr(reg(1)), {}, true})); break;   // FindClass
        case 21: { if (Obj* o = getObj(reg(1))) o->global = true; ret(reg(1)); } break;   // NewGlobalRef
        case 25: ret(reg(1)); break;                                              // NewLocalRef
        case 22: case 23: case 16: case 17: case 217: case 218: break;   // Delete*Ref, Exception*, Monitor*
        case 15: case 26: ret(0); break;                                          // ExceptionOccurred / EnsureLocalCapacity
        case 31: ret(reg(1)); break;                                              // GetObjectClass
        case 33: case 113: {                                                      // GetMethodID / GetStaticMethodID
            std::string nm = cstr(reg(2)), sg = cstr(reg(3)), key = nm + "|" + sg;
            auto f = midByKey_.find(key);
            if (f != midByKey_.end()) { ret(f->second); break; }
            uint32_t mid = nextMid_++;
            methodIds_[mid] = {nm, sg};
            midByKey_[key] = mid;
            ret(mid);
        } break;
        case 164: { Obj* o = getObj(reg(1)); ret(o ? uint32_t(o->s.size()) : 0); } break;          // GetStringLength
        case 167: ret(newHandle(Obj{Obj::String, cstr(reg(1)), {}, false})); break;                // NewStringUTF
        case 168: { Obj* o = getObj(reg(1)); ret(o ? uint32_t(o->s.size()) : 0); } break;           // GetStringUTFLength
        case 169: {                                                                                 // GetStringUTFChars
            Obj* o = getObj(reg(1));
            std::string s = o ? o->s : std::string();
            uint32_t p = gmalloc(uint32_t(s.size()) + 1);
            wr(p, s.c_str(), s.size() + 1);
            if (reg(2)) w32(reg(2), 1);
            ret(p);
        } break;
        case 170: gfree(reg(2)); break;                                                             // ReleaseStringUTFChars
        case 171: { Obj* o = getObj(reg(1)); ret(o ? uint32_t(o->b.size()) : 0); } break;          // GetArrayLength
        case 176: { Obj o{Obj::Bytes, {}, std::vector<uint8_t>(reg(1), 0), false}; ret(newHandle(std::move(o))); } break;   // NewByteArray
        case 184: {                                                                                 // GetByteArrayElements
            Obj* o = getObj(reg(1));
            if (!o) { ret(0); break; }
            uint32_t p = gmalloc(uint32_t(o->b.size()) + 1);
            wr(p, o->b.data(), o->b.size());
            pinned_[p] = reg(1);
            if (reg(2)) w32(reg(2), 1);
            ret(p);
        } break;
        case 192: {                                                                                 // ReleaseByteArrayElements
            uint32_t p = reg(2), mode = reg(3);
            auto it = pinned_.find(p);
            if (it != pinned_.end()) {
                if (Obj* o = getObj(it->second)) if (mode != 2 && !o->b.empty()) rd(p, o->b.data(), o->b.size());
                if (mode != 1) { pinned_.erase(it); gfree(p); }
            }
        } break;
        case 200: {                                                                                 // GetByteArrayRegion
            Obj* o = getObj(reg(1)); uint32_t st = reg(2), ln = reg(3), buf = stackArg(0);
            if (o && st + ln <= o->b.size()) wr(buf, o->b.data() + st, ln);
        } break;
        case 208: {                                                                                 // SetByteArrayRegion
            Obj* o = getObj(reg(1)); uint32_t st = reg(2), ln = reg(3), buf = stackArg(0);
            if (o && st + ln <= o->b.size()) rd(buf, o->b.data() + st, ln);
        } break;
        case 219: w32(reg(1), vmAddr_); ret(0); break;                                              // GetJavaVM
        default:
            if (idx >= 114 && idx <= 143) { jniCallStatic(idx); break; }
            if (warnedJni_.insert(idx).second && host_) host_->log("unimplemented JNI function #" + std::to_string(idx));
            ret(0);
    }
}

void Engine::jniCallStatic(int idx) {
    int form = (idx - 114) % 3;                  // 0 = varargs, 1 = va_list, 2 = jvalue*
    static const char kRet[] = "LZBCSIJFDV";
    char rtype = kRet[(idx - 114) / 3];
    auto mit = methodIds_.find(reg(2));
    if (mit == methodIds_.end()) { setReg(0, 0); return; }
    const std::string name = mit->second.first, sig = mit->second.second;
    std::vector<char> atypes; char sigRet;
    parseSig(sig, atypes, sigRet);
    (void)rtype;

    std::vector<JV> args;
    uint32_t jp = reg(3);
    ArgReader rdr = (form == 0) ? ArgReader(*this, 3) : ArgReader(*this, form == 1 ? reg(3) : 0u);
    for (size_t k = 0; k < atypes.size(); k++) {
        char t = atypes[k];
        JV v;
        if (form == 2) {                         // jvalue array: 8 bytes each
            uint8_t raw[8]; rd(jp + 8 * uint32_t(k), raw, 8);
            if (t == 'D') { double d; std::memcpy(&d, raw, 8); v = JV::Dbl(d); }
            else if (t == 'F') { float f; std::memcpy(&f, raw, 4); v = JV::Flt(f); }
            else if (t == 'J') { int64_t j; std::memcpy(&j, raw, 8); v = JV::Long_(j); }
            else { uint32_t w; std::memcpy(&w, raw, 4); v = (t == 'L' || t == '[') ? JV::Int_(w) : JV::Int_(s32(w)); }
        } else {
            if (t == 'J') v = JV::Long_(int64_t(rdr.i64()));
            else if (t == 'F') v = JV::Flt(rdr.f64());              // float is promoted to double in varargs
            else if (t == 'D') v = JV::Dbl(rdr.f64());
            else if (t == 'L' || t == '[') v = JV::Int_(rdr.i32());
            else v = JV::Int_(s32(rdr.i32()));
        }
        if (t == 'L' || t == '[') {                                 // resolve object handles to values
            uint32_t h = uint32_t(v.i);
            auto it = handles_.find(h);
            if (h == 0 || it == handles_.end()) v = JV::Null_();
            else if (it->second.k == Obj::String) v = JV::Str_(it->second.s);
            else if (it->second.k == Obj::Bytes) v = JV::Bytes_(it->second.b);
            else v = JV::Null_();
        }
        args.push_back(std::move(v));
    }

    JV r;
    if (name == "OnThreadCreate") {              // the engine asks for a Java thread: we schedule a guest thread instead
        GThread t;
        t.entry = sym("Java_com_namcobandaigames_tamagotchisp_SampleGameNatives_ThreadCreate");
        t.args[0] = envAddr_; t.args[1] = clsHandle_;
        t.args[2] = uint32_t(args.size() > 0 ? args[0].i : 0);
        t.args[3] = uint32_t(args.size() > 1 ? args[1].i : 0);
        t.spTop = THREAD_STACK_BASE + uint32_t(threads_.size()) * 0x200000 + THREAD_STACK_SIZE - 0x100;
        uc_mem_map(uc_, THREAD_STACK_BASE + uint32_t(threads_.size()) * 0x200000, THREAD_STACK_SIZE, UC_PROT_ALL);
        threads_.push_back(t);
        r = JV::Int_(int(threads_.size()) - 1);
    } else {
        r = host_->invoke(name, sig, args);
    }

    switch (sigRet) {
        case 'L': case '[':
            if (r.t == JV::Str) setReg(0, newHandle(Obj{Obj::String, r.s, {}, false}));
            else if (r.t == JV::Bytes) setReg(0, newHandle(Obj{Obj::Bytes, {}, std::move(r.b), false}));
            else setReg(0, 0);
            break;
        case 'V': break;
        case 'F': { float f = float(r.d); uint32_t b; std::memcpy(&b, &f, 4); setReg(0, b); } break;
        case 'D': { uint64_t b; std::memcpy(&b, &r.d, 8); setReg(0, uint32_t(b)); setReg(1, uint32_t(b >> 32)); } break;
        case 'J': setReg(0, uint32_t(uint64_t(r.i))); setReg(1, uint32_t(uint64_t(r.i) >> 32)); break;
        case 'Z': setReg(0, r.i != 0 ? 1 : 0); break;
        default: setReg(0, uint32_t(r.i)); break;
    }
}

}  // namespace tg
