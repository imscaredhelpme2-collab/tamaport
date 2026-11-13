// See engine.h. Port of the validated Python feasibility harness to C++.
#include "engine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/time.h>

namespace tg {

namespace {
constexpr uint32_t BASE = 0x10000000;
constexpr uint32_t IMPORT_BASE = 0x30000000;   // one "bx lr" slot per imported function
constexpr uint32_t IMPORT_SIZE = 0x20000;
constexpr uint32_t JNI_BASE = 0x31000000;      // JNIEnv function slots
constexpr uint32_t JNI_SLOTS = 240;
constexpr uint32_t VM_BASE = 0x31100000;       // JavaVM function slots
constexpr uint32_t SENT = 0x33000000;          // return sentinel
constexpr uint32_t DATA_BASE = 0x32000000;
constexpr uint32_t DATA_SIZE = 0x40000;
constexpr uint32_t HEAP_BASE = 0x40000000;
constexpr uint32_t HEAP_SIZE = 0x08000000;     // 128 MB, lazily committed by the OS
constexpr uint32_t STACK_TOP = 0x7f100000;
constexpr uint32_t STACK_SIZE = 0x100000;
constexpr uint32_t THREAD_STACK_BASE = 0x7a000000;
constexpr uint32_t THREAD_STACK_SIZE = 0x100000;
constexpr uint32_t BX_LR = 0xE12FFF1E;
constexpr uint32_t KUSER = 0xFFFF0000;

struct Elf32_Ehdr { uint8_t ident[16]; uint16_t type, machine; uint32_t version, entry, phoff, shoff, flags; uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; };
struct Elf32_Phdr { uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align; };
struct Elf32_Shdr { uint32_t name, type, flags, addr, offset, size, link, info, addralign, entsize; };
struct Elf32_Sym { uint32_t name, value, size; uint8_t info, other; uint16_t shndx; };
struct Elf32_Rel { uint32_t offset, info; };
struct Elf32_Dyn { int32_t tag; uint32_t val; };

inline uint32_t u32(uint64_t x) { return static_cast<uint32_t>(x); }
inline int32_t s32(uint32_t x) { return static_cast<int32_t>(x); }
inline float bitsToFloat(uint32_t b) { float f; std::memcpy(&f, &b, 4); return f; }

const char* const GL_NAMES[GL_FN_COUNT] = {
    "glBindTexture", "glBlendFunc", "glClearStencil", "glColor4f", "glColor4ub", "glColorMask",
    "glColorPointer", "glDeleteBuffers", "glDisable", "glDisableClientState", "glDrawArrays",
    "glEnable", "glEnableClientState", "glGenBuffers", "glGetIntegerv", "glLoadIdentity",
    "glMatrixMode", "glOrthof", "glPopMatrix", "glPushMatrix", "glRotatef", "glScalef",
    "glStencilFunc", "glStencilOp", "glTexCoordPointer", "glTexEnvi", "glTexParameterf",
    "glTexParameteri", "glTranslatef", "glVertexPointer"};
}  // namespace

// ---- lifecycle ----------------------------------------------------------------------------------
Engine::Engine(Host* host, GlBackend* gl) : host_(host), gl_(gl) {
    heapPtr_ = HEAP_BASE;
    dataPtr_ = DATA_BASE + 0x1000;
    uc_err err = uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc_);
    if (err != UC_ERR_OK) { fail("uc_open failed"); return; }
    uc_ctl_set_cpu_model(uc_, UC_CPU_ARM_CORTEX_A9);
    uint32_t cpacr = 0;
    uc_reg_read(uc_, UC_ARM_REG_C1_C0_2, &cpacr);
    cpacr |= (0xFu << 20);
    uc_reg_write(uc_, UC_ARM_REG_C1_C0_2, &cpacr);
    uint32_t fpexc = 0x40000000;
    uc_reg_write(uc_, UC_ARM_REG_FPEXC, &fpexc);
    setupMemory();
    setupJni();
    registerHandlers();
    uc_hook h1, h2;
    uc_hook_add(uc_, &h1, UC_HOOK_CODE, reinterpret_cast<void*>(codeHookThunk), this, IMPORT_BASE, VM_BASE + 0x1000);
    uc_hook_add(uc_, &h2, UC_HOOK_CODE, reinterpret_cast<void*>(kuserHookThunk), this, KUSER + 0xF00, KUSER + 0x1000);
}

Engine::~Engine() { if (uc_) uc_close(uc_); }

bool Engine::fail(const std::string& why) {
    if (!failed_) { failed_ = true; failure_ = why; if (host_) host_->log("ENGINE FAILURE: " + why); }
    if (uc_) uc_emu_stop(uc_);
    return false;
}

uint64_t Engine::nowUs() {
    if (clock_) return clock_();
    struct timeval tv; gettimeofday(&tv, nullptr);
    return uint64_t(tv.tv_sec) * 1000000ull + tv.tv_usec;
}

// ---- memory ------------------------------------------------------------------------------------
void Engine::setupMemory() {
    uc_mem_map(uc_, IMPORT_BASE, IMPORT_SIZE, UC_PROT_ALL);
    uc_mem_map(uc_, JNI_BASE, 0x2000, UC_PROT_ALL);
    uc_mem_map(uc_, VM_BASE, 0x1000, UC_PROT_ALL);
    uc_mem_map(uc_, SENT, 0x1000, UC_PROT_ALL);
    uc_mem_map(uc_, DATA_BASE, DATA_SIZE, UC_PROT_ALL);
    uc_mem_map(uc_, HEAP_BASE, HEAP_SIZE, UC_PROT_ALL);
    uc_mem_map(uc_, STACK_TOP - STACK_SIZE, STACK_SIZE, UC_PROT_ALL);
    uc_mem_map(uc_, KUSER, 0x1000, UC_PROT_ALL);
    uint32_t bx = BX_LR;
    uc_mem_write(uc_, SENT, &bx, 4);
    for (uint32_t off : {0xFA0u, 0xFC0u, 0xFE0u, 0xF60u}) uc_mem_write(uc_, KUSER + off, &bx, 4);
}

