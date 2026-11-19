// games/dissidia/runtime/xmod_interp.cpp — a small Allegrex interpreter for code an xdelta mod loads or rewrites at run time.
//
// Some ISO patches carry extra machine code in their data and load it into the heap while the game runs (Emendatus
// does: two "plugins" read from the data it appends to PACKAGE.BIN), and that code may rewrite instructions of its own
// after it is loaded (trampolines that hold the plugin's own address). A static recompile has never seen either. Until
// tools/xdelta_code.py has compiled such code (xdelta_mods.cpp learns where it lives on the first run), and wherever
// the compiled copy no longer matches memory, it is run here instead.
//
// Scope: the integer instruction set (with the Allegrex extras: ext/ins, seb/seh/wsbh/bitrev, min/max, clz/clo,
// madd/msub, rotations), branches and jumps with their delay slots, the FPU's loads/stores/moves, arithmetic,
// conversions and compares, and the VFPU through PPSSPP's interpreter (psp_vfpu_exec, the same code the recompiled
// VFPU ops run). Not supported (the call stops with one log line): syscalls, traps. Calls go through
// RECOMP_LOOKUP like recompiled code (so a call into the game runs the game's compiled function, a call to another
// address of the loaded code comes back here). `jr $ra` returns from the call that entered here, as in the recompiled
// code, which does not keep return addresses; a jump or branch out of [lo, hi) is a tail call.

#include "recomp.h"
#include "psp_vfpu.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <mutex>

