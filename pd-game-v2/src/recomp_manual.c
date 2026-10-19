/**
 * Manual function overrides and ICALL diagnostics
 *
 * This file provides:
 *   - recomp_lookup_manual()  : intercept specific Xbox VAs with hand-written code
 *   - recomp_icall_fail_log() : log when an indirect call target can't be resolved
 *   - ICALL trace ring buffer  : globals used by the RECOMP_ICALL macro
 *
 * The recomp pipeline generates an auto-dispatch table (recomp_lookup) that
 * resolves most function addresses. recomp_lookup_manual() is called FIRST,
 * giving you a chance to override any function with a custom implementation.
 *
 * Common reasons to add manual overrides:
 *   - Trace a function to understand call flow (wrap the generated version)
 *   - Fix a function the lifter translated incorrectly
 *   - Stub out a function that crashes (return early, set eax to a safe value)
 *   - Redirect a function to a native implementation (e.g., skip CRT init)
 *   - Intercept D3D/audio calls for custom rendering or sound
 */

#include <stdio.h>
#include <stdint.h>
#define RECOMP_GENERATED_CODE
#include "gen/recomp_funcs.h"

/* ── ICALL trace ring buffer ───────────────────────────────── */

/*
 * These globals are written by the RECOMP_ICALL macro (defined in
 * recomp_types.h) every time an indirect call is dispatched. When a
 * crash occurs, the VEH handler or recomp_icall_fail_log() can dump
 * the last 16 call targets to help you trace what happened.
 *
 * The runtime owns them: xbox_kernel defines all three in
 * src/kernel/xbox_memory_layout.c, and recomp_types.h declares them extern.
 * Declare, do not define -- a definition here as well is a duplicate symbol,
 * and a project copied from this template failed to link on all three:
 *
 *   xbox_memory_layout.obj : error LNK2005: g_icall_count already defined
 *                            in recomp_manual.obj
 */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

typedef void (*recomp_func_t)(void);

/* ── Register state (defined in xbox_memory_layout.c) ──────── */

extern uint32_t g_eax;
extern ptrdiff_t g_xbox_mem_offset;

/* ── Manual function overrides ─────────────────────────────── */

/*
 * Return a function pointer to override the given Xbox VA, or NULL
 * to fall through to the auto-generated dispatch table.
 *
 * This is called on every indirect call (RECOMP_ICALL) and every
 * direct call through the dispatch table, so keep it fast. A chain
 * of if-statements on uint32_t compiles to a simple comparison
 * sequence; for large override tables, consider a sorted array
 * with binary search.
 *
 * Examples of common override patterns:
 *
 *   // Trace wrapper: log entry/exit around the generated function
 *   extern void sub_00012345(void);
 *   static void traced_sub_00012345(void) {
 *       fprintf(stderr, "[TRACE] sub_00012345 entered, eax=0x%08X\n", g_eax);
 *       sub_00012345();
 *       fprintf(stderr, "[TRACE] sub_00012345 returned, eax=0x%08X\n", g_eax);
 *   }
 *
 *   // Stub: skip a function entirely (return 0 in eax)
 *   static void stub_00067890(void) {
 *       g_eax = 0;
 *   }
 *
 *   // Fix: replace a broken lifted function with correct C
 *   static void fixed_sub_000ABCDE(void) {
 *       // Read arguments from stack/registers per calling convention
 *       uint32_t arg1 = g_ecx;
 *       uint32_t arg2 = MEM32(g_esp + 4);
 *       // ... correct implementation ...
 *       g_eax = result;
 *   }
 */
/* RECOMP_FPS60: the vblank callback (D3D SetVerticalBlankCallback target) sets the frame-limiter event only when the old
 * counter is odd, i.e. every second vblank. Set it on every vblank as well (see g_fps60 in main.c). */
extern int g_fps60;
static void fps60_vblank_callback(void)
{
    uint32_t old = MEM32(0x39E700);
    if (!(old & 1u)) {
        PUSH32(esp, MEM32(0x39E710));
        PUSH32(esp, 0x000B0925u);
        sub_00144F2B();
    }
    sub_000B08E0();
}

recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    if (g_fps60 && xbox_va == 0x000B08E0u)
        return fps60_vblank_callback;
    /*
     * TODO: Add your overrides here. Examples:
     *
     * if (xbox_va == 0x00012345) return traced_sub_00012345;
     * if (xbox_va == 0x00067890) return stub_00067890;
     * if (xbox_va == 0x000ABCDE) return fixed_sub_000ABCDE;
     */

    (void)xbox_va;
    return (recomp_func_t)0;
}

/* ── ICALL failure logging ─────────────────────────────────── */

