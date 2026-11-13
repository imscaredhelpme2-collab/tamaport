// JNI glue between Kotlin (NativeEngine / GameHost) and the portable emulator core.
#include <jni.h>
#include <android/log.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "engine/engine.h"
#include "engine/gles1.h"

#define LOG_TAG "TamaEngine"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

using tg::JV;

// Forwards the engine's Java callbacks to the Kotlin GameHost object (a regular instance with the same method names).
class AndroidHost : public tg::Host {
public:
    JNIEnv* env = nullptr;     // valid for the current native call (everything runs on the GL thread)
    jobject host = nullptr;    // global ref
    jclass hostCls = nullptr;  // global ref

    JV invoke(const std::string& name, const std::string& sig, std::vector<JV>& args) override {
        JNIEnv* e = env;
        if (!e || !host) return JV::Int_(0);
        const std::string key = name + sig;
        jmethodID mid = nullptr;
        auto it = cache_.find(key);
        if (it != cache_.end()) mid = it->second;
        else {
            mid = e->GetMethodID(hostCls, name.c_str(), sig.c_str());
            if (!mid) { e->ExceptionClear(); if (warned_.insert(key).second) LOGW("host method not implemented: %s%s", name.c_str(), sig.c_str()); }
            cache_[key] = mid;
        }
        if (!mid) return JV::Int_(0);

        std::vector<char> types; char ret;
        parseSig(sig, types, ret);
        std::vector<jvalue> jv(types.size());
        std::vector<jobject> locals;
        for (size_t i = 0; i < types.size(); i++) {
            jv[i].j = 0;
            const JV* a = i < args.size() ? &args[i] : nullptr;
            switch (types[i]) {
                case 'Z': jv[i].z = (a && a->i != 0) ? JNI_TRUE : JNI_FALSE; break;
                case 'B': jv[i].b = (jbyte)(a ? a->i : 0); break;
                case 'C': jv[i].c = (jchar)(a ? a->i : 0); break;
                case 'S': jv[i].s = (jshort)(a ? a->i : 0); break;
                case 'I': jv[i].i = (jint)(a ? a->i : 0); break;
                case 'J': jv[i].j = (jlong)(a ? a->i : 0); break;
                case 'F': jv[i].f = (jfloat)(a ? a->d : 0); break;
                case 'D': jv[i].d = (jdouble)(a ? a->d : 0); break;
                case 'L':
                    if (a && a->t == JV::Str) { jstring s = e->NewStringUTF(a->s.c_str()); jv[i].l = s; locals.push_back(s); }
                    break;
                case '[':
                    if (a && a->t == JV::Bytes) {
                        jbyteArray arr = e->NewByteArray((jsize)a->b.size());
                        if (!a->b.empty()) e->SetByteArrayRegion(arr, 0, (jsize)a->b.size(), reinterpret_cast<const jbyte*>(a->b.data()));
                        jv[i].l = arr; locals.push_back(arr);
                    }
                    break;
            }
        }
        JV out;
        switch (ret) {
            case 'V': e->CallVoidMethodA(host, mid, jv.data()); out = JV::Int_(0); break;
            case 'Z': out = JV::Int_(e->CallBooleanMethodA(host, mid, jv.data()) ? 1 : 0); break;
            case 'B': out = JV::Int_(e->CallByteMethodA(host, mid, jv.data())); break;
            case 'C': out = JV::Int_(e->CallCharMethodA(host, mid, jv.data())); break;
            case 'S': out = JV::Int_(e->CallShortMethodA(host, mid, jv.data())); break;
            case 'I': out = JV::Int_(e->CallIntMethodA(host, mid, jv.data())); break;
            case 'J': out = JV::Long_(e->CallLongMethodA(host, mid, jv.data())); break;
            case 'F': out = JV::Flt(e->CallFloatMethodA(host, mid, jv.data())); break;
            case 'D': out = JV::Dbl(e->CallDoubleMethodA(host, mid, jv.data())); break;
            default: {
                jobject o = e->CallObjectMethodA(host, mid, jv.data());
                if (!o) { out = JV::Null_(); break; }
                if (ret == 'L') {
                    const char* c = e->GetStringUTFChars(static_cast<jstring>(o), nullptr);
                    out = JV::Str_(c ? c : "");
                    if (c) e->ReleaseStringUTFChars(static_cast<jstring>(o), c);
                } else {
                    jsize n = e->GetArrayLength(static_cast<jarray>(o));
                    std::vector<uint8_t> buf((size_t)n);
                    if (n) e->GetByteArrayRegion(static_cast<jbyteArray>(o), 0, n, reinterpret_cast<jbyte*>(buf.data()));
                    out = JV::Bytes_(std::move(buf));
                }
                e->DeleteLocalRef(o);
            }
        }
        for (jobject l : locals) e->DeleteLocalRef(l);
        if (e->ExceptionCheck()) { e->ExceptionDescribe(); e->ExceptionClear(); return JV::Int_(0); }
        return out;
    }