namespace {

uint32_t rd(uint8_t* m, uint32_t a) { return psp_mem_read<uint32_t>(m, a); }

void say_once(uint32_t pc, uint32_t op, const char* what) {
    static std::mutex mu;
    static std::set<uint32_t> said;
    std::lock_guard<std::mutex> lk(mu);
    if (said.insert(pc).second && said.size() < 64)
        std::fprintf(stderr, "[XMOD] interpreter: %s at 0x%08X (word 0x%08X); the call is cut short\n", what, pc, op);
}

int32_t f2i(float f, int mode) {   // mode 0 nearest-even, 1 truncate, 2 ceil, 3 floor; saturates like the PSP
    double d = f;
    switch (mode) {
        case 0: d = std::nearbyint(d); break;
        case 1: d = std::trunc(d); break;
        case 2: d = std::ceil(d); break;
        default: d = std::floor(d); break;
    }
    return psp_sat_f2i(d);
}

enum Flow { NEXT, STOP };

// executes one non-control instruction; false = not supported
bool exec(uint32_t op, uint32_t pc, uint8_t* m, recomp_context* c) {
    const uint32_t rs = (op >> 21) & 31, rt = (op >> 16) & 31, rd_ = (op >> 11) & 31, sa = (op >> 6) & 31, fn = op & 63;
    const uint32_t S = (uint32_t)c->r[rs], T = (uint32_t)c->r[rt];
    const int32_t imm = (int16_t)(op & 0xFFFF);
    const uint32_t uimm = op & 0xFFFF;
    auto W = [&](uint32_t r, uint32_t v) { if (r) c->r[r] = (int32_t)v; };
    const uint32_t ea = S + (uint32_t)imm;
    switch (op >> 26) {
    case 0x00:
        switch (fn) {
            case 0x00: W(rd_, T << sa); return true;                                            // sll (nop)
            case 0x02: W(rd_, rs == 1 ? (sa ? (T >> sa) | (T << (32 - sa)) : T) : T >> sa); return true;   // srl / rotr
            case 0x03: W(rd_, (uint32_t)((int32_t)T >> sa)); return true;
            case 0x04: W(rd_, T << (S & 31)); return true;
            case 0x06: { uint32_t s = S & 31; W(rd_, sa == 1 ? (s ? (T >> s) | (T << (32 - s)) : T) : T >> s); return true; }
            case 0x07: W(rd_, (uint32_t)((int32_t)T >> (S & 31))); return true;
            case 0x0A: if (T == 0) W(rd_, S); return true;                                       // movz
            case 0x0B: if (T != 0) W(rd_, S); return true;                                       // movn
            case 0x0F: return true;                                                              // sync
            case 0x10: W(rd_, c->hi); return true;
            case 0x11: c->hi = S; return true;
            case 0x12: W(rd_, c->lo); return true;
            case 0x13: c->lo = S; return true;
            case 0x16: W(rd_, S ? (uint32_t)__builtin_clz(S) : 32); return true;                 // clz
            case 0x17: W(rd_, ~S ? (uint32_t)__builtin_clz(~S) : 32); return true;               // clo
            case 0x18: { int64_t p = (int64_t)(int32_t)S * (int32_t)T; c->lo = (uint32_t)p; c->hi = (uint32_t)(p >> 32); return true; }
            case 0x19: { uint64_t p = (uint64_t)S * T; c->lo = (uint32_t)p; c->hi = (uint32_t)(p >> 32); return true; }
            case 0x1A:
                if ((int32_t)T == 0) { c->lo = (int32_t)S < 0 ? 1 : 0xFFFFFFFFu; c->hi = S; }
                else if ((int32_t)S == INT32_MIN && (int32_t)T == -1) { c->lo = S; c->hi = 0; }
                else { c->lo = (uint32_t)((int32_t)S / (int32_t)T); c->hi = (uint32_t)((int32_t)S % (int32_t)T); }
                return true;
            case 0x1B:
                if (T == 0) { c->lo = 0xFFFFFFFFu; c->hi = S; } else { c->lo = S / T; c->hi = S % T; }
                return true;
            case 0x1C: case 0x1D: case 0x2E: case 0x2F: {                                       // madd maddu msub msubu
                int64_t acc = (int64_t)(((uint64_t)c->hi << 32) | c->lo);
                int64_t p = (fn == 0x1C || fn == 0x2E) ? (int64_t)(int32_t)S * (int32_t)T : (int64_t)((uint64_t)S * T);
                acc = (fn == 0x1C || fn == 0x1D) ? acc + p : acc - p;
                c->lo = (uint32_t)acc; c->hi = (uint32_t)((uint64_t)acc >> 32);
                return true;
            }
            case 0x20: case 0x21: W(rd_, S + T); return true;
            case 0x22: case 0x23: W(rd_, S - T); return true;
            case 0x24: W(rd_, S & T); return true;
            case 0x25: W(rd_, S | T); return true;
            case 0x26: W(rd_, S ^ T); return true;
            case 0x27: W(rd_, ~(S | T)); return true;
            case 0x2A: W(rd_, (int32_t)S < (int32_t)T); return true;
            case 0x2B: W(rd_, S < T); return true;
            case 0x2C: W(rd_, (int32_t)S > (int32_t)T ? S : T); return true;                     // max
            case 0x2D: W(rd_, (int32_t)S < (int32_t)T ? S : T); return true;                     // min
            default: return false;
        }
    case 0x08: case 0x09: W(rt, S + (uint32_t)imm); return true;
    case 0x0A: W(rt, (int32_t)S < imm); return true;
    case 0x0B: W(rt, S < (uint32_t)imm); return true;
    case 0x0C: W(rt, S & uimm); return true;
    case 0x0D: W(rt, S | uimm); return true;
    case 0x0E: W(rt, S ^ uimm); return true;
    case 0x0F: W(rt, uimm << 16); return true;
    case 0x1F:                                                                                  // SPECIAL3
        if (fn == 0x00) { uint32_t size = rd_ + 1; W(rt, (S >> sa) & (size >= 32 ? 0xFFFFFFFFu : ((1u << size) - 1))); return true; }   // ext
        if (fn == 0x04) {                                                                          // ins
            uint32_t size = rd_ + 1 - sa;
            uint32_t mask = (size >= 32 ? 0xFFFFFFFFu : ((1u << size) - 1)) << sa;
            W(rt, (T & ~mask) | ((S << sa) & mask));
            return true;
        }
        if (fn == 0x20) {
            switch (sa) {
                case 0x10: W(rd_, (uint32_t)(int32_t)(int8_t)T); return true;                    // seb
                case 0x18: W(rd_, (uint32_t)(int32_t)(int16_t)T); return true;                   // seh
                case 0x02: W(rd_, ((T & 0xFF00FF00u) >> 8) | ((T & 0x00FF00FFu) << 8)); return true;   // wsbh
                case 0x03: W(rd_, __builtin_bswap32(T)); return true;                             // wsbw
                case 0x14: { uint32_t v = 0; for (int i = 0; i < 32; i++) if (T & (1u << i)) v |= 1u << (31 - i); W(rd_, v); return true; }
                default: return false;
            }
        }
        return false;
    case 0x20: W(rt, (uint32_t)(int32_t)psp_mem_read<int8_t>(m, ea)); return true;
    case 0x21: W(rt, (uint32_t)(int32_t)psp_mem_read<int16_t>(m, ea)); return true;
    case 0x23: W(rt, rd(m, ea)); return true;
    case 0x24: W(rt, psp_mem_read<uint8_t>(m, ea)); return true;
    case 0x25: W(rt, psp_mem_read<uint16_t>(m, ea)); return true;
    case 0x22: { uint32_t sh = (ea & 3) * 8, w = rd(m, ea & ~3u); W(rt, (T & (0x00FFFFFFu >> sh)) | (w << (24 - sh))); return true; }   // lwl
    case 0x26: { uint32_t sh = (ea & 3) * 8, w = rd(m, ea & ~3u); W(rt, (T & ~(0xFFFFFFFFu >> sh)) | (w >> sh)); return true; }       // lwr
    case 0x28: psp_mem_write<uint8_t>(m, ea, (uint8_t)T); return true;
    case 0x29: psp_mem_write<uint16_t>(m, ea, (uint16_t)T); return true;
    case 0x2B: psp_mem_write<uint32_t>(m, ea, T); return true;
    case 0x2A: { uint32_t sh = (ea & 3) * 8, a = ea & ~3u, w = rd(m, a);                                   // swl
                 psp_mem_write<uint32_t>(m, a, (w & ~(0xFFFFFFFFu >> (24 - sh))) | (T >> (24 - sh))); return true; }
    case 0x2E: { uint32_t sh = (ea & 3) * 8, a = ea & ~3u, w = rd(m, a);                                   // swr
                 psp_mem_write<uint32_t>(m, a, (w & ~(0xFFFFFFFFu << sh)) | (T << sh)); return true; }
    case 0x2F: return true;                                                                     // cache
    case 0x31: c->fi[rt] = rd(m, ea); return true;                                              // lwc1
    case 0x39: psp_mem_write<uint32_t>(m, ea, c->fi[rt]); return true;                          // swc1
    case 0x11: {                                                                                // COP1
        const uint32_t ft = rt, fs = rd_, fd = sa;
        if (rs == 0x00) { W(rt, c->fi[fs]); return true; }                                      // mfc1
        if (rs == 0x04) { c->fi[fs] = T; return true; }                                         // mtc1
        if (rs == 0x02) { W(rt, fs == 31 ? ((c->fcr31 & ~(1u << 23)) | (c->fpu_cc ? 1u << 23 : 0)) : 0); return true; }   // cfc1
        if (rs == 0x06) { if (fs == 31) { c->fcr31 = T; c->fpu_cc = (T >> 23) & 1; } return true; }                      // ctc1
        if (rs == 0x14 && fn == 0x20) { c->f[fd] = (float)(int32_t)c->fi[fs]; return true; }   // cvt.s.w
        if (rs != 0x10) return false;
        const float a = c->f[fs], b = c->f[ft];
        switch (fn) {
            case 0x00: c->f[fd] = a + b; return true;
            case 0x01: c->f[fd] = a - b; return true;
            case 0x02: c->f[fd] = a * b; return true;
            case 0x03: c->f[fd] = a / b; return true;
            case 0x04: c->f[fd] = std::sqrt(a); return true;
            case 0x05: c->f[fd] = std::fabs(a); return true;
            case 0x06: c->fi[fd] = c->fi[fs]; return true;
            case 0x07: c->f[fd] = -a; return true;
            case 0x0C: c->fi[fd] = (uint32_t)f2i(a, 0); return true;
            case 0x0D: c->fi[fd] = (uint32_t)f2i(a, 1); return true;
            case 0x0E: c->fi[fd] = (uint32_t)f2i(a, 2); return true;
            case 0x0F: c->fi[fd] = (uint32_t)f2i(a, 3); return true;
            case 0x24: c->fi[fd] = (uint32_t)f2i(a, (int)(c->fcr31 & 3) == 0 ? 0 : (int)(c->fcr31 & 3) == 1 ? 1 : (int)(c->fcr31 & 3) == 2 ? 2 : 3); return true;
            default:
                if (fn >= 0x30) {                                                               // c.cond.s
                    const bool un = std::isnan(a) || std::isnan(b);
                    bool r = false;
                    if ((fn & 1) && un) r = true;
                    if ((fn & 2) && !un && a == b) r = true;
                    if ((fn & 4) && !un && a < b) r = true;
                    c->fpu_cc = r;
                    return true;
                }
                return false;
        }
    }
    default:
        return psp_vfpu_exec(c, m, op);                                                          // VFPU (incl. lv/sv, mfv/mtv)
    }
}

bool is_control(uint32_t op) {
    const uint32_t o = op >> 26;
    if (o == 0) return (op & 63) == 0x08 || (op & 63) == 0x09;
    if (o == 1) return true;
    if (o >= 2 && o <= 7) return true;
    if (o >= 0x14 && o <= 0x17) return true;
    if (o == 0x11 && ((op >> 21) & 31) == 0x08) return true;                                     // bc1x
    if (o == 0x12 && ((op >> 21) & 31) == 0x08) return true;                                     // bvf / bvt
    return false;
}

}  // namespace