/*
 * Called when RECOMP_ICALL cannot resolve a target address.
 * This usually means one of:
 *   - A vtable dispatch to an address not in the dispatch table
 *   - A function pointer loaded from uninitialized or corrupt memory
 *   - A kernel thunk address that the bridge doesn't handle
 *
 * During early bring-up you will see many of these. Most are harmless
 * (the ICALL macro pops the dummy return address and continues).
 * Focus on the ones that cause crashes or incorrect behavior.
 */
void recomp_icall_fail_log(uint32_t va)
{
    fprintf(stderr, "[ICALL] Failed to resolve VA 0x%08X (total calls: %llu)\n",
            va, (unsigned long long)g_icall_count);

    /* Dump last 16 call targets from the ring buffer */
    fprintf(stderr, "  Recent ICALL targets:\n");
    for (int i = 0; i < 16; i++) {
        int idx = (g_icall_trace_idx - 16 + i) & 15;
        if (g_icall_trace[idx])
            fprintf(stderr, "    [%2d] 0x%08X\n", i, g_icall_trace[idx]);
    }
    fflush(stderr);
}

/* An indirect call whose target is not code: a null or wild function pointer.
 *
 * Skipping these is right -- calling a data address is worse -- but skipping
 * them *silently* is not. They almost always arrive inside a loop, so the
 * symptom is a hang with no output rather than a diagnosable null vtable call.
 *
 * Rate-limited per address: a spin can produce millions of these, and the
 * useful information is which addresses occur, not how often.
 */
void recomp_icall_not_code_log(uint32_t va)
{
    enum { SLOTS = 16 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
    }
    hits[i]++;
    /* Report at 1, 10, 100, 1000 ... rather than once. A single line says a
     * wild pointer was skipped; the progression says it is being skipped in a
     * loop, which is the difference between a curiosity and the reason the
     * title is hung. */
    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }
    fprintf(stderr, "[ICALL] target 0x%08X is not code -- skipped %llu time(s) "
                    "(null or wild function pointer, at call #%llu)\n",
            va, (unsigned long long)hits[i],
            (unsigned long long)g_icall_count);
    fflush(stderr);
}

/* ── Untranslated instructions ───────────────────────────────────────────
 *
 * The lifter emits RECOMP_UNIMPL(text, va) at every instruction it has no
 * translation for, in place of the bare comment it used to leave. The
 * instruction is still a no-op; this only stops the omission being silent.
 * RECOMP_UNIMPL_TRAP=1 aborts at the first hit, at the guest address of the
 * cause rather than wherever the damage surfaces. */
#include <stdlib.h>

void recomp_unimpl(const char *text, uint32_t va)
{
    static int printed;
    const char *trap = getenv("RECOMP_UNIMPL_TRAP");
    int stop = trap && *trap && *trap != '0';

    if (printed < 50 || stop) {
        printed++;
        fprintf(stderr,
                "[UNIMPL] untranslated instruction REACHED: `%s` at 0x%08X"
                " (a no-op; set RECOMP_UNIMPL_TRAP=1 to stop here)\n",
                text, va);
        fflush(stderr);
    }
    if (stop) abort();
}

/* -- Display gamma ramp (hand-written; see manual_functions.json) ------------
 *
 * sub_0024D446 is D3D's DAC upload: register args eax = a pointer whose first
 * dword is the NV2A register base, edx = the ramp (256 R, 256 G, 256 B bytes).
 * The original writes the ramp through the palette DAC ports, which are plain
 * memory in this runtime and drive nothing. This version keeps those writes
 * (so anything reading them back is unchanged) and also hands the ramp to the
 * renderer, which applies it in its present pass. That is what the game's
 * Options > Screen Brightness slider changes. */
extern void xbox_SetGammaRamp(const uint8_t *rgb768);

void sub_0024D446(void)
{
    uint32_t nv = MEM32(eax), ramp = edx;
    uint8_t buf[768];
    int i;

    for (i = 0; i < 256; i++) {
        buf[i]       = MEM8(ramp + i);
        buf[256 + i] = MEM8(ramp + 0x100 + i);
        buf[512 + i] = MEM8(ramp + 0x200 + i);
    }
    xbox_SetGammaRamp(buf);
    MEM8(nv + 0x6813C8) = 0;
    MEM8(nv + 0x6813C9) = buf[512 + 255];
    ecx = nv;
    eax = ramp + 0x100 + 256;
    esp += 4;                              /* ret */
}