    void log(const std::string& msg) override { LOGI("%s", msg.c_str()); }

private:
    static void parseSig(const std::string& sig, std::vector<char>& args, char& ret) {
        size_t i = 1; args.clear();
        while (i < sig.size() && sig[i] != ')') {
            char c = sig[i];
            if (c == 'L') { i = sig.find(';', i) + 1; args.push_back('L'); }
            else if (c == '[') { while (sig[i] == '[') i++; if (sig[i] == 'L') i = sig.find(';', i) + 1; else i++; args.push_back('['); }
            else { args.push_back(c); i++; }
        }
        ret = sig[i + 1];
        if (ret == '[') ret = '[';   // byte[] is the only array return type the engine uses
    }
    std::unordered_map<std::string, jmethodID> cache_;
    std::unordered_set<std::string> warned_;
};

struct Native {
    AndroidHost host;
    tg::Gles1 gl;
    std::unique_ptr<tg::Engine> engine;
    bool glReady = false;
};
Native* g = nullptr;

jstring toJ(JNIEnv* env, const std::string& s) { return env->NewStringUTF(s.c_str()); }
inline void bind(JNIEnv* env) { if (g) g->host.env = env; }

}  // namespace

extern "C" {

#define FN(ret, name) JNIEXPORT ret JNICALL Java_com_tamaport_classic_NativeEngine_##name

FN(jstring, nativeCreate)(JNIEnv* env, jclass, jobject host, jbyteArray lib) {
    delete g; g = new Native();
    g->host.env = env;
    g->host.host = env->NewGlobalRef(host);
    g->host.hostCls = static_cast<jclass>(env->NewGlobalRef(env->GetObjectClass(host)));
    g->engine = std::make_unique<tg::Engine>(&g->host, &g->gl);
    jsize n = env->GetArrayLength(lib);
    std::vector<uint8_t> data((size_t)n);
    env->GetByteArrayRegion(lib, 0, n, reinterpret_cast<jbyte*>(data.data()));
    std::string err;
    if (!g->engine->load(data.data(), data.size(), err)) return toJ(env, err);
    return nullptr;
}

FN(jboolean, nativeInitGl)(JNIEnv* env, jclass) {
    if (!g) return JNI_FALSE;
    bind(env);
    g->gl.shutdown();
    g->glReady = g->gl.init();
    return g->glReady ? JNI_TRUE : JNI_FALSE;
}

FN(jstring, nativeStart)(JNIEnv* env, jclass, jint w, jint h) {
    if (!g || !g->engine) return toJ(env, "engine not created");
    bind(env);
    std::string err;
    if (!g->engine->start(w, h, err)) return toJ(env, err);
    return nullptr;
}

FN(void, nativeFrame)(JNIEnv* env, jclass, jint sw, jint sh, jint vx, jint vy, jint vw, jint vh) {
    if (!g || !g->engine || !g->glReady) return;
    bind(env);
    g->gl.beginFrame(sw, sh, vx, vy, vw, vh);
    g->engine->runFrame();
}

FN(void, nativeTouch)(JNIEnv* env, jclass, jint id, jfloat x, jfloat y, jfloat hx, jfloat hy) { if (g && g->engine) { bind(env); g->engine->touch(id, x, y, hx, hy); } }
FN(void, nativeRelease)(JNIEnv* env, jclass, jint id, jfloat x, jfloat y) { if (g && g->engine) { bind(env); g->engine->release(id, x, y); } }
FN(jint, nativeGetPart)(JNIEnv* env, jclass) { if (!g || !g->engine) return 0; bind(env); return g->engine->getPart(); }
FN(void, nativeSetAppRequest)(JNIEnv* env, jclass, jint v) { if (g && g->engine) { bind(env); g->engine->setAppRequest(v); } }
FN(void, nativeMakeAlarmInfo)(JNIEnv* env, jclass) { if (g && g->engine) { bind(env); g->engine->makeAlarmInfo(); } }
FN(void, nativeStop)(JNIEnv* env, jclass) { if (g && g->engine) { bind(env); g->engine->stopGame(); } }
FN(void, nativeTerm)(JNIEnv* env, jclass) { if (g && g->engine) { bind(env); g->engine->term(); } }

FN(jstring, nativeFailure)(JNIEnv* env, jclass) {
    if (!g || !g->engine || !g->engine->failed()) return nullptr;
    return toJ(env, g->engine->failure());
}

// 8-byte header (width, height as big-endian int32) followed by RGBA pixels, top row first.
FN(jbyteArray, nativeReadPixels)(JNIEnv* env, jclass) {
    if (!g || !g->glReady) return nullptr;
    std::vector<uint8_t> px; int w = 0, h = 0;
    if (!g->gl.readViewport(px, w, h)) return nullptr;
    jbyteArray out = env->NewByteArray((jsize)(8 + px.size()));
    uint8_t hdr[8] = {uint8_t(w >> 24), uint8_t(w >> 16), uint8_t(w >> 8), uint8_t(w), uint8_t(h >> 24), uint8_t(h >> 16), uint8_t(h >> 8), uint8_t(h)};
    env->SetByteArrayRegion(out, 0, 8, reinterpret_cast<jbyte*>(hdr));
    env->SetByteArrayRegion(out, 8, (jsize)px.size(), reinterpret_cast<jbyte*>(px.data()));
    return out;
}

FN(void, nativeDestroy)(JNIEnv* env, jclass) {
    if (!g) return;
    bind(env);
    g->engine.reset();
    if (g->host.host) env->DeleteGlobalRef(g->host.host);
    if (g->host.hostCls) env->DeleteGlobalRef(g->host.hostCls);
    delete g; g = nullptr;
}

}  // extern "C"