bool Engine::rd(uint32_t a, void* d, size_t n) { return n == 0 || uc_mem_read(uc_, a, d, n) == UC_ERR_OK; }
bool Engine::wr(uint32_t a, const void* s, size_t n) { return n == 0 || uc_mem_write(uc_, a, s, n) == UC_ERR_OK; }
uint32_t Engine::r32(uint32_t a) { uint32_t v = 0; rd(a, &v, 4); return v; }
void Engine::w32(uint32_t a, uint32_t v) { wr(a, &v, 4); }
std::string Engine::cstr(uint32_t a, size_t maxLen) {
    std::string out;
    char buf[256];
    while (out.size() < maxLen) {
        size_t n = 256 - (a & 255);   // never cross a 256-byte boundary, so we never read past mapped memory
        if (!rd(a, buf, n)) break;
        for (size_t i = 0; i < n; i++) { if (!buf[i]) return out; out.push_back(buf[i]); }
        a += uint32_t(n);
    }
    return out;
}

uint32_t Engine::dataAlloc(uint32_t n, uint32_t align) {
    dataPtr_ = (dataPtr_ + align - 1) & ~(align - 1);
    uint32_t p = dataPtr_; dataPtr_ += n;
    if (dataPtr_ > DATA_BASE + DATA_SIZE) fail("data region exhausted");
    return p;
}

// Segregated free-list allocator with an 8-byte header holding the block capacity.
static uint32_t classFor(uint32_t n) {
    if (n <= 512) return (n + 15) & ~15u;
    uint32_t c = 1024; while (c < n) c <<= 1; return c;
}
uint32_t Engine::gmalloc(uint32_t n) {
    uint32_t cap = classFor(std::max<uint32_t>(n, 16));
    auto& fl = freeLists_[cap];
    if (!fl.empty()) { uint32_t p = fl.back(); fl.pop_back(); return p; }
    uint32_t p = heapPtr_ + 8;
    if (uint64_t(p) + cap > uint64_t(HEAP_BASE) + HEAP_SIZE) { fail("guest heap exhausted"); return 0; }
    heapPtr_ = p + cap;
    // keep 8-byte alignment of payloads
    heapPtr_ = (heapPtr_ + 7) & ~7u;
    blockCap_[p] = cap;
    return p;
}
void Engine::gfree(uint32_t p) {
    if (!p) return;
    auto it = blockCap_.find(p);
    if (it == blockCap_.end()) return;
    freeLists_[it->second].push_back(p);
}

uint32_t Engine::importAddr(const std::string& name) {
    auto it = importIdx_.find(name);
    if (it != importIdx_.end()) return IMPORT_BASE + 4 * it->second;
    uint32_t idx = uint32_t(imports_.size());
    imports_.push_back(name);
    importIdx_[name] = idx;
    return IMPORT_BASE + 4 * idx;
}

uint32_t Engine::dataImport(const std::string& name) {
    auto it = dataSyms_.find(name);
    if (it != dataSyms_.end()) return it->second;
    uint32_t a = 0;
    if (name == "__sF") { a = dataAlloc(3 * 84); }
    else if (name == "_ctype_") {
        uint8_t tbl[257] = {0};
        for (int c = 0; c < 256; c++) {
            uint8_t f = 0;
            if (c >= 'A' && c <= 'Z') f |= 0x01;
            if (c >= 'a' && c <= 'z') f |= 0x02;
            if (c >= '0' && c <= '9') f |= 0x04;
            if (c == 9 || c == 10 || c == 11 || c == 12 || c == 13 || c == 32) f |= 0x08;
            if ((c >= 33 && c <= 47) || (c >= 58 && c <= 64) || (c >= 91 && c <= 96) || (c >= 123 && c <= 126)) f |= 0x10;
            if (c < 32 || c == 127) f |= 0x20;
            if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) f |= 0x40;
            if (c == 32) f |= 0x80;
            tbl[c + 1] = f;
        }
        a = dataAlloc(257); wr(a, tbl, 257);
    } else if (name == "__stack_chk_guard") { a = dataAlloc(4); w32(a, 0xDEADBEEF); }
    else return 0;
    dataSyms_[name] = a;
    return a;
}