/* -- pd-1ml: bounded wait for a stolen sound stream to stop -------------------------------------
 * sub_001BB745 is IDirectSoundStream::GetStatus (a this-adjusting thunk into sub_001BAB52). The game's sound-slot
 * allocator sub_00167BC0 steals a busy slot, calls FlushEx and then spins `while (GetStatus(stream, &st), st & 3)`.
 * In the quest001 tutorial that stream's state word ([[this-0x1C+0x20]+0x12]) stayed 0xFFFF, status never became 0
 * and the game froze (~25 M RtlEnter/LeaveCriticalSection calls per second, nothing else running). Poking the word
 * to 0 ended the freeze at once. Why the word was not updated is not established (bead pd-1ml); this override keeps
 * the original behaviour (same call, same stack handling) and only intervenes when one thread has polled the same
 * stream, with every call within 2 ms of the previous one, more than 300000 times and it still reports busy: that
 * is ~1 s of pure spinning, far beyond any real stream drain; normal per-frame polling is spaced ~3 ms apart.
 * Then it clears the stream's state word and returns "stopped" (status 0), exactly what the experiment did. */
#include <windows.h>
void sub_001BB745(void)
{
    uint32_t self = MEM32(esp + 4), st = MEM32(esp + 8);
    uint32_t arg1 = self ? self - 0x1C : 0;
    static uint32_t s_obj, s_n; static DWORD s_tid; static LONGLONG s_last, s_freq; static uint32_t s_forced;
    LARGE_INTEGER now;

    PUSH32(esp, st);
    PUSH32(esp, arg1);
    PUSH32(esp, 0x001BB75Eu);                 /* the thunk's own return address inside this function */
    sub_001BAB52();
    esp += 12;                                /* original epilogue: ret 8 */

    if (!s_freq) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); s_freq = f.QuadPart; }
    QueryPerformanceCounter(&now);
    if (eax == 0 && (MEM8(st) & 3)) {
        if (s_obj == self && s_tid == GetCurrentThreadId() && (now.QuadPart - s_last) * 1000 < 2 * s_freq) {
            if (++s_n > 300000u) {
                uint32_t impl = MEM32(arg1 + 0x20);
                if (s_forced++ < 20)
                    fprintf(stderr, "[AUDIO] pd-1ml: stream %08X still busy after %u back-to-back GetStatus polls (state word %04X): forcing stopped\n",
                            self, s_n, (unsigned)MEM16(impl + 0x12));
                MEM16(impl + 0x12) = 0;
                MEM32(st) = 0;
                s_n = 0; s_obj = 0;
            }
        } else {
            s_obj = self; s_tid = GetCurrentThreadId(); s_n = 1;
        }
        s_last = now.QuadPart;
    } else {
        s_obj = 0; s_n = 0;
    }
}

/* Debug: start story stage <stage> from the game thread, the way the scenario menu does
 * (sub_000B8570 -> sub_000DEA90 with EAX = stage). Called from the function-entry hook at the
 * per-frame state machine (sub_000DEB50), so the guest is between instructions of a known
 * routine. The call is made like a guest call: a fake return address goes on the guest stack
 * and every register is restored afterwards. */
void pd_inject_stage(int stage)
{
    uint32_t se = g_esp, sa = g_eax, sc = g_ecx, sd = g_edx, sb = g_ebx, ss = g_esi, sdi = g_edi, sbp = g_ebp;
    MEM32(0x002CA4B8) = (uint32_t)stage;  /* what sub_000B8480 stores before starting a stage */
    MEM32(0x0037BE74) = (uint32_t)stage;
    MEM32(0x0039E8A4) = 0;  /* the old scene's render list: the new map is not loaded yet, sub_000BE0E0 skips a null list */
    PUSH32(g_esp, 0);
    g_eax = (uint32_t)stage;
    sub_000DEA90();
    g_esp = se; g_eax = sa; g_ecx = sc; g_edx = sd; g_ebx = sb; g_esi = ss; g_edi = sdi; g_ebp = sbp;
}

/* Variant: go through the mode object the way a stage retry does: sub_000B8380(obj, flag=0) with
 * [0x2CA4B8] = stage, where obj = [0x37BE54]. If RECOMP_STAGE_END is set the old stage is ended first
 * with sub_000B86C0(obj) (state 5). */
extern void sub_000B8380(void);
extern void sub_000B86C0(void);
void pd_inject_stage_b(int stage)
{
    uint32_t se = g_esp, sa = g_eax, sc = g_ecx, sd = g_edx, sb = g_ebx, ss = g_esi, sdi = g_edi, sbp = g_ebp;
    uint32_t obj = MEM32(0x0037BE54);
    MEM32(0x002CA4B8) = (uint32_t)stage;
    MEM32(0x0037BE74) = (uint32_t)stage;
    if (getenv("RECOMP_STAGE_END")) {
        PUSH32(g_esp, obj); PUSH32(g_esp, 0);
        sub_000B86C0();
    }
    g_esp = se;
    PUSH32(g_esp, 0); PUSH32(g_esp, 0); PUSH32(g_esp, 0);   /* arg2, flag, return address */
    g_ecx = obj;
    sub_000B8380();
    g_esp = se; g_eax = sa; g_ecx = sc; g_edx = sd; g_ebx = sb; g_esi = ss; g_edi = sdi; g_ebp = sbp;
}