// Runs guest code at `pc` until it returns (jr $ra) or leaves [lo, hi). False when it met an instruction it cannot run.
bool dissidia_xmod_interpret(uint32_t pc, uint32_t lo, uint32_t hi, uint8_t* m, recomp_context* c) {
    const uint32_t entry = pc, entry_ra = (uint32_t)c->r[31];
    for (uint64_t steps = 0;; steps++) {
        if (steps > 200000000ull) {
            std::fprintf(stderr, "[XMOD] interpreter: entered at 0x%08X range [0x%08X, 0x%08X) ra=%08X\n", entry, lo, hi, (uint32_t)c->r[31]);
            say_once(pc, rd(m, pc), "runaway loop"); return false;
        }
        const uint32_t op = rd(m, pc);
        if (!is_control(op)) {
            if (!exec(op, pc, m, c)) { say_once(pc, op, (op >> 26) == 0 && (op & 63) == 0x0C ? "syscall" : "unsupported instruction"); return false; }
            pc += 4;
            continue;
        }
        const uint32_t o = op >> 26, rs = (op >> 21) & 31, rt = (op >> 16) & 31;
        const uint32_t S = (uint32_t)c->r[rs], T = (uint32_t)c->r[rt];
        const uint32_t btarget = pc + 4 + ((uint32_t)(int32_t)(int16_t)(op & 0xFFFF) << 2);
        bool taken = false, likely = false, call = false, reg = false;
        uint32_t target = btarget, link = 0;
        switch (o) {
            case 0x00:
                reg = true; target = S;
                if ((op & 63) == 0x09) { call = true; link = (op >> 11) & 31; }
                taken = true;
                break;
            case 0x01:
                switch (rt) {
                    case 0x00: taken = (int32_t)S < 0; break;
                    case 0x01: taken = (int32_t)S >= 0; break;
                    case 0x02: taken = (int32_t)S < 0; likely = true; break;
                    case 0x03: taken = (int32_t)S >= 0; likely = true; break;
                    case 0x10: taken = (int32_t)S < 0; call = true; link = 31; break;
                    case 0x11: taken = (int32_t)S >= 0; call = true; link = 31; break;
                    case 0x12: taken = (int32_t)S < 0; call = true; link = 31; likely = true; break;
                    case 0x13: taken = (int32_t)S >= 0; call = true; link = 31; likely = true; break;
                    default: say_once(pc, op, "unsupported branch"); return false;
                }
                break;
            case 0x02: case 0x03:
                target = ((pc + 4) & 0xF0000000u) | ((op & 0x03FFFFFFu) << 2);
                taken = true;
                if (o == 0x03) { call = true; link = 31; }
                break;
            case 0x04: case 0x14: taken = S == T; likely = o == 0x14; break;
            case 0x05: case 0x15: taken = S != T; likely = o == 0x15; break;
            case 0x06: case 0x16: taken = (int32_t)S <= 0; likely = o == 0x16; break;
            case 0x07: case 0x17: taken = (int32_t)S > 0; likely = o == 0x17; break;
            case 0x11: {
                const uint32_t nd = (op >> 16) & 3;   // bc1f bc1t bc1fl bc1tl
                taken = (nd & 1) ? c->fpu_cc : !c->fpu_cc;
                likely = (nd & 2) != 0;
                break;
            }
            case 0x12: {                              // bvf / bvt / bvfl / bvtl on a VFPU condition bit
                const bool bit = (c->vfpu_ctrl[3] >> ((op >> 18) & 7)) & 1;   // VFPU_CTRL_CC
                taken = ((op >> 16) & 1) ? bit : !bit;
                likely = ((op >> 17) & 1) != 0;
                break;
            }
        }
        if (likely && !taken) { pc += 8; continue; }
        if (call && link) c->r[link] = (int32_t)(pc + 8);
        const uint32_t ds = rd(m, pc + 4);
        if (is_control(ds)) { say_once(pc + 4, ds, "branch in a delay slot"); return false; }
        if (!exec(ds, pc + 4, m, c)) { say_once(pc + 4, ds, "unsupported instruction"); return false; }
        if (!taken) { pc += 8; continue; }
        if (call && !reg && target == pc + 8) { pc = target; continue; }   // `bal 1f; nop; 1:` reads the PC: no call
        if (call) {
            c->pc = target;
            RECOMP_LOOKUP(target)(m, c);
            pc += 8;
            continue;
        }
        if (reg && rs == 31) return true;                                                         // jr $ra: return
        if (reg && target == entry_ra) return true;          // jr to the return address kept in another register: return
        if (!reg && target >= lo && target < hi) {
            if (target <= pc && --c->preempt_budget <= 0) sched_preempt(c);
            pc = target;
            continue;
        }
        c->pc = target;
        RECOMP_LOOKUP(target)(m, c);                                                             // tail call
        return true;
    }
}

// One non-control instruction (the delay slot of a jump an xdelta mod wrote into game code, xdelta_mods.cpp).
bool dissidia_xmod_step(uint32_t op, uint32_t pc, uint8_t* m, recomp_context* c) {
    if (is_control(op)) return false;
    return exec(op, pc, m, c);
}