// ---- ELF loading ---------------------------------------------------------------------------------
bool Engine::load(const uint8_t* so, size_t size, std::string& err) {
    if (failed_) { err = failure_; return false; }
    if (size < sizeof(Elf32_Ehdr)) { err = "engine library too small"; return false; }
    const auto* eh = reinterpret_cast<const Elf32_Ehdr*>(so);
    if (std::memcmp(eh->ident, "\x7f""ELF", 4) != 0 || eh->ident[4] != 1 || eh->machine != 40) {
        err = "engine library is not a 32-bit ARM ELF"; return false;
    }
    uint32_t top = 0;
    std::vector<Elf32_Phdr> loads;
    for (int i = 0; i < eh->phnum; i++) {
        Elf32_Phdr ph; std::memcpy(&ph, so + eh->phoff + i * eh->phentsize, sizeof ph);
        if (ph.type == 1) { loads.push_back(ph); top = std::max(top, ph.vaddr + ph.memsz); }
        if (ph.type == 0x70000001) { exidxAddr_ = BASE + ph.vaddr; exidxCount_ = ph.filesz / 8; }
    }
    if (loads.empty()) { err = "no PT_LOAD segments"; return false; }
    uint32_t mapSize = (top + 0xFFF) & ~0xFFFu;
    image_.assign(mapSize, 0);
    for (auto& ph : loads) {
        if (ph.offset + ph.filesz > size) { err = "truncated segment"; return false; }
        std::memcpy(image_.data() + ph.vaddr, so + ph.offset, ph.filesz);
    }
    // dynamic section
    uint32_t symtab = 0, strtab = 0, rel = 0, relsz = 0, jmprel = 0, pltrelsz = 0, nsyms = 0;
    for (int i = 0; i < eh->phnum; i++) {
        Elf32_Phdr ph; std::memcpy(&ph, so + eh->phoff + i * eh->phentsize, sizeof ph);
        if (ph.type != 2) continue;
        for (uint32_t o = 0; o + 8 <= ph.memsz; o += 8) {
            Elf32_Dyn d; std::memcpy(&d, image_.data() + ph.vaddr + o, 8);
            if (d.tag == 0) break;
            switch (d.tag) {
                case 5: strtab = d.val; break;
                case 6: symtab = d.val; break;
                case 17: rel = d.val; break;
                case 18: relsz = d.val; break;
                case 23: jmprel = d.val; break;
                case 2: pltrelsz = d.val; break;
                case 25: initArray_ = d.val; break;
                case 27: initArraySz_ = d.val; break;
                case 4: { uint32_t nchain; std::memcpy(&nchain, image_.data() + d.val + 4, 4); nsyms = nchain; } break;
            }
        }
    }
    if (!symtab || !strtab) { err = "no dynamic symbol table"; return false; }
    if (!nsyms && eh->shoff) {  // use section headers when there's no DT_HASH
        for (int i = 0; i < eh->shnum; i++) {
            Elf32_Shdr sh; std::memcpy(&sh, so + eh->shoff + i * eh->shentsize, sizeof sh);
            if (sh.type == 11) nsyms = sh.size / sizeof(Elf32_Sym);
        }
    }
    if (!nsyms) { err = "cannot determine symbol count"; return false; }
    auto symAt = [&](uint32_t i) { Elf32_Sym s; std::memcpy(&s, image_.data() + symtab + i * sizeof s, sizeof s); return s; };
    auto symName = [&](const Elf32_Sym& s) { return std::string(reinterpret_cast<const char*>(image_.data() + strtab + s.name)); };
    for (uint32_t i = 0; i < nsyms; i++) {
        Elf32_Sym s = symAt(i);
        if (s.shndx != 0 && s.name) exports_[symName(s)] = BASE + s.value;
    }
    auto applyRel = [&](uint32_t tab, uint32_t sz) -> bool {
        for (uint32_t o = 0; o + 8 <= sz; o += 8) {
            Elf32_Rel r; std::memcpy(&r, image_.data() + tab + o, 8);
            uint32_t type = r.info & 0xFF, si = r.info >> 8;
            if (r.offset + 4 > image_.size()) { err = "relocation out of range"; return false; }
            uint32_t* where = reinterpret_cast<uint32_t*>(image_.data() + r.offset);
            if (type == 23) { *where += BASE; }
            else if (type == 21 || type == 22 || type == 2) {
                Elf32_Sym s = symAt(si);
                uint32_t val;
                if (s.shndx != 0) val = BASE + s.value;
                else {
                    std::string nm = symName(s);
                    uint32_t d = dataImport(nm);
                    val = d ? d : importAddr(nm);
                }
                if (type == 2) val += *where;
                *where = val;
            } else if (type != 0) { err = "unhandled relocation type " + std::to_string(type); return false; }
        }
        return true;
    };
    if (rel && !applyRel(rel, relsz)) return false;
    if (jmprel && !applyRel(jmprel, pltrelsz)) return false;
    if (imports_.size() * 4 > IMPORT_SIZE) { err = "too many imports"; return false; }

    if (uc_mem_map(uc_, BASE, mapSize, UC_PROT_ALL) != UC_ERR_OK) { err = "cannot map engine image"; return false; }
    uc_mem_write(uc_, BASE, image_.data(), image_.size());
    // import stubs + handlers
    impl_.assign(imports_.size(), Handler());
    importGl_.assign(imports_.size(), -1);
    uint32_t bx = BX_LR;
    for (size_t i = 0; i < imports_.size(); i++) {
        uc_mem_write(uc_, IMPORT_BASE + uint32_t(i) * 4, &bx, 4);
        const std::string& n = imports_[i];
        auto h = handlers_.find(n);
        if (h != handlers_.end()) { impl_[i] = h->second; continue; }
        bool isGl = false;
        for (int g = 0; g < GL_FN_COUNT; g++) if (n == GL_NAMES[g]) { importGl_[i] = g; isGl = true; break; }
        if (!isGl) {
            host_->log("unimplemented import: " + n);
            impl_[i] = [this] { setReg(0, 0); };
        }
    }
    loaded_ = true;
    return true;
}

uint32_t Engine::sym(const char* name) {
    auto it = exports_.find(name);
    return it == exports_.end() ? 0 : it->second;
}

// ---- registers / calls ---------------------------------------------------------------------------
uint32_t Engine::reg(int n) { uint32_t v = 0; uc_reg_read(uc_, UC_ARM_REG_R0 + n, &v); return v; }
void Engine::setReg(int n, uint32_t v) { uc_reg_write(uc_, UC_ARM_REG_R0 + n, &v); }
uint32_t Engine::sp() { uint32_t v = 0; uc_reg_read(uc_, UC_ARM_REG_SP, &v); return v; }
uint32_t Engine::stackArg(int i) { return r32(sp() + 4 * i); }

uint32_t Engine::call(uint32_t fn, std::initializer_list<uint32_t> args, uint64_t maxInsn) {
    if (failed_) return 0;
    callDepth_++;
    int i = 0;
    uint32_t stackTop = STACK_TOP - 0x100;
    size_t extra = args.size() > 4 ? args.size() - 4 : 0;
    stackTop -= 4 * uint32_t(extra);
    stackTop &= ~7u;
    for (uint32_t a : args) {
        if (i < 4) setReg(i, a); else w32(stackTop + 4 * (i - 4), a);
        i++;
    }
    uc_reg_write(uc_, UC_ARM_REG_SP, &stackTop);
    uint32_t lr = SENT; uc_reg_write(uc_, UC_ARM_REG_LR, &lr);
    uc_err e = uc_emu_start(uc_, fn, SENT, 0, maxInsn);
    uint32_t r = 0;
    if (!failed_) {
        if (e != UC_ERR_OK) {
            uint32_t pc = 0; uc_reg_read(uc_, UC_ARM_REG_PC, &pc);
            fail(std::string("emulation error: ") + uc_strerror(e) + " at pc=" + std::to_string(pc - BASE));
        } else {
            uint32_t pc = 0; uc_reg_read(uc_, UC_ARM_REG_PC, &pc);
            if (pc != SENT) fail("call did not return (instruction budget exhausted)");
            r = reg(0);
        }
    }
    callDepth_--;
    if (callDepth_ == 0) releaseLocalHandles();
    return r;
}

uint32_t Engine::guestCall(uint32_t fn, uint32_t a0, uint32_t a1) {
    uint32_t saved[16];
    for (int i = 0; i < 13; i++) saved[i] = reg(i);
    saved[13] = sp();
    uc_reg_read(uc_, UC_ARM_REG_LR, &saved[14]);
    uc_reg_read(uc_, UC_ARM_REG_CPSR, &saved[15]);
    setReg(0, a0); setReg(1, a1);
    uint32_t nsp = (saved[13] - 0x200) & ~7u;
    uc_reg_write(uc_, UC_ARM_REG_SP, &nsp);
    uint32_t lr = SENT; uc_reg_write(uc_, UC_ARM_REG_LR, &lr);
    uc_err e = uc_emu_start(uc_, fn, SENT, 0, 10000000);
    uint32_t r = reg(0);
    if (e != UC_ERR_OK) fail(std::string("nested call error: ") + uc_strerror(e));
    for (int i = 0; i < 13; i++) setReg(i, saved[i]);
    uc_reg_write(uc_, UC_ARM_REG_SP, &saved[13]);
    uc_reg_write(uc_, UC_ARM_REG_LR, &saved[14]);
    uc_reg_write(uc_, UC_ARM_REG_CPSR, &saved[15]);
    // NOTE: PC is deliberately not restored; writing it would re-run the import hook.
    return r;
}