/* Debug (pd-ab1/pd-2ns): start quest <id> (a battle) from the hub the way the script natives do.
 * Phase 0->1: replicate sub_0001F7D0's tail (copy the 0x520-byte quest record, set [0x2D99B8]) and call the
 * "end stage" native sub_000EB850 (sets [0x37BE74]=0x7777 and ends the stage via sub_000B86C0).
 * Phase 1: each frame call sub_000EB850 until it returns 0 (mode state == 5), then the "start quest" native
 * sub_000EB930 (-> sub_00045F80 with record 0x399B58). Driven from the sub_000DE980 hook (FN_WATCH=5). */
extern void sub_000EB850(void);
extern void sub_000EB930(void);
extern void sub_000EB890(void);
volatile int g_pd_quest_phase = 0;
static int pd_quest_calls, pd_quest_seen5, pd_quest_lastst = -1;   /* file scope so a second quest can reset them */
void pd_quest_step(int req)
{
    uint32_t se = g_esp, sa = g_eax, sc = g_ecx, sd = g_edx, sb = g_ebx, ss = g_esi, sdi = g_edi, sbp = g_ebp;
    if (req >= 0 && g_pd_quest_phase == 2) {   /* pd-nre: a new quest request after one has run (win -> hub -> next quest) */
        g_pd_quest_phase = 0; pd_quest_calls = 0; pd_quest_seen5 = 0; pd_quest_lastst = -1;
    }
    if (req >= 0 && g_pd_quest_phase == 0) {
        uint32_t src = MEM32(0x0039E754) + (uint32_t)req * 0x520u, i, p;
        fprintf(stderr, "[QUEST] id %d record@%08X stage_field=%d\n", req, src, (int)(int16_t)(MEM32(src + 0x504) & 0xFFFF));
        for (i = 0; i < 0x148; i++) MEM32(0x00399B58 + i * 4) = MEM32(src + i * 4);
        MEM32(0x002D99B8) = (MEM32(0x002D99B8) & 0xFFFF0000u) | (uint32_t)req;
        MEM32(0x00399B48) = 0x00399340u + (uint32_t)req * 8;
        p = MEM32(0x0032FC9C);
        MEM32(p + 0x54) = (uint32_t)req;
        g_pd_quest_phase = 1;
    }
    if (g_pd_quest_phase == 1) {
        uint32_t obj = MEM32(0x0037BE54);
        unsigned st = MEM32(obj + 0x28) & 0xFF;
        if (pd_quest_calls == 0) {            /* end the hub stage once (state -> 5, counts to 0 by itself) */
            PUSH32(g_esp, 0); PUSH32(g_esp, 0);
            sub_000EB850();
            pd_quest_calls = 1;
            fprintf(stderr, "[QUEST] end-stage native called, state=%u%c", (unsigned)(MEM32(obj + 0x28) & 0xFF), 10);
        } else {
            if ((int)st != pd_quest_lastst) { fprintf(stderr, "[QUEST] mode state -> %u%c", st, 10); pd_quest_lastst = (int)st; }
            if (st == 5) pd_quest_seen5 = 1;
            if (pd_quest_seen5 && st == 0) {
                fprintf(stderr, "[QUEST] stage torn down; starting quest%c", 10);
                g_esp = se; PUSH32(g_esp, 0); PUSH32(g_esp, 0);
                sub_000EB930();
                g_pd_quest_phase = 2;
            }
        }
    }
    if (g_pd_quest_phase == 2) {
        static int n; uint32_t obj = MEM32(0x0037BE54);
        if (n++ < 400 && (n < 20 || n % 10 == 0))
            fprintf(stderr, "[QSTATE %d] 37BE74=%X 2CA4B8=%X 33094C=%X 33028C=%X modestate=%u/%u 39E890=%X 39EA54=%X 39E8A0=%X 39E854=%X 39E100=%X%c", n,
                MEM32(0x37BE74), MEM32(0x2CA4B8), MEM32(0x33094C), MEM32(0x33028C), MEM32(obj + 0x28) & 0xFF, (MEM32(obj + 0x28) >> 8) & 0xFF,
                MEM32(0x39E890), MEM32(0x39EA54), MEM32(0x39E8A0), MEM32(0x39E854), MEM32(0x39E100), 10);
    }
done:
    g_esp = se; g_eax = sa; g_ecx = sc; g_edx = sd; g_ebx = sb; g_esi = ss; g_edi = sdi; g_ebp = sbp;
}