void Engine::tailCall(uint32_t fn) {
    uint32_t cpsr = 0; uc_reg_read(uc_, UC_ARM_REG_CPSR, &cpsr);
    if (fn & 1) cpsr |= 0x20; else cpsr &= ~0x20u;
    uc_reg_write(uc_, UC_ARM_REG_CPSR, &cpsr);
    uint32_t pc = fn & ~1u; uc_reg_write(uc_, UC_ARM_REG_PC, &pc);
}

uint32_t Engine::jniEntry(const char* name, std::initializer_list<uint32_t> args) {
    uint32_t fn = sym(name);
    if (!fn) { fail(std::string("missing export ") + name); return 0; }
    std::vector<uint32_t> v{envAddr_, clsHandle_};
    v.insert(v.end(), args.begin(), args.end());
    // std::initializer_list can't be built from a vector, so dispatch by size.
    switch (v.size()) {
        case 2: return call(fn, {v[0], v[1]});
        case 3: return call(fn, {v[0], v[1], v[2]});
        case 4: return call(fn, {v[0], v[1], v[2], v[3]});
        case 5: return call(fn, {v[0], v[1], v[2], v[3], v[4]});
        case 6: return call(fn, {v[0], v[1], v[2], v[3], v[4], v[5]});
        case 7: return call(fn, {v[0], v[1], v[2], v[3], v[4], v[5], v[6]});
    }
    return 0;
}

// ---- hooks -------------------------------------------------------------------------------------
void Engine::codeHookThunk(uc_engine*, uint64_t addr, uint32_t, void* user) { static_cast<Engine*>(user)->onCode(addr); }
void Engine::kuserHookThunk(uc_engine*, uint64_t addr, uint32_t, void* user) { static_cast<Engine*>(user)->onKuser(addr); }

void Engine::onKuser(uint64_t addr) {
    if (addr == KUSER + 0xFC0) {            // __kuser_cmpxchg(old, new, ptr)
        uint32_t oldv = reg(0), newv = reg(1), ptr = reg(2);
        uint32_t cpsr = 0; uc_reg_read(uc_, UC_ARM_REG_CPSR, &cpsr);
        if (r32(ptr) == oldv) { w32(ptr, newv); setReg(0, 0); cpsr |= (1u << 29); }
        else { setReg(0, 1); cpsr &= ~(1u << 29); }
        uc_reg_write(uc_, UC_ARM_REG_CPSR, &cpsr);
    } else if (addr == KUSER + 0xFE0) {     // __kuser_get_tls
        if (!tlsBlock_) tlsBlock_ = dataAlloc(256);
        setReg(0, tlsBlock_);
    }
}

void Engine::onCode(uint64_t addr) {
    if (addr >= IMPORT_BASE && addr < IMPORT_BASE + IMPORT_SIZE) {
        size_t idx = (addr - IMPORT_BASE) / 4;
        if (idx >= impl_.size()) return;
        if (importGl_[idx] >= 0) {
            if (gl_) {
                uint32_t a[6];
                for (int i = 0; i < 4; i++) a[i] = reg(i);
                a[4] = stackArg(0); a[5] = stackArg(1);
                gl_->call(*this, GlFn(importGl_[idx]), a);
            }
            setReg(0, 0);
            return;
        }
        impl_[idx]();
    } else if (addr >= JNI_BASE && addr < JNI_BASE + JNI_SLOTS * 4) {
        jniDispatch(int((addr - JNI_BASE) / 4));
    } else if (addr >= VM_BASE && addr < VM_BASE + 0x100) {
        vmDispatch(int((addr - VM_BASE) / 4));
    }
}

// ---- cooperative threads -----------------------------------------------------------------------
void Engine::runSlice(GThread& t, uint64_t maxInsn) {
    if (failed_ || t.done) return;
    curThread_ = int(&t - threads_.data());
    t.yielded = false;
    uint32_t begin;
    if (!t.started) {
        t.started = true;
        for (int i = 0; i < 4; i++) setReg(i, t.args[i]);
        uint32_t s = t.spTop; uc_reg_write(uc_, UC_ARM_REG_SP, &s);
        uint32_t lr = SENT; uc_reg_write(uc_, UC_ARM_REG_LR, &lr);
        begin = t.entry;
    } else {
        for (int i = 0; i < 13; i++) setReg(i, t.ctx.r[i]);
        uc_reg_write(uc_, UC_ARM_REG_SP, &t.ctx.sp);
        uc_reg_write(uc_, UC_ARM_REG_LR, &t.ctx.lr);
        uc_reg_write(uc_, UC_ARM_REG_CPSR, &t.ctx.cpsr);
        begin = t.ctx.pc | ((t.ctx.cpsr & 0x20) ? 1u : 0u);
    }
    callDepth_++;
    uc_err e = uc_emu_start(uc_, begin, SENT, 0, maxInsn);
    callDepth_--;
    curThread_ = -1;
    if (failed_) return;
    if (e != UC_ERR_OK) {
        uint32_t pc = 0; uc_reg_read(uc_, UC_ARM_REG_PC, &pc);
        fail(std::string("logic thread error: ") + uc_strerror(e) + " at pc=" + std::to_string(pc - BASE));
        return;
    }
    if (t.yielded) return;                 // usleep() already saved the context
    uint32_t pc = 0; uc_reg_read(uc_, UC_ARM_REG_PC, &pc);
    if (pc == SENT) { t.done = true; return; }
    // Budget exhausted without yielding: save and resume next frame.
    for (int i = 0; i < 13; i++) t.ctx.r[i] = reg(i);
    t.ctx.sp = sp();
    uc_reg_read(uc_, UC_ARM_REG_LR, &t.ctx.lr);
    uc_reg_read(uc_, UC_ARM_REG_CPSR, &t.ctx.cpsr);
    t.ctx.pc = pc;
}

bool Engine::start(int w, int h, std::string& err) {
    if (!loaded_) { err = "engine not loaded"; return false; }
    if (failed_) { err = failure_; return false; }
    uint32_t cls = newHandle(Obj{Obj::Class, "com/namcobandaigames/tamagotchisp/SampleGameNatives", {}, true});
    clsHandle_ = cls;
    for (uint32_t i = 0; i < initArraySz_ / 4; i++) {
        uint32_t fn = r32(BASE + initArray_ + 4 * i);
        if (fn == 0 || fn == 0xFFFFFFFFu) continue;
        call(fn, {});
        if (failed_) { err = failure_; return false; }
    }
    jniEntry("Java_com_namcobandaigames_tamagotchisp_SampleGameNatives_init", {uint32_t(w), uint32_t(h)});
    if (failed_) { err = failure_; return false; }
    started_ = true;
    return true;
}

void Engine::runFrame() {
    if (!started_ || failed_) return;
    static uint64_t lastUs = 0;
    uint64_t now = nowUs();
    int slices = 1;
    if (lastUs) {
        int64_t dt = int64_t(now - lastUs);
        if (dt > 33000) slices = 1 + int(std::min<int64_t>(3, (dt - 16667) / 16667));
    }
    lastUs = now;
    for (int s = 0; s < slices; s++)
        for (auto& t : threads_) runSlice(t, sliceBudget_);
    jniEntry("Java_com_namcobandaigames_tamagotchisp_SampleGameNatives_step", {});
}

// ---- JNI entry points --------------------------------------------------------------------------
int Engine::baseWidth()  { return int(jniEntry("Java_com_namcobandaigames_tamagotchisp_SampleGameNatives_GetBaseScreenWidth", {})); }
int Engine::baseHeight() { return int(jniEntry("Java_com_namcobandaigames_tamagotchisp_SampleGameNatives_GetBaseScreenHeight", {})); }
int Engine::getPart()    { return int(jniEntry("Java_com_namcobandaigames_tamagotchisp_SampleGameNatives_GameGetPart", {})); }
void Engine::setAppRequest(int v) { jniEntry("Java_com_namcobandaigames_tamagotchisp_SampleGameNatives_GameSetAppRequest", {uint32_t(v)}); }
void Engine::makeAlarmInfo() { jniEntry("Java_com_namcobandaigames_tamagotchisp_SampleGameNatives_TMGCmakeAlarmInfoData", {}); }
void Engine::stopGame() { jniEntry("Java_com_namcobandaigames_tamagotchisp_SampleGameNatives_stop", {}); }
void Engine::term() { jniEntry("Java_com_namcobandaigames_tamagotchisp_SampleGameNatives_GameTerm", {}); }
void Engine::touch(int id, float x, float y, float hx, float hy) {
    uint32_t b[4]; std::memcpy(&b[0], &x, 4); std::memcpy(&b[1], &y, 4); std::memcpy(&b[2], &hx, 4); std::memcpy(&b[3], &hy, 4);
    jniEntry("Java_com_namcobandaigames_tamagotchisp_SampleGameNatives_GameInputSetTouches", {uint32_t(id), b[0], b[1], b[2], b[3]});
}
void Engine::release(int id, float x, float y) {
    uint32_t b[2]; std::memcpy(&b[0], &x, 4); std::memcpy(&b[1], &y, 4);
    jniEntry("Java_com_namcobandaigames_tamagotchisp_SampleGameNatives_GameInputReleaseTouches", {uint32_t(id), b[0], b[1]});
}

// ---- printf --------------------------------------------------------------------------------------
std::string Engine::format(const std::string& fmt, ArgReader& a) {
    std::string out;
    size_t i = 0;
    while (i < fmt.size()) {
        char c = fmt[i];
        if (c != '%') { out.push_back(c); i++; continue; }
        i++;
        std::string flags;
        while (i < fmt.size() && std::strchr("-+ 0#", fmt[i])) flags.push_back(fmt[i++]);
        std::string width;
        if (i < fmt.size() && fmt[i] == '*') { width = std::to_string(s32(a.i32())); i++; }
        else while (i < fmt.size() && isdigit((unsigned char)fmt[i])) width.push_back(fmt[i++]);
        int prec = -1;
        if (i < fmt.size() && fmt[i] == '.') {
            i++; prec = 0;
            if (i < fmt.size() && fmt[i] == '*') { prec = s32(a.i32()); i++; }
            else while (i < fmt.size() && isdigit((unsigned char)fmt[i])) prec = prec * 10 + (fmt[i++] - '0');
        }
        int lng = 0;
        while (i < fmt.size() && std::strchr("hlzjtL", fmt[i])) { if (fmt[i] != 'h') lng++; i++; }
        char conv = i < fmt.size() ? fmt[i] : '%'; i++;
        std::string spec = "%" + flags + width + (prec >= 0 ? "." + std::to_string(prec) : "");
        char buf[512];
        switch (conv) {
            case '%': out.push_back('%'); break;
            case 'd': case 'i':
                if (lng >= 2) { std::snprintf(buf, sizeof buf, (spec + "lld").c_str(), (long long)a.i64()); }
                else std::snprintf(buf, sizeof buf, (spec + "d").c_str(), (int)s32(a.i32()));
                out += buf; break;
            case 'u': case 'x': case 'X': case 'o':
                if (lng >= 2) { std::snprintf(buf, sizeof buf, (spec + "ll" + conv).c_str(), (unsigned long long)a.i64()); }
                else std::snprintf(buf, sizeof buf, (spec + conv).c_str(), (unsigned)a.i32());
                out += buf; break;
            case 'c': { char ch = char(a.i32() & 0xFF); std::snprintf(buf, sizeof buf, (spec + "c").c_str(), ch); out += buf; } break;
            case 's': {
                uint32_t p = a.i32();
                std::string s = p ? cstr(p) : "(null)";
                if (prec >= 0 && size_t(prec) < s.size()) s.resize(prec);
                size_t w = width.empty() ? 0 : size_t(std::atoi(width.c_str()));
                if (s.size() < w) { if (flags.find('-') != std::string::npos) s.append(w - s.size(), ' '); else s.insert(0, w - s.size(), ' '); }
                out += s;
            } break;
            case 'p': std::snprintf(buf, sizeof buf, "0x%x", a.i32()); out += buf; break;
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
                std::snprintf(buf, sizeof buf, (spec + conv).c_str(), a.f64()); out += buf; break;
            default: out.push_back('%'); out.push_back(conv);
        }
    }
    return out;
}
void Engine::out(const std::string& s) { if (host_ && !s.empty()) host_->log(s); }
double Engine::dbl(int lo, int hi) { uint64_t v = uint64_t(reg(lo)) | (uint64_t(reg(hi)) << 32); double d; std::memcpy(&d, &v, 8); return d; }
void Engine::retDbl(double d) { uint64_t v; std::memcpy(&v, &d, 8); setReg(1, uint32_t(v >> 32)); setReg(0, uint32_t(v)); }

// ---- libc / runtime shims ---------------------------------------------------------------------
void Engine::registerHandlers() {
    auto H = [this](const char* n, Handler f) { handlers_[n] = std::move(f); };
    auto ret = [this](uint32_t v) { setReg(0, v); };

    H("malloc", [=] { ret(gmalloc(reg(0))); });
    H("free", [=] { gfree(reg(0)); });
    H("realloc", [=] {
        uint32_t p = reg(0), n = reg(1);
        if (p) { auto it = blockCap_.find(p); if (it != blockCap_.end() && n <= it->second) { ret(p); return; } }
        uint32_t q = gmalloc(n);
        if (p && q) {
            auto it = blockCap_.find(p);
            uint32_t old = it == blockCap_.end() ? 0 : it->second;
            uint32_t m = std::min(old, n);
            std::vector<uint8_t> tmp(m); rd(p, tmp.data(), m); wr(q, tmp.data(), m);
            gfree(p);
        }
        ret(q);
    });
    auto memcpyH = [=] {
        uint32_t d = reg(0), s = reg(1), n = reg(2);
        if (n) { std::vector<uint8_t> tmp(n); rd(s, tmp.data(), n); wr(d, tmp.data(), n); }
        ret(d);
    };
    H("memcpy", memcpyH); H("memmove", memcpyH);
    H("memset", [=] {
        uint32_t d = reg(0), n = reg(2); uint8_t v = uint8_t(reg(1));
        if (n) { std::vector<uint8_t> tmp(n, v); wr(d, tmp.data(), n); }
        ret(d);
    });
    H("memcmp", [=] {
        uint32_t n = reg(2);
        std::vector<uint8_t> a(n), b(n); rd(reg(0), a.data(), n); rd(reg(1), b.data(), n);
        int c = n ? std::memcmp(a.data(), b.data(), n) : 0;
        ret(c == 0 ? 0 : (c > 0 ? 1 : 0xFFFFFFFFu));
    });
    H("memchr", [=] {
        uint32_t p = reg(0), n = reg(2); uint8_t c = uint8_t(reg(1));
        std::vector<uint8_t> a(n); rd(p, a.data(), n);
        auto* f = n ? static_cast<uint8_t*>(std::memchr(a.data(), c, n)) : nullptr;
        ret(f ? p + uint32_t(f - a.data()) : 0);
    });
    H("strlen", [=] { ret(uint32_t(cstr(reg(0)).size())); });
    auto cmp = [](const std::string& a, const std::string& b) { int c = a.compare(b); return c == 0 ? 0u : (c > 0 ? 1u : 0xFFFFFFFFu); };
    H("strcmp", [=] { ret(cmp(cstr(reg(0)), cstr(reg(1)))); });
    H("strcoll", [=] { ret(cmp(cstr(reg(0)), cstr(reg(1)))); });
    H("strncmp", [=] { size_t n = reg(2); ret(cmp(cstr(reg(0)).substr(0, n), cstr(reg(1)).substr(0, n))); });
    H("strcpy", [=] { std::string s = cstr(reg(1)); wr(reg(0), s.c_str(), s.size() + 1); ret(reg(0)); });
    H("strcat", [=] { uint32_t d = reg(0); std::string s = cstr(reg(1)); wr(d + uint32_t(cstr(d).size()), s.c_str(), s.size() + 1); ret(d); });
    H("strstr", [=] { uint32_t h = reg(0); auto p = cstr(h).find(cstr(reg(1))); ret(p == std::string::npos ? 0 : h + uint32_t(p)); });
    H("strxfrm", [=] {
        std::string s = cstr(reg(1)); uint32_t d = reg(0), n = reg(2);
        if (d && n) { std::string o = s.substr(0, n - 1); wr(d, o.c_str(), o.size() + 1); }
        ret(uint32_t(s.size()));
    });
    H("strtol", [=] {
        uint32_t p = reg(0); std::string s = cstr(p); int base = int(reg(2));
        char* end = nullptr; long v = std::strtol(s.c_str(), &end, base);
        if (reg(1)) w32(reg(1), p + uint32_t(end - s.c_str()));
        ret(uint32_t(v));
    });
    H("strtod", [=] {
        uint32_t p = reg(0); std::string s = cstr(p);
        char* end = nullptr; double v = std::strtod(s.c_str(), &end);
        if (reg(1)) w32(reg(1), p + uint32_t(end - s.c_str()));
        retDbl(v);
    });
    H("strtok", [=] {
        uint32_t s = reg(0); std::string delim = cstr(reg(1));
        if (s) strtokSave_ = s;
        uint32_t p = strtokSave_;
        if (!p) { ret(0); return; }
        auto isDelim = [&](char ch) { return delim.find(ch) != std::string::npos; };
        char ch = 0;
        for (;;) { rd(p, &ch, 1); if (!ch) { strtokSave_ = 0; ret(0); return; } if (!isDelim(ch)) break; p++; }
        uint32_t start = p;
        for (;;) {
            rd(p, &ch, 1);
            if (!ch) { strtokSave_ = 0; ret(start); return; }
            if (isDelim(ch)) { char z = 0; wr(p, &z, 1); strtokSave_ = p + 1; ret(start); return; }
            p++;
        }
    });
    H("sscanf", [=] {   // supports the common conversions: %d %i %u %x %f %g %e %s %c %n and literal text
        std::string in = cstr(reg(0)), fmt = cstr(reg(1));
        ArgReader a(*this, 2);
        size_t ip = 0; int assigned = 0;
        for (size_t fp = 0; fp < fmt.size(); fp++) {
            char f = fmt[fp];
            if (isspace((unsigned char)f)) { while (ip < in.size() && isspace((unsigned char)in[ip])) ip++; continue; }
            if (f != '%') { if (ip < in.size() && in[ip] == f) { ip++; continue; } break; }
            fp++; bool suppress = false; if (fp < fmt.size() && fmt[fp] == '*') { suppress = true; fp++; }
            int width = 0; while (fp < fmt.size() && isdigit((unsigned char)fmt[fp])) width = width * 10 + (fmt[fp++] - '0');
            int lng = 0; while (fp < fmt.size() && std::strchr("hlLz", fmt[fp])) { if (fmt[fp] != 'h') lng++; fp++; }
            char conv = fp < fmt.size() ? fmt[fp] : 0;
            if (conv != 'c' && conv != 'n') while (ip < in.size() && isspace((unsigned char)in[ip])) ip++;
            if (conv == 'n') { if (!suppress) w32(a.i32(), uint32_t(ip)); continue; }
            if (ip >= in.size()) { if (!assigned) assigned = -1; break; }
            const char* start = in.c_str() + ip; char* end = nullptr;
            if (conv == 'd' || conv == 'i' || conv == 'u' || conv == 'x' || conv == 'X') {
                long v = (conv == 'x' || conv == 'X') ? std::strtol(start, &end, 16) : std::strtol(start, &end, conv == 'i' ? 0 : 10);
                if (end == start) break;
                ip += size_t(end - start); if (!suppress) { w32(a.i32(), uint32_t(v)); assigned++; }
            } else if (conv == 'f' || conv == 'g' || conv == 'e' || conv == 'E') {
                double v = std::strtod(start, &end);
                if (end == start) break;
                ip += size_t(end - start);
                if (!suppress) {
                    uint32_t p = a.i32();
                    if (lng) wr(p, &v, 8); else { float fv = float(v); wr(p, &fv, 4); }
                    assigned++;
                }
            } else if (conv == 's') {
                size_t e2 = ip; while (e2 < in.size() && !isspace((unsigned char)in[e2]) && (!width || int(e2 - ip) < width)) e2++;
                if (!suppress) { std::string t = in.substr(ip, e2 - ip); uint32_t p = a.i32(); wr(p, t.c_str(), t.size() + 1); assigned++; }
                ip = e2;
            } else if (conv == 'c') {
                if (!suppress) { uint32_t p = a.i32(); wr(p, &in[ip], 1); assigned++; }
                ip++;
            } else break;
        }
        ret(uint32_t(assigned));
    });
    H("snprintf", [=] {
        uint32_t d = reg(0), n = reg(1);
        ArgReader a(*this, 3); std::string s = format(cstr(reg(2)), a);
        if (n) { std::string o = s.substr(0, n - 1); wr(d, o.c_str(), o.size() + 1); }
        ret(uint32_t(s.size()));
    });
    H("sprintf", [=] { ArgReader a(*this, 2); std::string s = format(cstr(reg(1)), a); wr(reg(0), s.c_str(), s.size() + 1); ret(uint32_t(s.size())); });
    H("vsnprintf", [=] {
        uint32_t d = reg(0), n = reg(1);
        ArgReader a(*this, reg(3)); std::string s = format(cstr(reg(2)), a);
        if (n) { std::string o = s.substr(0, n - 1); wr(d, o.c_str(), o.size() + 1); }
        ret(uint32_t(s.size()));
    });
    H("vsprintf", [=] { ArgReader a(*this, reg(2)); std::string s = format(cstr(reg(1)), a); wr(reg(0), s.c_str(), s.size() + 1); ret(uint32_t(s.size())); });
    H("printf", [=] { ArgReader a(*this, 1); std::string s = format(cstr(reg(0)), a); out(s); ret(uint32_t(s.size())); });
    H("puts", [=] { out(cstr(reg(0))); ret(1); });
    H("fputs", [=] { out(cstr(reg(0))); ret(1); });
    for (const char* n : {"fputc", "putc", "putwc"}) H(n, [=] { ret(reg(0)); });
    H("fwrite", [=] { ret(reg(2)); });
    for (const char* n : {"fflush", "setvbuf", "fopen", "fdopen", "fclose", "fread"}) H(n, [=] { ret(0); });
    for (const char* n : {"fseek", "ftell", "getc", "ungetc", "getwc", "ungetwc", "read", "lseek", "ioctl"}) H(n, [=] { ret(0xFFFFFFFFu); });
    H("write", [=] { ret(reg(2)); });
    H("abort", [=] { fail("abort() called"); });
    H("raise", [=] { fail("raise(" + std::to_string(reg(0)) + ") called"); });
    H("exit", [=] { fail("exit(" + std::to_string(reg(0)) + ") called"); });
    H("__stack_chk_fail", [=] { fail("stack smashing detected"); });
    H("__errno", [=] { if (!errnoAddr_) errnoAddr_ = dataAlloc(4); ret(errnoAddr_); });
    H("strerror", [=] { if (!strerrAddr_) { strerrAddr_ = dataAlloc(8); wr(strerrAddr_, "error", 6); } ret(strerrAddr_); });
    H("setlocale", [=] { if (!cLocaleAddr_) { cLocaleAddr_ = dataAlloc(4); wr(cLocaleAddr_, "C", 2); } ret(cLocaleAddr_); });
    H("__cxa_atexit", [=] { ret(0); });
    H("__cxa_finalize", [=] { ret(0); });
    H("__gnu_Unwind_Find_exidx", [=] { w32(reg(1), exidxCount_); ret(exidxAddr_); });

    // time (real wall clock; the pet ages in real time)
    H("time", [=] { uint32_t t = uint32_t(nowUs() / 1000000ull); if (reg(0)) w32(reg(0), t); ret(t); });
    H("gettimeofday", [=] {
        uint64_t us = nowUs(); uint32_t p = reg(0);
        if (p) { uint32_t tv[2] = {uint32_t(us / 1000000ull), uint32_t(us % 1000000ull)}; wr(p, tv, 8); }
        ret(0);
    });
    H("ftime", [=] { ret(0); });
    H("usleep", [=] {
        if (curThread_ >= 0) {                   // yield the logic thread back to the render loop
            GThread& t = threads_[curThread_];
            uint32_t lr = 0, cpsr = 0; uc_reg_read(uc_, UC_ARM_REG_LR, &lr); uc_reg_read(uc_, UC_ARM_REG_CPSR, &cpsr);
            for (int i = 0; i < 13; i++) t.ctx.r[i] = reg(i);
            t.ctx.r[0] = 0;
            t.ctx.sp = sp(); t.ctx.lr = lr;
            t.ctx.cpsr = (lr & 1) ? (cpsr | 0x20) : (cpsr & ~0x20u);
            t.ctx.pc = lr & ~1u;
            t.yielded = true; t.yields++;
            uc_emu_stop(uc_);
        }
        ret(0);
    });
    H("localtime", [=] {
        if (!tmAddr_) tmAddr_ = dataAlloc(44);
        time_t tt = time_t(r32(reg(0))); struct tm t; localtime_r(&tt, &t);
        uint32_t v[9] = {uint32_t(t.tm_sec), uint32_t(t.tm_min), uint32_t(t.tm_hour), uint32_t(t.tm_mday), uint32_t(t.tm_mon), uint32_t(t.tm_year), uint32_t(t.tm_wday), uint32_t(t.tm_yday), uint32_t(t.tm_isdst)};
        wr(tmAddr_, v, sizeof v); ret(tmAddr_);
    });
    H("strftime", [=] {   // uses the host strftime on the guest's struct tm
        uint32_t buf = reg(0), max = reg(1), tmAddr = reg(3);
        std::string f = cstr(reg(2));
        uint32_t v[9]; rd(tmAddr, v, sizeof v);
        struct tm t; std::memset(&t, 0, sizeof t);
        t.tm_sec = int(v[0]); t.tm_min = int(v[1]); t.tm_hour = int(v[2]); t.tm_mday = int(v[3]); t.tm_mon = int(v[4]);
        t.tm_year = int(v[5]); t.tm_wday = int(v[6]); t.tm_yday = int(v[7]); t.tm_isdst = int(v[8]);
        std::vector<char> out(max ? max : 1);
        size_t n = max ? std::strftime(out.data(), max, f.c_str(), &t) : 0;
        if (max) wr(buf, out.data(), n + 1);
        ret(uint32_t(n));
    });
    H("wcsftime", [=] { ret(0); });
    H("lrand48", [=] { static uint64_t x = 88172645463325252ull; x ^= x << 13; x ^= x >> 7; x ^= x << 17; ret(uint32_t(x >> 33)); });
    H("srand48", [=] { ret(0); });

    // math (soft-float ABI: doubles travel in core register pairs)
    H("sin", [=] { retDbl(std::sin(dbl(0, 1))); });
    H("cos", [=] { retDbl(std::cos(dbl(0, 1))); });
    H("sqrt", [=] { retDbl(std::sqrt(std::max(0.0, dbl(0, 1)))); });
    H("round", [=] { retDbl(std::floor(dbl(0, 1) + 0.5)); });
    H("atan2", [=] { retDbl(std::atan2(dbl(0, 1), dbl(2, 3))); });
    H("pow", [=] { retDbl(std::pow(dbl(0, 1), dbl(2, 3))); });

    // pthread: single host thread, cooperative guest threads
    for (const char* n : {"pthread_mutex_lock", "pthread_mutex_unlock", "pthread_cond_wait", "pthread_cond_broadcast", "pthread_key_delete"}) H(n, [=] { ret(0); });
    H("pthread_key_create", [=] { uint32_t k = nextKey_++; w32(reg(0), k); ret(0); });
    H("pthread_getspecific", [=] { auto it = tls_.find(reg(0)); ret(it == tls_.end() ? 0 : it->second); });
    H("pthread_setspecific", [=] { tls_[reg(0)] = reg(1); ret(0); });
    H("pthread_once", [=] {
        uint32_t ctl = reg(0), fn = reg(1);
        if (r32(ctl) == 0) { w32(ctl, 1); tailCall(fn); }
        else ret(0);
    });

    // wide chars / locale helpers (only used by C++ iostream initialisation)
    H("btowc", [=] { ret(reg(0)); });
    H("wctob", [=] { ret(reg(0) & 0xFF); });
    H("mbrtowc", [=] {
        uint32_t d = reg(0), s = reg(1); if (!s) { ret(0); return; }
        uint8_t c = 0; rd(s, &c, 1); if (d) w32(d, c); ret(c ? 1 : 0);
    });
    H("wcrtomb", [=] { if (reg(0)) { uint8_t c = uint8_t(reg(1)); wr(reg(0), &c, 1); } ret(1); });
    H("wcslen", [=] { uint32_t p = reg(0), n = 0; while (r32(p + 4 * n)) n++; ret(n); });
    H("towlower", [=] { uint32_t c = reg(0); ret(c < 128 ? uint32_t(std::tolower(int(c))) : c); });
    H("towupper", [=] { uint32_t c = reg(0); ret(c < 128 ? uint32_t(std::toupper(int(c))) : c); });
    H("iswctype", [=] { ret(0); });
    H("wctype", [=] { ret(1); });
    auto wmemcpyH = [=] {
        uint32_t n = reg(2) * 4; if (n) { std::vector<uint8_t> t(n); rd(reg(1), t.data(), n); wr(reg(0), t.data(), n); }
        ret(reg(0));
    };
    H("wmemcpy", wmemcpyH); H("wmemmove", wmemcpyH);
    H("wmemset", [=] { uint32_t d = reg(0), v = reg(1), n = reg(2); for (uint32_t i = 0; i < n; i++) w32(d + 4 * i, v); ret(d); });
    for (const char* n : {"wmemcmp", "wmemchr", "wcscoll", "wcsxfrm"}) H(n, [=] { ret(0); });
    H("__android_log_print", [=] { ArgReader a(*this, 3); out(format(cstr(reg(2)), a)); ret(0); });
    H("__android_log_write", [=] { out(cstr(reg(2))); ret(0); });

    H("qsort", [=] {
        uint32_t base = reg(0), n = reg(1), sz = reg(2), cmpFn = reg(3);
        if (n < 2 || !sz) { ret(0); return; }
        std::vector<std::vector<uint8_t>> items(n, std::vector<uint8_t>(sz));
        for (uint32_t i = 0; i < n; i++) rd(base + i * sz, items[i].data(), sz);
        uint32_t A = gmalloc(sz), B = gmalloc(sz);
        std::stable_sort(items.begin(), items.end(), [&](const std::vector<uint8_t>& x, const std::vector<uint8_t>& y) {
            if (failed_) return false;
            wr(A, x.data(), sz); wr(B, y.data(), sz);
            return s32(guestCall(cmpFn, A, B)) < 0;
        });
        gfree(A); gfree(B);
        for (uint32_t i = 0; i < n; i++) wr(base + i * sz, items[i].data(), sz);
        ret(0);
    });
}

}  // namespace tg
