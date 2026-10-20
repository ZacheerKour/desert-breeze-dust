/**
 * YOUR_GAME_NAME - Recompiled Game Entry Point
 *
 * This is the Windows executable that hosts the recompiled game code.
 * It performs the following initialization sequence:
 *
 * 1. Load the original XBE file from disk
 * 2. Initialize the Xbox memory layout (map data sections to original VAs)
 * 3. Initialize the Xbox kernel replacement layer
 * 4. Initialize the kernel bridge (thunk table in Xbox memory)
 * 5. Set up game file paths for I/O redirection
 * 6. Initialize the stack pointer
 * 7. Install VEH crash handler for diagnostics
 * 8. Call the game's original entry point (recompiled)
 *
 * Customize this file for your game:
 *   - Set YOUR_GAME_ENTRY_POINT to the XBE entry point address
 *   - Set YOUR_GAME_XBE_PATH to where the XBE file lives
 *   - Set YOUR_GAME_DIR to the game data directory
 *   - Add any CRT global pre-initialization your game needs
 *   - Customize the VEH handler for game-specific crash diagnosis
 *
 * XBE Details (fill in from xbe_parser output):
 *   Title:       YOUR_GAME_NAME
 *   Title ID:    0x00000000
 *   Base addr:   0x00010000
 *   Entry point: 0x00000000
 *   Code size:   ~??? KB (.text)
 *   Sections:    ?? (list them)
 *   Kernel imports: ??
 */

#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

/* xboxrecomp runtime headers */
#include <xbox/xboxrecomp.h>

/*
 * If xboxrecomp.h is not an umbrella header in your setup, include
 * the individual headers directly:
 *
 * #include "kernel.h"
 * #include "xbox_memory_layout.h"
 * #include "d3d8_xbox.h"
 * #include "dsound_xbox.h"
 * #include "xinput_xbox.h"
 */

/* ── Global register state (defined in xbox_memory_layout.c) ── */

/* RECOMP_TLS is not optional here. The runtime defines these thread-local, and
 * a plain `extern` referencing a __declspec(thread) variable does not resolve
 * to the calling thread's copy -- it resolves to the image's TLS template. The
 * host side then writes g_esp somewhere the generated code never reads, so the
 * guest starts with every register at zero and faults immediately, having
 * apparently ignored the setup that visibly ran. */
extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;
extern RECOMP_TLS uint32_t g_seh_ebp;
/* x87 and SSE state. Global for the same reason the volatile GPRs are: one
 * guest routine can lift to several C functions, so a value written in one
 * body is read in the next. Defined in xbox_memory_layout.c like the rest of
 * the register file. */
extern RECOMP_TLS double g_fp_stack[8];
extern RECOMP_TLS int g_fp_top;
extern RECOMP_TLS uint16_t g_fp_control_word;
extern RECOMP_TLS int g_fp_cmp;
extern RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3;
extern RECOMP_TLS RecompXmm g_xmm4, g_xmm5, g_xmm6, g_xmm7;
extern ptrdiff_t g_xbox_mem_offset;

/* ── XBE Constants ─────────────────────────────────────────── */

/*
 * TODO: Set these from your xbe_parser output.
 * Run: py -3 -m tools.xbe_parser game/default.xbe
 */
#define YOUR_GAME_ENTRY_POINT   0x00145B19  /* XBE entry point VA */
#define YOUR_GAME_XBE_PATH      "..\\extracted\\default.xbe"
#define YOUR_GAME_DIR            "..\\extracted"

/* ── Forward declarations ──────────────────────────────────── */

static BOOL load_xbe(const char *path, void **out_data, size_t *out_size);

/* scratch-only diagnostic (enabled by RECOMP_SAMPLE=1): sample where each thread of this
 * process is executing, 40 times, then print the module-relative RIPs after sampling. */
#include <tlhelp32.h>
/* RECOMP_FN_WATCH=1: log the order in which watched guest functions are first
 * called (game movie wrapper 0xA5000-0xA9000, XMV library 0x1FF320-0x228000)
 * and print call counts about every 4 s. Needs the RECOMP_ABI_CHECK build, whose
 * call macro carries the hook. */
volatile int g_fn_watch_on;
volatile int g_fn_watch_arm;
volatile int g_pd_stage_req = -1;
volatile DWORD g_fncount_ms = 1000;
volatile int g_pd_quest_req = -1;
volatile int g_pd_hub_req = 0, g_pd_hook_va = 0;   /* console `hub`: end the stage and start the town (pd-sweep) */
extern volatile int g_pd_quest_phase, g_pd_hub_phase;
static struct { uint32_t va; unsigned n; } s_fnw[4096];
static int s_fnw_n;
/* pd-i0w: "wfn <hexaddr>" in the RECOMP_MEM_LIVE console (mode 5 only) reports, when the dword at that guest address
 * changes, the last guest functions entered. The writer is one of them or a caller still running. */
volatile uint32_t g_wfn_va[4];
volatile uint32_t g_wfn_last[4];
volatile int g_wfn_n;
volatile uint32_t g_fl_va[16]; volatile int g_fl_n;
volatile DWORD g_fo_until; volatile uint32_t g_fo_dst, g_fo_src; volatile float g_fo_d[3];
static uint32_t s_wfn_ring[16]; static unsigned s_wfn_rp;
/* RECOMP_FPS60=1: the title's frame loop (sub_000B13D0) locks itself to 2 vblanks per frame in sub_000B0960
 * ("target = last target + 2", then waits on an event the vblank callback sub_000B08E0 sets on every second callback).
 * Its game time is counted in vblanks, so one vblank per frame is a true 60 FPS frame with half the step. Hook: at entry
 * of sub_000B0960 the stored target is lowered by one, so the add of 2 makes it +1 (the vblank callback is overridden in
 * recomp_manual.c so that the event comes every vblank). Needs the function-entry hook, which is on for mode 7. */
uint32_t *g_snapbuf;   /* debug console snap/sdiff/fdiff */
int g_fps60;
/* pd-cab experiment (off unless RECOMP_MOVIE_CLOCKS / RECOMP_MOVIE_FCLOCKS name counters): the movie subtitle clock.
 * None of the counters tried so far (int 39E6FC 37E8A8 2E4518 253AF8, float 2DD4AC) moved the subtitles. */
/* pd-cab / pd-ukj: the movie subtitle clock. While bit 0x40 of [0x39E7C0] is set (movie playback) the frame loop
 * sub_000B13D0 skips its 2-vblank limiter and runs as fast as it can, adding 1 to [0x39E6FC] after each pass
 * (sub_000B1110 runs the pass and reads it). The game times subtitle lines in that counter. On the console the WMV
 * decode made a pass take about 1/30 s; here the loop runs at 60, so subtitles came at twice the narration's
 * speed. Slowing the loop is not an option: at 30 passes a second the movie's audio packets were handed over a
 * pass late at fixed points (30-100 ms gaps, 0.1.6). So the loop keeps its rate and, at the start of each pass,
 * the counter is set to half the vblanks ([0x39E700], 60 Hz) since the movie started: what a 30 Hz loop counts.
 * The hook mode (8) is on only while the bit is set (pd_movie_pace_thread). RECOMP_MOVIE_PACE=0 = off. */
static void pd_movie_pace(void)
{
    /* RECOMP_MOVIE_CLOCKS=<hex,hex,...> picks the counters (default below). */
    static uint32_t addr[4]; static int naddr = -1;
    static uint32_t base[4], last[4]; static int valid[4];
    uint32_t vb = *(volatile uint32_t *)((uint8_t *)g_xbox_mem_offset + 0x39E700u);
    int k;
    if (naddr < 0) {
        const char *e = getenv("RECOMP_MOVIE_CLOCKS");
        char buf[128], *t, *ctx = NULL;
        naddr = 0;
        snprintf(buf, sizeof buf, "%s", e ? e : "");
        for (t = strtok_s(buf, ",", &ctx); t && naddr < 4; t = strtok_s(NULL, ",", &ctx)) addr[naddr++] = (uint32_t)strtoul(t, NULL, 16);
    }
    {   /* float counters: RECOMP_MOVIE_FCLOCKS=<hex,...> */
        static uint32_t faddr[4]; static int nf = -1; static float fbase_v[4], flast[4]; static uint32_t fbase_vb[4]; static int fvalid[4];
        int j;
        if (nf < 0) {
            const char *e = getenv("RECOMP_MOVIE_FCLOCKS");
            char buf[128], *t, *ctx = NULL;
            nf = 0;
            snprintf(buf, sizeof buf, "%s", e ? e : "");
            for (t = strtok_s(buf, ",", &ctx); t && nf < 4; t = strtok_s(NULL, ",", &ctx)) faddr[nf++] = (uint32_t)strtoul(t, NULL, 16);
        }
        for (j = 0; j < nf; j++) {
            volatile float *fc = (volatile float *)((uint8_t *)g_xbox_mem_offset + faddr[j]);
            float c = *fc, want;
            if (!fvalid[j] || c < flast[j] - 0.5f || c < 0.5f) { fbase_v[j] = c; fbase_vb[j] = vb; fvalid[j] = 1; }
            want = fbase_v[j] + (float)(vb - fbase_vb[j]) * 0.5f;
            *fc = want; flast[j] = want;
            { static unsigned nl; if (nl < 4 || (nl % 600) == 0) fprintf(stderr, "[MPACE] f%X: %g -> %g (vblank %u)\n", faddr[j], c, want, vb); nl++; }
        }
    }
    for (k = 0; k < naddr; k++) {
        volatile uint32_t *cnt = (volatile uint32_t *)((uint8_t *)g_xbox_mem_offset + addr[k]);
        uint32_t c = *cnt, want;
        if (!valid[k] || c < last[k] || c == 0) { base[k] = vb - 2u * c; valid[k] = 1; }   /* the game reset the count */
        want = (vb - base[k]) / 2u;
        if (want < c && c - want > 1000u) { base[k] = vb - 2u * c; want = c; }
        *cnt = want;
        last[k] = want;
        { static unsigned nlog; if (nlog < 4 || (nlog % 600) == 0) fprintf(stderr, "[MPACE] %X: count %u -> %u (vblank %u)\n", addr[k], c, want, vb); nlog++; }
    }
}
static DWORD WINAPI pd_movie_pace_thread(LPVOID unused)
{
    (void)unused;
    for (;;) {
        int movie = (*(volatile uint8_t *)((uint8_t *)g_xbox_mem_offset + 0x39E7C0u) & 0x40) != 0;
        if (movie && g_fn_watch_on == 0) { g_fn_watch_on = 8; fprintf(stderr, "[MPACE] movie mode on%c", 10); }
        else if (movie && g_fn_watch_on != 8) { static int warned; if (!warned++) fprintf(stderr, "[MPACE] movie mode, but hook mode is %d%c", g_fn_watch_on, 10); }
        else if (!movie && g_fn_watch_on == 8) g_fn_watch_on = 0;
        Sleep(10);
    }
    return 0;
}
void recomp_fn_watch_hit(uint32_t va)
{
    int i;
    static DWORD last;
    if (g_fn_watch_on == 8) {
        if (va == 0xB1110u) pd_movie_pace();
        return;
    }
    if (g_fps60 && va == 0xB0960u) {
        /* the original computes target = [esi+0x54] + 2. Make it max(last + 1, current count): one vblank per frame, and a
         * frame that ran late is not punished with the extra "resync" vblank wait of the original (it would turn a 17.5 ms
         * frame into 33 ms). */
        uint32_t *last = (uint32_t *)((uint8_t *)g_xbox_mem_offset + g_esi + 0x54);
        uint32_t cnt = *(volatile uint32_t *)((uint8_t *)g_xbox_mem_offset + 0x39E700u);
        if ((int32_t)(cnt - (*last + 1)) > 0) *last = cnt - 2; else *last -= 1;
    }
    if (g_fn_watch_on == 7)
        return;
    if (g_fn_watch_on == 6) {
        /* Mode 6 (console "fncount <ms>"): count calls of every guest function for a window, then print the busiest
         * 70 and return to mode 5. Shows what a stuck scene is polling. */
        static uint32_t cnt[0x262000 >> 2]; static DWORD t0, win;
        extern volatile DWORD g_fncount_ms;
        if (va >= 0x11000u && va < 0x262000u) cnt[va >> 2]++;
        if (!t0) { t0 = GetTickCount(); win = g_fncount_ms; }
        if (GetTickCount() - t0 > win) {
            int k, i; static uint32_t top_v[4000], top_c[4000];
            memset(top_c, 0, sizeof top_c);
            for (i = 0; i < (int)(0x262000 >> 2); i++) if (cnt[i] > top_c[3999]) {
                k = 3999; while (k > 0 && cnt[i] > top_c[k - 1]) { top_c[k] = top_c[k - 1]; top_v[k] = top_v[k - 1]; k--; }
                top_c[k] = cnt[i]; top_v[k] = (uint32_t)i << 2;
            }
            fprintf(stderr, "[FNCOUNT] tick %lu %lu ms, busiest:", (unsigned long)GetTickCount(), (unsigned long)win);
            for (k = 0; k < 4000 && top_c[k]; k++) fprintf(stderr, " %06X=%u", (unsigned)top_v[k], (unsigned)top_c[k]);
            fprintf(stderr, "%c", 10);
            memset(cnt, 0, sizeof cnt); t0 = 0; g_fn_watch_on = 5;
        }
        return;
    }
    if (g_fn_watch_on == 5) {
        /* Mode 5: stage injection. "stage <n>" in the RECOMP_MEM_LIVE file queues a request that is
         * carried out at the next entry of the per-frame state machine sub_000DEB50. */
        extern void pd_inject_stage(int stage);
        extern volatile int g_pd_stage_req;
        if (g_pd_quest_phase == 2) {
            static uint8_t seen[0x400000]; static unsigned nq;
            if (va < 0x400000 && !seen[va]) { seen[va] = 1; if (nq++ < 6000) fprintf(stderr, "[QF] %06X%c", (unsigned)va, 10); }
        }
        if (g_fo_until) {
            /* pd-nre: "follow <dst> <src> <dx> <dy> <dz> <ms>": for <ms> milliseconds every guest function entry rewrites the
             * position of actor <dst> (+0x90/+0xD0) to <src>'s position plus an offset, so an enemy can be held at a known spot */
            if ((int)(GetTickCount() - g_fo_until) < 0) {
                int j; float p[3];
                memcpy(p, (uint8_t *)g_xbox_mem_offset + g_fo_src + 0x90, 12);
                for (j = 0; j < 3; j++) p[j] += g_fo_d[j];
                memcpy((uint8_t *)g_xbox_mem_offset + g_fo_dst + 0x90, p, 12);
                memcpy((uint8_t *)g_xbox_mem_offset + g_fo_dst + 0xD0, p, 12);
            } else g_fo_until = 0;
        }
        if (g_fl_n) {
            /* pd-nre: "fnlog <hex>" console command: log every entry of up to 16 guest functions with the first stack args */
            int k; static unsigned nfl;
            for (k = 0; k < g_fl_n; k++) if (va == g_fl_va[k] && nfl < 6000) {
                uint32_t w[6]; int j; nfl++;
                for (j = 0; j < 6; j++) memcpy(&w[j], (uint8_t *)g_xbox_mem_offset + g_esp + j * 4u, 4);
                fprintf(stderr, "[FL] %06X t=%lu esp=%08X ret=%08X a=%08X %08X %08X %08X %08X%c", (unsigned)va, (unsigned long)GetTickCount(), (unsigned)g_esp, (unsigned)w[0], (unsigned)w[1], (unsigned)w[2], (unsigned)w[3], (unsigned)w[4], (unsigned)w[5], 10);
            }
        }
        if (g_wfn_n) {
            int k;
            s_wfn_ring[s_wfn_rp++ & 15] = va;
            for (k = 0; k < g_wfn_n; k++) {
                uint32_t now_v; memcpy(&now_v, (uint8_t *)g_xbox_mem_offset + g_wfn_va[k], 4);
                if (now_v != g_wfn_last[k]) {
                    unsigned j; static unsigned nrep;
                    if (nrep++ < 400) {
                        fprintf(stderr, "[WFN] %08X %08X -> %08X; entered (newest first):", (unsigned)g_wfn_va[k], (unsigned)g_wfn_last[k], (unsigned)now_v);
                        for (j = 1; j <= 12; j++) fprintf(stderr, " %06X", (unsigned)s_wfn_ring[(s_wfn_rp - j) & 15]);
                        fprintf(stderr, "%c  guest stack code addrs:", 10);
                        for (j = 0; j < 160; j++) {
                            uint32_t w; memcpy(&w, (uint8_t *)g_xbox_mem_offset + g_esp + j * 4u, 4);
                            if (w >= 0x11000u && w < 0x262000u) fprintf(stderr, " [+%u]%06X", j * 4u, (unsigned)w);
                        }
                        fprintf(stderr, "%c", 10);
                    }
                    g_wfn_last[k] = now_v;
                }
            }
        }
        if (va == 0x1A9B80u || va == 0x1A9BE0u) { extern void pd_reg_hook(uint32_t); pd_reg_hook(va); }
        if ((va == 0xDE980u || va == 0xB83B0u) && (g_pd_quest_req >= 0 || g_pd_quest_phase >= 1 || g_pd_hub_req || g_pd_hub_phase)) {
            g_pd_hook_va = (int)va;
            extern void pd_quest_step(int req);
            int q = g_pd_quest_req; g_pd_quest_req = -1;
            pd_quest_step(q);
        }
        if (va == 0xDE980u && g_pd_stage_req >= 0) {
            int n = g_pd_stage_req; g_pd_stage_req = -1;
            fprintf(stderr, "[STAGE] injecting stage %d%c", n, 10);
            if (getenv("RECOMP_STAGE_B")) { extern void pd_inject_stage_b(int); pd_inject_stage_b(n); } else pd_inject_stage(n);
        }
        return;
    }
    if (g_fn_watch_on == 4) {
        /* Mode 4: RECOMP_FN_STATEWATCH=<hexaddr>:<size>[,<hexaddr>:<size>...] reports, when a watched
         * guest value changes, the last functions entered before the change was seen (the writer is
         * one of them or a callee that was inlined/leaf). Armed by fn_arm.flag like mode 2. */
        static struct { uint32_t va; int sz; uint32_t last; int init; } w[6]; static int nw = -1, armed;
        static uint32_t ring[8]; static unsigned rp; static DWORD chk;
        int i;
        if (nw < 0) {
            const char *e = getenv("RECOMP_FN_STATEWATCH"); nw = 0;
            while (e && *e && nw < 6) { char *q; w[nw].va = (uint32_t)strtoul(e, &q, 16); e = q; w[nw].sz = 4; if (*e == ':') { w[nw].sz = (int)strtoul(e + 1, &q, 10); e = q; } nw++; if (*e == ',') e++; else break; }
        }
        if (GetTickCount() - chk > 200) { chk = GetTickCount(); if (!armed && GetFileAttributesA("fn_arm.flag") != INVALID_FILE_ATTRIBUTES) armed = 1; }
        ring[rp++ & 7] = va;
        for (i = 0; i < nw; i++) {
            uint32_t v = 0; memcpy(&v, (uint8_t *)g_xbox_mem_offset + w[i].va, (size_t)w[i].sz);
            if (!w[i].init) { w[i].init = 1; w[i].last = v; continue; }
            if (v != w[i].last) {
                if (armed) {
                    unsigned k; fprintf(stderr, "[SW] %08X %X -> %X; entered before:", w[i].va, w[i].last, v);
                    for (k = 1; k <= 6; k++) fprintf(stderr, " %06X", ring[(rp - k) & 7]);
                    fprintf(stderr, "%c", 10);
                }
                w[i].last = v;
            }
        }
        return;
    }
    if (g_fn_watch_on == 3) {
        /* Mode 3: RECOMP_FN_ARGS=<hex va>[,<hex va>...] prints the first stack arguments
         * of each call to those functions (esp points at the return address). */
        static uint32_t tgt[8]; static int nt = -1; static unsigned shown;
        if (nt < 0) {
            const char *e = getenv("RECOMP_FN_ARGS"); nt = 0;
            while (e && *e && nt < 8) { tgt[nt++] = (uint32_t)strtoul(e, (char **)&e, 16); if (*e == ',') e++; else break; }
        }
        for (i = 0; i < nt; i++)
            if (tgt[i] == va && shown < 20000) {
                const uint32_t *a = (const uint32_t *)((uint8_t *)g_xbox_mem_offset + g_esp);
                shown++;
                fprintf(stderr, "[ARGS] %06X esp=%08X ret=%08X a0=%08X a1=%08X a2=%08X a3=%08X a4=%08X%c", (unsigned)va, (unsigned)g_esp, a[0], a[1], a[2], a[3], a[4], 10);
            }
        if (va == 0xA78E0u && g_fn_watch_on == 3 && getenv("RECOMP_XMV_STATE")) {
            static unsigned k;
            if ((++k % 60u) == 1) {
                const uint8_t *t = (const uint8_t *)g_xbox_mem_offset + g_ecx;
                const uint32_t *w = (const uint32_t *)t;
                fprintf(stderr, "[XMVST] this=%08X +2c=%X +38=%X +3c=%X +48=%X +60=%X +64=%X +b0=%X +b4=%X +b8=%X tick=%lu%c", (unsigned)g_ecx,
                        w[0x2c/4], w[0x38/4], w[0x3c/4], w[0x48/4], w[0x60/4], w[0x64/4], w[0xb0/4], w[0xb4/4], w[0xb8/4], (unsigned long)GetTickCount(), 10);
            }
        }
        return;
    }
    if (g_fn_watch_on == 2) {
        /* Mode 2: record every function ever called; once fn_arm.flag exists,
         * print the ones called for the first time after that (what a menu
         * action newly runs). Bitmap lookup keeps this cheap. */
        static unsigned char seen[0x262000 >> 0];
        static DWORD chk; static int armed;
        if (va < 0x11000u || va >= 0x262000u) return;
        if (!seen[va]) {
            seen[va] = 1;
            if (armed) fprintf(stderr, "[FN2] new function 0x%06X\n", (unsigned)va);
        }
        if (GetTickCount() - chk > 300) {
            chk = GetTickCount();
            if (!armed && GetFileAttributesA("fn_arm.flag") != INVALID_FILE_ATTRIBUTES) { armed = 1; fprintf(stderr, "[FN2] armed\n"); }
        }
        return;
    }
    if (!g_fn_watch_arm || va < 0x11000u || va >= 0x262000u)
        return;
    for (i = 0; i < s_fnw_n; i++)
        if (s_fnw[i].va == va) { s_fnw[i].n++; goto tick; }
    if (s_fnw_n < 4096) {
        s_fnw[s_fnw_n].va = va; s_fnw[s_fnw_n].n = 1; s_fnw_n++;
        fprintf(stderr, "[FN] first call 0x%06X (#%d) t=%lu\n", (unsigned)va, s_fnw_n, (unsigned long)GetTickCount());
    }
tick:
    if (GetTickCount() - last > 4000) {
        last = GetTickCount();
        fprintf(stderr, "[FN] counts:");
        for (i = 0; i < s_fnw_n; i++) fprintf(stderr, " %06X=%u", (unsigned)s_fnw[i].va, s_fnw[i].n);
        fprintf(stderr, "\n");
    }
}

/* RECOMP_MEM_LIVE=<file>: a debug console on guest memory. Lines appended to the file:
 *   poke <hexaddr> <hexval> [1|2|4]   write guest memory (default 4 bytes)
 *   peek <hexaddr> [dwords]           print dwords (default 1) to stderr as [MEM]
 *   watch <hexaddr> [1|2|4]           print [WATCH] whenever the value changes (up to 8)
 *   scan <hexval> / rescan <hexval>   find / narrow dword candidates in guest RAM (pd-i0w)
 * Writes are plain host stores, so use them on data, not on code or while the guest holds a lock. */
static DWORD WINAPI pd_mem_live(LPVOID unused)
{
    const char *path = getenv("RECOMP_MEM_LIVE");
    long consumed = 0;
    struct { uint32_t va; int sz; uint32_t last; } w[8]; int nw = 0;
    (void)unused;
    if (!path) return 0;
    for (;;) {
        FILE *f = fopen(path, "rb");
        Sleep(100);
        if (f) {
            char line[256];
            fseek(f, consumed, SEEK_SET);
            while (fgets(line, sizeof line, f)) {
                size_t n = strlen(line);
                unsigned a = 0, v = 0; int sz = 4, k; float fd[3];
                if (n == 0 || line[n - 1] != '\n') break;      /* incomplete line: read it next time */
                consumed += (long)n;
                if (sscanf(line, "poke %x %x %d", &a, &v, &sz) >= 2 && (sz == 1 || sz == 2 || sz == 4)) {
                    uint8_t *m = (uint8_t *)g_xbox_mem_offset + a;
                    memcpy(m, &v, (size_t)sz);
                    fprintf(stderr, "[MEM] poke %08X <- %X (%d bytes)\n", a, v, sz);
                } else if (sscanf(line, "peek %x %d", &a, &sz) >= 1) {
                    const uint32_t *m = (const uint32_t *)((uint8_t *)g_xbox_mem_offset + a);
                    if (sz < 1 || sz > 64) sz = 1;
                    fprintf(stderr, "[MEM] peek %08X:", a);
                    for (k = 0; k < sz; k++) fprintf(stderr, " %08X", m[k]);
                    fprintf(stderr, "\n");
                } else if (sscanf(line, "rscan %x %x", &a, &v) == 2) {
                    /* pd-cab: dwords (aligned, guest RAM) whose value lies in [lo, hi): pointers into a buffer */
                    const uint32_t *base = (const uint32_t *)((uint8_t *)g_xbox_mem_offset);
                    unsigned i, shown = 0;
                    fprintf(stderr, "[RSCAN] tick %lu %X..%X:", (unsigned long)GetTickCount(), a, v);
                    for (i = 0x10000u / 4; i < 0x04000000u / 4; i++) if (base[i] >= a && base[i] < v && shown++ < 400) fprintf(stderr, " %X=%X", i * 4, base[i]);
                    fprintf(stderr, "%c", 10);
                } else if (sscanf(line, "fdiff %f %f", &fd[0], &fd[1]) == 2) {
                    /* pd-cab: like sdiff, for floats: dwords that read as finite floats both times and grew by [min, max] */
                    extern uint32_t *g_snapbuf;
                    const uint32_t *base = (const uint32_t *)((uint8_t *)g_xbox_mem_offset);
                    unsigned i, shown = 0, total = 0;
                    if (g_snapbuf) {
                        fprintf(stderr, "[FDIFF] tick %lu %g..%g:", (unsigned long)GetTickCount(), fd[0], fd[1]);
                        for (i = 0x10000u / 4; i < 0x04000000u / 4; i++) {
                            float f0, f1, df; uint32_t u0 = g_snapbuf[i], u1 = base[i];
                            if (u0 == u1 || ((u0 >> 23) & 0xFF) == 0xFF || ((u1 >> 23) & 0xFF) == 0xFF) continue;
                            memcpy(&f0, &u0, 4); memcpy(&f1, &u1, 4); df = f1 - f0;
                            if (fabsf(f0) < 1e-3f || fabsf(f1) > 1e7f) continue;
                            if (df >= fd[0] && df <= fd[1]) { total++; if (shown++ < 300) fprintf(stderr, " %X(%g->%g)", i * 4, f0, f1); }
                        }
                        fprintf(stderr, " | %u total%c", total, 10);
                    }
                } else if (!strncmp(line, "snap", 4) || sscanf(line, "sdiff %d %d", &a, &v) == 2) {
                    /* pd-cab: "snap" copies guest RAM 0x10000..0x04000000; "sdiff <min> <max>" lists dwords whose
                     * integer increase since the snapshot is in [min, max] (counters advancing at a known rate). */
                    extern uint32_t *g_snapbuf;
                    uint32_t *snapbuf;
                    const uint32_t *base = (const uint32_t *)((uint8_t *)g_xbox_mem_offset);
                    unsigned i, shown = 0, total = 0;
                    if (line[0] == 's' && line[1] == 'n') {
                        if (!g_snapbuf) g_snapbuf = (uint32_t *)malloc(0x04000000u);
                        snapbuf = g_snapbuf;
                        if (snapbuf) memcpy(snapbuf + 0x10000u / 4, base + 0x10000u / 4, 0x04000000u - 0x10000u);
                        fprintf(stderr, "[SNAP] taken tick %lu%c", (unsigned long)GetTickCount(), 10);
                    } else if ((snapbuf = g_snapbuf) != NULL) {
                        fprintf(stderr, "[SDIFF] tick %lu %d..%d:", (unsigned long)GetTickCount(), (int)a, (int)v);
                        for (i = 0x10000u / 4; i < 0x04000000u / 4; i++) {
                            int32_t dlt = (int32_t)(base[i] - snapbuf[i]);
                            if (dlt >= (int32_t)a && dlt <= (int32_t)v) { total++; if (shown++ < 4000) fprintf(stderr, " %X(+%d)", i * 4, (int)dlt); }
                        }
                        fprintf(stderr, " | %u total%c", total, 10);
                    }
                } else if (sscanf(line, "scan %x", &a) == 1 || sscanf(line, "rescan %x", &a) == 1) {
                    /* pd-pc7/pd-i0w: cheat-engine style search of guest RAM for a dword. "scan <hex>" starts a
                     * candidate list (aligned dwords in 0x10000..0x04000000), "rescan <hex>" keeps the candidates
                     * that now hold <hex>. Prints the count and up to 24 addresses. */
                    static uint32_t *cand; static unsigned ncand;
                    unsigned i, o = 0; const uint32_t *base = (const uint32_t *)((uint8_t *)g_xbox_mem_offset);
                    if (line[0] == 's') {
                        if (!cand) cand = (uint32_t *)malloc(sizeof(uint32_t) * 4000000u);
                        ncand = 0;
                        for (i = 0x10000u / 4; i < 0x04000000u / 4 && ncand < 4000000u; i++) if (base[i] == a) cand[ncand++] = i * 4;
                    } else {
                        for (i = 0; i < ncand; i++) if (base[cand[i] / 4] == a) cand[o++] = cand[i];
                        ncand = o;
                    }
                    fprintf(stderr, "[SCAN] %X: %u candidates:", a, ncand);
                    for (i = 0; i < ncand && i < 80; i++) fprintf(stderr, " %X", cand[i]);
                    fprintf(stderr, "%c", 10);
                } else if (sscanf(line, "dw %x %x", &a, &v) >= 1) {
                    /* pd-nre: page-protect write watch on the guest dword at <hex> (0 = off): prints old/new value,
                     * registers and the guest return chain of the writer (RECOMP_WATCH machinery, armed at run time) */
                    extern int xbox_WatchArmRuntime(uint32_t va, uint32_t minv);
                    fprintf(stderr, "[MEM] dw %08X -> %s%c", a, xbox_WatchArmRuntime(a, (sscanf(line, "dw %x %x", &a, &v) >= 2) ? v : 0) ? "armed" : "FAILED", 10);
                } else if (sscanf(line, "fncount %d", &sz) == 1) {
                    extern volatile DWORD g_fncount_ms;
                    g_fncount_ms = (DWORD)sz; g_fn_watch_on = 6;
                    fprintf(stderr, "[MEM] fncount %d ms started%c", sz, 10);
                } else if (sscanf(line, "fnwatch %d", &sz) == 1) {
                    /* switch the per-call function hook off (0) after the quest has started, so profiling is not
                     * distorted by it (the RECOMP_ABI_CHECK call macro still tests the flag) */
                    g_fn_watch_on = sz;
                    fprintf(stderr, "[MEM] fn watch mode -> %d%c", sz, 10);
                } else if (sscanf(line, "follow %x %x %f %f %f %d", &a, &v, &fd[0], &fd[1], &fd[2], &sz) == 6) {
                    extern volatile DWORD g_fo_until; extern volatile uint32_t g_fo_dst, g_fo_src; extern volatile float g_fo_d[3];
                    g_fo_dst = a; g_fo_src = v; g_fo_d[0] = fd[0]; g_fo_d[1] = fd[1]; g_fo_d[2] = fd[2];
                    g_fo_until = GetTickCount() + (DWORD)sz; if (!g_fo_until) g_fo_until = 1;
                    fprintf(stderr, "[MEM] follow dst=%08X src=%08X d=%.1f,%.1f,%.1f for %d ms%c", a, v, fd[0], fd[1], fd[2], sz, 10);
                } else if (sscanf(line, "fnlog %x", &a) == 1) {
                    extern volatile uint32_t g_fl_va[16]; extern volatile int g_fl_n;
                    if (a == 0) g_fl_n = 0; else if (g_fl_n < 16) g_fl_va[g_fl_n++] = a;
                    fprintf(stderr, "[MEM] fnlog %08X (n=%d)%c", a, g_fl_n, 10);
                } else if (sscanf(line, "wfn %x", &a) == 1 && g_wfn_n < 4) {
                    memcpy((void *)&g_wfn_last[g_wfn_n], (uint8_t *)g_xbox_mem_offset + a, 4);
                    g_wfn_va[g_wfn_n] = a; g_wfn_n++;
                    fprintf(stderr, "[MEM] wfn armed on %08X%c", a, 10);
                } else if (sscanf(line, "pat %x %x %x", &a, &v, &sz) == 3) {
                    /* "pat v1 v2 v3": addresses A holding v1 where A+S holds v2 and A+2S holds v3 for some stride S
                     * (4..0x4000): finds the per-character table of (HP 20, 20, 10). */
                    const uint32_t *base = (const uint32_t *)((uint8_t *)g_xbox_mem_offset);
                    unsigned i, S, shown = 0; uint32_t v3 = (uint32_t)sz;
                    for (i = 0x10000u / 4; i < 0x03F00000u / 4 && shown < 24; i++) {
                        if (base[i] != a) continue;
                        for (S = 1; S < 0x1000u; S++) {
                            if (i + 2 * S >= 0x04000000u / 4) break;
                            if (base[i + S] == v && base[i + 2 * S] == v3) { fprintf(stderr, "[PAT] %X stride %X%c", i * 4, S * 4, 10); shown++; }
                        }
                    }
                    fprintf(stderr, "[PAT] done %X %X %X: %u hits%c", a, v, v3, shown, 10);
                } else if (!strncmp(line, "hub", 3)) {
                    g_pd_hub_req = 1;
                    fprintf(stderr, "[MEM] hub request%c", 10);
                } else if (sscanf(line, "quest %d", &sz) == 1) {
                    g_pd_quest_req = sz;
                    fprintf(stderr, "[MEM] quest request %d%c", sz, 10);
                } else if (sscanf(line, "stage %d", &sz) == 1) {
                    g_pd_stage_req = sz;
                    fprintf(stderr, "[MEM] stage request %d%c", sz, 10);
                } else if (sscanf(line, "watch %x %d", &a, &sz) >= 1 && nw < 8) {
                    if (sz != 1 && sz != 2 && sz != 4) sz = 4;
                    w[nw].va = a; w[nw].sz = sz; w[nw].last = 0xDEADBEEFu; nw++;
                }
            }
            fclose(f);
        }
        {
            int i;
            for (i = 0; i < nw; i++) {
                uint32_t v = 0;
                memcpy(&v, (uint8_t *)g_xbox_mem_offset + w[i].va, (size_t)w[i].sz);
                if (v != w[i].last) { fprintf(stderr, "[WATCH] %08X = %X (tick %lu)\n", w[i].va, v, (unsigned long)GetTickCount()); w[i].last = v; }
            }
        }
    }
    return 0;
}

static DWORD WINAPI pd_sampler(LPVOID unused)
{
    int pd_sample_stack = getenv("PD_SAMPLE_STACK") != NULL;
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    DWORD me = GetCurrentThreadId();
    static unsigned long long rips[200][64]; static DWORD tids[200][64]; int cnt[200] = {0};
    int s, k;
    (void)unused;
    /* PD_SAMPLE_DELAY=flag: sample 20 s each time sample.flag appears (the file is deleted when the round is printed), so one
     * boot can be profiled in several scenes. */
    for (;;) {
    int rearm = 0;
    { const char *dly = getenv("PD_SAMPLE_DELAY");
      if (dly && !strcmp(dly, "flag")) { rearm = 1; while (GetFileAttributesA("sample.flag") == INVALID_FILE_ATTRIBUTES) Sleep(500); }
      else Sleep(dly ? (DWORD)atoi(dly) * 1000u : 9000u); }
    memset(cnt, 0, sizeof cnt);
    for (s = 0; s < 200; s++) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 te; te.dwSize = sizeof(te);
        if (snap != INVALID_HANDLE_VALUE && Thread32First(snap, &te)) {
            do {
                if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == me || cnt[s] >= 64) continue;
                HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
                if (!h) continue;
                if (SuspendThread(h) != (DWORD)-1) {
                    CONTEXT c; c.ContextFlags = CONTEXT_CONTROL;
                    if (GetThreadContext(h, &c)) {
                        rips[s][cnt[s]] = (unsigned long long)(c.Rip - base); tids[s][cnt[s]] = te.th32ThreadID; cnt[s]++;
                        if (c.Rip - base >= 0x4000000ull && pd_sample_stack) {
                            /* a thread parked outside the exe (ntdll wait): add the first exe return addresses on its stack as
                             * pseudo-threads tid+k*0x10000000 so sampres.py names the guest function that called the wait */
                            int k = 0, q;
                            const unsigned long long *sp = (const unsigned long long *)c.Rsp;
                            for (q = 0; q < 1024 && k < 10 && cnt[s] < 64; q++) {
                                unsigned long long v; SIZE_T got = 0;
                                if (!ReadProcessMemory(GetCurrentProcess(), sp + q, &v, sizeof v, &got) || got != sizeof v) break;
                                if (v > base && v - base < 0x4000000ull) { rips[s][cnt[s]] = v - base; tids[s][cnt[s]] = te.th32ThreadID + (DWORD)(k + 1) * 0x10000000u; cnt[s]++; k++; }
                            }
                        }
                    }
                    ResumeThread(h);
                }
                CloseHandle(h);
            } while (Thread32Next(snap, &te));
            CloseHandle(snap);
        }
        Sleep(100);
    }
    for (s = 0; s < 200; s++) {
        fprintf(stderr, "[SAMPLE] %d:", s);
        for (k = 0; k < cnt[s]; k++) {
            /* pd-pc7: RIPs outside the exe image (D3D11 driver, kernel32, ...) are printed as module!offset */
            unsigned long long a = rips[s][k] + (unsigned long long)base; HMODULE hm = NULL; char nm[MAX_PATH];
            if (rips[s][k] >= 0x4000000ull && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(uintptr_t)a, &hm) && GetModuleFileNameA(hm, nm, sizeof nm)) {
                const char *b = strrchr(nm, 92); fprintf(stderr, " %lu=@%s+%llX", (unsigned long)tids[s][k], b ? b + 1 : nm, a - (unsigned long long)(uintptr_t)hm);
            } else
                fprintf(stderr, " %lu=%llX", (unsigned long)tids[s][k], rips[s][k]);
        }
        fprintf(stderr, "\n");
    }
    fflush(stderr);
    if (!rearm) break;
    DeleteFileA("sample.flag");
    fprintf(stderr, "[SAMPLE] round done\n");
    }
    return 0;
}

/* Recompiled entry point (generated by recomp pipeline) */
extern void xbe_entry_point(void);

/* ── VEH crash handler ─────────────────────────────────────── */

/*
 * Vectored Exception Handler for crash diagnostics.
 *
 * When the recompiled game hits an access violation, this handler prints
 * the faulting address, all Xbox register values, and a native stack trace.
 * This is your primary debugging tool during bring-up.
 *
 * Customize this for your game:
 *   - Add game-specific address checks (GPU register probes, etc.)
 *   - Add dumps of game-specific globals (heap handles, state flags)
 *   - Add SEH simulation if your game uses __try/__except
 */
/* Name the guest function a fault happened in, and recover the call chain.
 *
 * Recompiled code faults as ordinary native code, so the exception record
 * carries a host RIP and nothing else -- there is no guest program counter to
 * report, and the host address changes every build. Two things recover the
 * guest view:
 *
 *   - every generated function is a real symbol in the image (sub_005A03C0
 *     and so on), so the linker's PDB already maps host address back to guest
 *     function. dbghelp turns an anonymous RIP into that name.
 *
 *   - every lifted call pushes its guest return address onto the guest stack
 *     before jumping, so the stack still holds the chain. Scanning up from esp
 *     for values inside the code sections recovers it.
 *
 * ponytail: the stack scan is a scan, not a frame walk -- these are FPO frames
 * with no reliable ebp chain, so there is nothing to walk. It over-reports,
 * since addresses from returned-from calls linger below esp, but naming the
 * guest function is the whole question at a fault.
 *
 * Requires linking dbghelp and keeping the .pdb beside the .exe.
 */
static void print_guest_context(void *rip)
{
    /* SYMBOL_INFO is variable-length: the name is written past the struct, so
     * it must be over-allocated with MaxNameLen set to the slack. */
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    DWORD64 disp = 0;

    memset(buf, 0, sizeof(buf));
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    if (SymFromAddr(GetCurrentProcess(), (DWORD64)(uintptr_t)rip, &disp, sym))
        fprintf(stderr, "  in %s+0x%llX\n",
                sym->Name, (unsigned long long)disp);

    if (g_xbox_mem_offset && g_esp) {
        const uint32_t *sp =
            (const uint32_t *)((uintptr_t)g_xbox_mem_offset + g_esp);
        int shown = 0, i;
        fprintf(stderr, "  guest stack (return addresses, innermost first):\n");
        for (i = 0; i < 256 && shown < 24; i++) {
            uint32_t v = sp[i];
            if (v > g_xbox_code_lo && v < g_xbox_code_hi) {
                fprintf(stderr, "    [esp+%-4d] 0x%08X\n", i * 4, v);
                shown++;
            }
        }
        /* When the frame is gone, say so, and fall back on what is left.
         *
         * A fault inside a block copy leaves no return address in range: the
         * copy is a host memmove and its caller's frame is further up than
         * this reaches. Worse, a copy that ran past its buffer overwrites the
         * stack itself, so the list comes out empty exactly when it is most
         * needed -- and an empty list under a promising heading reads like
         * "nothing to see" rather than "the evidence is destroyed". */
        if (!shown)
            fprintf(stderr, "    (no return addresses in range)\n");

        /* Everything below only pays for itself when the scan came back
         * empty. A crash where the chain was recovered already says what
         * happened, and burying that under another twenty lines makes the
         * common report worse to read. */
        if (!shown) {
            uint32_t k;

            /* The indirect-call history, which the stack cannot overwrite.
             * recomp_manual.c prints this too, but only from
             * recomp_icall_fail_log -- a page fault inside a block copy
             * never reaches that path, and this is the same information by
             * the route the fault actually took.
             *
             * Declared here rather than included: main.c does not pull in
             * recomp_types.h, and recomp_manual.c spells it the same way. */
            extern volatile uint32_t g_icall_trace[16];
            extern volatile uint32_t g_icall_trace_idx;

            fprintf(stderr, "  recent ICALL targets:");
            for (k = 0; k < 16; k++)
                fprintf(stderr, " %08X",
                        g_icall_trace[(g_icall_trace_idx + k) & 15]);
            fprintf(stderr, "\n");

            /* The frame unfiltered. The pointers and lengths handed to
             * whatever faulted live here and look nothing like code, so the
             * filter above drops exactly what is wanted. On the crash this
             * was written for, one of these words was the end of the mapped
             * RAM mirror -- which is what identified the fault as a copy
             * running off its buffer rather than a stray pointer. */
            for (i = 0; i < 20; i++)
                fprintf(stderr, "    raw[esp+%-4d] 0x%08X\n", i * 4, sp[i]);
        }
    }
}

/* The emulated APU.
 *
 * Two entry points that had no callers anywhere in the tree. apu_mmio_hook.c
 * documents apu_hook_handle_mmio as "called from VEH in main.c" and no main.c
 * called it; apu_decode_and_handle's first line is `if (!g_apu_state) return
 * false` and nothing assigned g_apu_state. So with RECOMP_AC97_READY set the
 * APU's registers were unmapped to be trapped, and every trap was then
 * declined and surfaced as an access violation on the first register
 * DirectSound touched.
 *
 * Declared rather than included so this does not depend on src/apu being on
 * the include path. */
typedef struct MCPXAPUState MCPXAPUState;
extern MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr);
extern MCPXAPUState *g_apu_state;
extern bool apu_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                                 uint32_t fault_xbox_va, int is_write);

/* The trapped span, matching what MemoryLayoutInit unmaps: the APU's own
 * 512 KB, not the whole MCPX aperture. AC'97 above it stays plain memory,
 * which is what the codec-ready bit needs. */
#define APU_TRAP_BASE 0xFE800000u
#define APU_TRAP_END  0xFE880000u

extern void xbox_OhciInit(void);
extern int  xbox_OhciOwnsAddress(uint32_t xbox_va);
extern int  xbox_OhciHandleMmio(void *ctx, uint32_t xbox_va);
extern void xbox_OhciReport(void);

static LONG CALLBACK veh_handler(PEXCEPTION_POINTERS ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION
            && (ep->ExceptionRecord->ExceptionCode & 0xF0000000u) == 0xC0000000u
            && ep->ExceptionRecord->ExceptionCode != 0xC0000194u) {
        /* pd-ab1: non-AV fatal exceptions used to kill the process silently */
        fprintf(stderr, "[CRASH2] exception 0x%08lX at RIP=0x%llX (module+0x%llX) eax=%08X ecx=%08X edx=%08X ebx=%08X esi=%08X edi=%08X esp=%08X%c",
            ep->ExceptionRecord->ExceptionCode, (unsigned long long)ep->ContextRecord->Rip,
            (unsigned long long)(ep->ContextRecord->Rip - (uintptr_t)GetModuleHandleA(NULL)),
            g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi, g_esp, 10);
        fflush(stderr);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        uintptr_t fault_addr = ep->ExceptionRecord->ExceptionInformation[1];

        /*
         * GPU register probe at 0xFD000000 range.
         * Some games probe NV2A registers directly. On real hardware this
         * returns GPU state; here we just skip the instruction.
         * TODO: Implement mini x86-64 decoder for instruction skipping,
         * or connect to the xbox_nv2a library for proper handling.
         */
        if (fault_addr >= 0xFD000000 && fault_addr < 0xFE000000) {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        /* APU registers -> the emulated APU.
         *
         * The other half of RECOMP_AC97_READY: reporting the codec ready is
         * what lets DirectSoundCreate past its first gate, and from there it
         * drives the APU directly, so the registers have to fault to be seen.
         * "Handled" means the access was decoded and the instruction stepped
         * over, so execution resumes instead of unwinding. */
        {
            uint32_t xbox_va =
                (uint32_t)(fault_addr - (uintptr_t)g_xbox_mem_offset);

            if (xbox_va >= APU_TRAP_BASE && xbox_va < APU_TRAP_END
                    && apu_hook_handle_mmio(
                           ep->ContextRecord, fault_addr, xbox_va,
                           ep->ExceptionRecord->ExceptionInformation[0]
                               ? 1 : 0)) {
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }

        {
            uint32_t xva = (uint32_t)(fault_addr - (uintptr_t)g_xbox_mem_offset);
            if (xva >= 0xFED00000u && xva < 0xFED10000u && xbox_OhciOwnsAddress(xva)
                    && xbox_OhciHandleMmio(ep->ContextRecord, xva))
                return EXCEPTION_CONTINUE_EXECUTION;
        }

        fprintf(stderr, "[CRASH] Access violation at RIP=0x%llX, fault addr=0x%llX (%s)\n",
            (unsigned long long)ep->ContextRecord->Rip,
            (unsigned long long)fault_addr,
            ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read");
        fprintf(stderr, "  RIP module offset: 0x%llX%c", (unsigned long long)(ep->ContextRecord->Rip - (uintptr_t)GetModuleHandleA(NULL)), 10);
        fprintf(stderr, "  Xbox regs: eax=0x%08X ecx=0x%08X edx=0x%08X esp=0x%08X\n",
            g_eax, g_ecx, g_edx, g_esp);
        fprintf(stderr, "  Xbox regs: ebx=0x%08X esi=0x%08X edi=0x%08X\n",
            g_ebx, g_esi, g_edi);
        fprintf(stderr, "  Xbox VA of fault: 0x%08X\n",
            (uint32_t)(fault_addr - (uintptr_t)g_xbox_mem_offset));
        print_guest_context((void *)ep->ContextRecord->Rip);
        { /* pd-dz0 (2026-10-19): dump the memory the crashing registers point at */
            uint32_t rr[5] = { g_esi, g_edi, g_ebx, g_ecx, g_edx }; const char *rn[5] = { "esi", "edi", "ebx", "ecx", "edx" }; int q, w;
            for (q = 0; q < 5; q++) {
                uint32_t va = rr[q];
                if (!((va >= 0x10000u && va < 0x04000000u) || (va >= 0x80010000u && va < 0x84000000u))) continue;
                __try {
                    fprintf(stderr, "  [DUMP %s=%08X]", rn[q], va);
                    for (w = 0; w < 16; w++) fprintf(stderr, " %08X", *(volatile uint32_t *)((uint8_t *)g_xbox_mem_offset + va + w * 4));
                    fprintf(stderr, "%c", 10);
                } __except (EXCEPTION_EXECUTE_HANDLER) { fprintf(stderr, " (unreadable)%c", 10); }
            }
        }


        /*
         * TODO: Add game-specific diagnostics here. Examples:
         *
         * Dump CRT heap handle:
         *   uint32_t heap = *(uint32_t *)((uint8_t *)g_xbox_mem_offset + HEAP_HANDLE_VA);
         *   fprintf(stderr, "  CRT heap handle: 0x%08X\n", heap);
         *
         * Dump game state:
         *   uint32_t state = *(uint32_t *)((uint8_t *)g_xbox_mem_offset + GAME_STATE_VA);
         *   fprintf(stderr, "  Game state: %u\n", state);
         */

        /* Print native stack return addresses for debugging */
        {
            uintptr_t *sp = (uintptr_t *)ep->ContextRecord->Rsp;
            fprintf(stderr, "  Native stack (first 8 return addrs):\n");
            for (int i = 0; i < 64 && sp[i]; i++) {
                if (sp[i] >= 0x140000000ULL && sp[i] < 0x150000000ULL) {
                    fprintf(stderr, "    [%d] 0x%llX\n", i, (unsigned long long)sp[i]);
                }
            }
        }
        fflush(stderr);
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

/* ── WinMain ───────────────────────────────────────────────── */

/* settings.ini (next to PLAY.bat): read at startup, turned into the RECOMP_* switches the runtime already understands.
 * A switch already present in the environment (set in a .bat) wins over the file. Keys, one per line, `key=value`, `;`/`#` comments:
 *   fullscreen=0|1  window=auto|WxH  render_scale=auto|1..8  aspect=4:3|16:9  interpolation=1|0  vsync=1|0  keys=mmo|classic */
static void pd_setenv_default(const char *name, const char *val)
{
    if (getenv(name)) return;
    _putenv_s(name, val);
}
static int pd_truthy(const char *v) { return atoi(v) || !_stricmp(v, "on") || !_stricmp(v, "yes") || !_stricmp(v, "true"); }
static void pd_load_settings(void)
{
    static const char *paths[] = { "../settings.ini", "settings.ini" };
    FILE *f = NULL; char line[256]; int i;
    for (i = 0; i < 2 && !f; i++) f = fopen(paths[i], "r");
    if (!f) { fprintf(stderr, "[SETTINGS] no settings.ini%c", 10); return; }
    fprintf(stderr, "[SETTINGS] reading settings.ini%c", 10);
    pd_setenv_default("RECOMP_KEYS", "mmo");
    while (fgets(line, sizeof line, f)) {
        char *k = line, *v, *e;
        while (*k == ' ' || *k == '\t') k++;
        if (*k == ';' || *k == '#' || *k == '[' || !(v = strchr(k, '='))) continue;
        *v++ = 0;
        for (e = k + strlen(k); e > k && (e[-1] == ' ' || e[-1] == '\t'); ) *--e = 0;
        while (*v == ' ' || *v == '\t') v++;
        for (e = v + strlen(v); e > v && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'); ) *--e = 0;
        fprintf(stderr, "[SETTINGS] %s=%s%c", k, v, 10);
        if (!_stricmp(k, "fullscreen")) pd_setenv_default("RECOMP_HW_FULLSCREEN", pd_truthy(v) ? "1" : "0");
        else if (!_stricmp(k, "window")) { if (_stricmp(v, "auto") && *v) pd_setenv_default("RECOMP_WINDOW_SIZE", v); }
        else if (!_stricmp(k, "render_scale")) { if (_stricmp(v, "auto") && *v) pd_setenv_default("RECOMP_HW_SCALE", v); }
        else if (!_stricmp(k, "aspect")) { if (strcmp(v, "4:3") && *v) pd_setenv_default("RECOMP_HW_ASPECT", v); }
        else if (!_stricmp(k, "interpolation")) pd_setenv_default("RECOMP_INTERP", pd_truthy(v) ? "1" : "0");
        else if (!_stricmp(k, "vsync")) pd_setenv_default("RECOMP_HW_VSYNC", pd_truthy(v) ? "1" : "0");
        else if (!_stricmp(k, "keys")) { if (!_stricmp(v, "classic")) _putenv_s("RECOMP_KEYS", ""); else if (!_stricmp(v, "mmo")) _putenv_s("RECOMP_KEYS", "mmo"); }
    }
    fclose(f);
}

/* ---- Name entry typing (2026-10-19) --------------------------------------------------------------------------------------------
 * The profile NAME ENTRY screen (menu script playdatasel_en.ssb) is an on-screen keyboard. With the physical keyboard, letters, digits,
 * Space, Backspace, Enter (= OK) and Esc (= back) are translated into the game's own pad input: the cursor is walked over the grid with
 * d-pad pulses and A is pressed, closed loop on the cursor index the game keeps in its screen object. Nothing is poked into the name buffer.
 * Screen object (guest heap, found by the label string "PLAYER :" at +0x20 from the cursor word; address varies):
 *   cursor-0x14,-0x10,-0x0C = 1,1,1 while the keyboard has the input (0,x,3 after Back, 0,1,2 on the overwrite prompt)
 *   cursor  = 0-9 digits, 10-35 a-z, 36 Space, 37 Caps, 38 Backspace, 39 OK;  +4 = name length;  +8 = caps flag;  +0x14 = name bytes
 * g_pd_text_input switches the layout's letter keys off while the screen is active, so typing does not also move the pad. */
extern volatile int g_pd_text_input;
extern volatile unsigned g_pd_pulse_dig, g_pd_pulse_ana;
extern int xbox_FramebufferKeyDown(int vk);
#define NEM(a) (*(volatile uint32_t *)((uint8_t *)g_xbox_mem_offset + (a)))
static uint32_t g_ne_base;
static int g_ne_q[64], g_ne_qn;
static int ne_valid(uint32_t b)
{
    int ok = 0;
    if (!b || b < 0x100000u || b > 0x03F00000u) return 0;
    __try {
        ok = NEM(b - 0x14) == 1 && NEM(b - 0x10) <= 1 && NEM(b - 0x0C) == 1 && NEM(b) < 40 && NEM(b + 4) <= 32 &&
             NEM(b + 0x20) == 0x59414C50u && NEM(b + 0x24) == 0x3A205245u;
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    return ok;
}
static uint32_t ne_find(void)
{
    uint32_t a;
    __try {
        for (a = 0x00100000u; a < 0x03F00000u; a += 4)
            if (NEM(a) == 0x59414C50u && NEM(a + 4) == 0x3A205245u && ne_valid(a - 0x20)) return a - 0x20;
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    return 0;
}
/* edge detection of the keys we turn into characters; pushes codes: 0-9 digit, 10-35 letter (+100 = Shift held), 36 space, 50 back, 51 enter, 52 esc */
static void ne_poll_keys(void)
{
    static unsigned char prev[256];
    static const int vks[] = { VK_SPACE, VK_BACK, VK_RETURN, VK_ESCAPE };
    int v, i;
    for (v = 0; v < 256; v++) {
        int is = (v >= '0' && v <= '9') || (v >= 'A' && v <= 'Z');
        for (i = 0; i < 4; i++) if (v == vks[i]) is = 1;
        if (!is) continue;
        {
            int d = xbox_FramebufferKeyDown(v) ? 1 : 0;
            if (d && !prev[v] && g_ne_qn < 64) {
                int code = v >= '0' && v <= '9' ? v - '0' : v >= 'A' && v <= 'Z' ? 10 + v - 'A' : v == VK_SPACE ? 36 : v == VK_BACK ? 50 : v == VK_RETURN ? 51 : 52;
                if (code >= 10 && code < 36 && (xbox_FramebufferKeyDown(VK_LSHIFT) || xbox_FramebufferKeyDown(VK_RSHIFT))) code += 100;
                g_ne_q[g_ne_qn++] = code;
            }
            prev[v] = (unsigned char)d;
        }
    }
}
static void ne_wait(int ms)
{
    int t;
    for (t = 0; t < ms; t += 5) { Sleep(5); ne_poll_keys(); }
}
static void ne_press_ms(unsigned dig, unsigned ana, int hold)
{
    g_pd_pulse_dig = dig; g_pd_pulse_ana = ana;
    ne_wait(hold);
    g_pd_pulse_dig = 0; g_pd_pulse_ana = 0;
    ne_wait(70);
}
static void ne_press(unsigned dig, unsigned ana) { ne_press_ms(dig, ana, 90); }
static int ne_row(int i) { return i < 36 ? i / 10 : i == 36 ? 3 : 4; }
static int ne_x2(int i) { return i < 36 ? (i % 10) * 2 : i == 36 ? 15 : i == 37 ? 2 : i == 38 ? 8 : 15; }
static int ne_goto(uint32_t b, int tgt)
{
    int iter, retry = 0;
    for (iter = 0; iter < 60; iter++) {
        int cur = (int)NEM(b), before = cur, t;
        unsigned d;
        if (!ne_valid(b)) return 0;
        if (cur == tgt) return 1;
        if (ne_row(cur) != ne_row(tgt)) d = ne_row(tgt) > ne_row(cur) ? 2u : 1u;
        else d = ne_x2(tgt) < ne_x2(cur) ? 4u : 8u;
        ne_press(d, 0);
        for (t = 0; t < 300 && (int)NEM(b) == before; t += 10) ne_wait(10);
        if ((int)NEM(b) == before && ++retry > 3) return 0;
    }
    return 0;
}
static DWORD WINAPI pd_nameentry_thread(LPVOID unused)
{
    static DWORD lastscan;
    (void)unused;
    for (;;) {
        Sleep(4);
        ne_poll_keys();
        if (g_ne_base && !ne_valid(g_ne_base)) g_ne_base = 0;
        if (!g_ne_base && g_ne_qn > 0 && GetTickCount() - lastscan > 1500) { lastscan = GetTickCount(); g_ne_base = ne_find(); fprintf(stderr, "[NAME] scan: base=%08X (%d keys queued)%c", g_ne_base, g_ne_qn, 10); }
        g_pd_text_input = g_ne_base ? 1 : 0;
        if (!g_ne_base) { g_ne_qn = 0; continue; }
        while (g_ne_qn > 0 && g_ne_base && ne_valid(g_ne_base)) {
            uint32_t b = g_ne_base; int code = g_ne_q[0], i, upper = 0;
            for (i = 1; i < g_ne_qn; i++) g_ne_q[i - 1] = g_ne_q[i];
            g_ne_qn--;
            if (code >= 100) { code -= 100; upper = 1; }
            if (code == 50) { ne_press_ms(0, 0x04, 45); ne_wait(150); }                   /* X = Backspace */
            else if (code == 52) ne_press(0, 0x02);              /* B = back */
            else if (code == 51) { if (ne_goto(b, 39)) ne_press(0, 0x01); }
            else {
                uint32_t len0 = NEM(b + 4);
                if (code < 36 && (int)NEM(b + 8) != (code >= 10 ? upper : 0)) {   /* digits only exist on the lower-case page */
                    int t; ne_press(0, 0x08);                    /* Y = Caps */
                    for (t = 0; t < 400 && (int)NEM(b + 8) != (code >= 10 ? upper : 0); t += 10) ne_wait(10);
                }
                if (ne_goto(b, code)) { ne_press(0, 0x01); for (i = 0; i < 300 && NEM(b + 4) == len0; i += 10) ne_wait(10); }
            }
        }
    }
    return 0;
}

/* Hybrid-CPU / power-throttling set-up, run first thing in WinMain (the exe is a WIN32-subsystem build: main() below is not its entry). */
/* pd-vnc: D3D's make-space wait (sub_00246261) blocks on the event at miniport (0x2532A8) + 0x1A4, which the graphics interrupt sets
 * for software method 5 (pushbuffer method 0x100, data 5). The executor now hands such methods to this hook. Needs
 * RECOMP_TITLE_KEVENTS=1 (default here) so that wait really blocks on, and is woken by, the guest-built event. */
extern void (*g_nv2a_swm_hook)(uint32_t subch, uint32_t data);
extern void xbox_GuestKeSetEvent(uint32_t va);
extern void xbox_EnableGuestEvent(uint32_t va);
static void pd_swm_hook(uint32_t subch, uint32_t data)
{
    if (subch == 0 && (data & 0x1Fu) == 5u)
        xbox_GuestKeSetEvent(0x002532A8u + 0x1A4u);
}

static void pd_cpu_setup(void)
{
    xbox_EnableGuestEvent(0x002532A8u + 0x1A4u);
    if (!getenv("RECOMP_SWM5") || atoi(getenv("RECOMP_SWM5"))) g_nv2a_swm_hook = pd_swm_hook;
    {   /* RECOMP_PCORES=1: keep the process on the performance cores of a hybrid CPU (experiment: E-core placement). */
        const char *pc = getenv("RECOMP_PCORES");
        if (pc && atoi(pc)) {
            DWORD len = 0; BYTE *buf; DWORD_PTR mask = 0; BYTE maxclass = 0;
            GetLogicalProcessorInformationEx(RelationProcessorCore, NULL, &len);
            buf = len ? (BYTE *)malloc(len) : NULL;
            if (buf && GetLogicalProcessorInformationEx(RelationProcessorCore, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf, &len)) {
                DWORD off = 0;
                for (off = 0; off < len; ) { PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX e = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf + off); if (e->Processor.EfficiencyClass > maxclass) maxclass = e->Processor.EfficiencyClass; off += e->Size; }
                for (off = 0; off < len; ) { PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX e = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf + off); if (e->Processor.EfficiencyClass == maxclass) mask |= e->Processor.GroupMask[0].Mask; off += e->Size; }
                if (mask) { SetProcessAffinityMask(GetCurrentProcess(), mask); fprintf(stderr, "[CPU] pinned to performance cores, mask %llx (class %u)\n", (unsigned long long)mask, (unsigned)maxclass); }
            }
            free(buf);
        }
    }
    {   /* Hybrid CPUs (P/E cores): opt out of EcoQoS throttling, and keep the 1 ms timer
         * resolution (vblank pacing) when the window is covered. RECOMP_THROTTLE=1 keeps Windows' default. */
        const char *t = getenv("RECOMP_THROTTLE");
        if (!(t && atoi(t))) {
            PROCESS_POWER_THROTTLING_STATE st;
            st.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
            st.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
            st.StateMask = 0;
            SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &st, sizeof st);
        }
    }
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow)
{
    void *xbe_data = NULL;
    size_t xbe_size = 0;

    (void)hInstance;
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    pd_cpu_setup();
    pd_load_settings();
    {   /* restarted by the "Go to Xbox Live" hand-off (kernel_bridge.c): wait for the old process to be gone first */
        char pb[32];
        if (GetEnvironmentVariableA("PD_RELAUNCH_PARENT", pb, sizeof pb)) {
            HANDLE ph = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)atoi(pb));
            if (ph) { WaitForSingleObject(ph, 15000); CloseHandle(ph); }
            SetEnvironmentVariableA("PD_RELAUNCH_PARENT", NULL);
        }
    }
    /* Unbuffered output for immediate visibility during debugging */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    printf("=== YOUR_GAME_NAME - Static Recompilation ===\n");
    printf("Loading XBE...\n");

    /* Install VEH handler (first handler in chain) */
    /* Load symbols up front rather than from inside the handler: at fault
     * time the process is already in a bad way, and SymInitialize
     * allocates. Failure is not fatal -- the handler prints no name. */
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    AddVectoredExceptionHandler(1, veh_handler);

    /* Step 1: Load XBE */
    if (!load_xbe(YOUR_GAME_XBE_PATH, &xbe_data, &xbe_size)) {
        MessageBoxA(NULL, "Failed to load default.xbe.\n"
                    "Place the game files in the 'game' subdirectory.",
                    "Recomp", MB_ICONERROR);
        return 1;
    }
    printf("XBE loaded: %zu bytes\n", xbe_size);

    /* Step 2: Initialize Xbox memory layout */
    printf("Initializing Xbox memory layout...\n");
    if (!xbox_MemoryLayoutInit(xbe_data, xbe_size)) {
        MessageBoxA(NULL, "Failed to initialize Xbox memory layout.\n"
                    "The required virtual address range may be unavailable.",
                    "Recomp", MB_ICONERROR);
        free(xbe_data);
        return 1;
    }

    g_xbox_mem_offset = xbox_GetMemoryOffset();
    if (getenv("RECOMP_USB")) {
        DWORD oldp;
        xbox_OhciInit();
        /* OHCI0 at 0xFED00000, OHCI1 at 0xFED08000, 4 KB each: trap the pages so
         * register accesses reach the model instead of reading plain RAM. */
        if (VirtualProtect((char *)g_xbox_mem_offset + 0xFED00000u, 0x1000, PAGE_NOACCESS, &oldp) &&
            VirtualProtect((char *)g_xbox_mem_offset + 0xFED08000u, 0x1000, PAGE_NOACCESS, &oldp))
            fprintf(stderr, "  USB: OHCI0/OHCI1 pages trapped for MMIO\n");
        else
            fprintf(stderr, "  USB: OHCI page trap FAILED (%lu)\n", GetLastError());
    }
    printf("Xbox memory mapped. Offset: 0x%llX\n", (unsigned long long)g_xbox_mem_offset);

    /* Bring up the emulated APU.
     *
     * Gated on the same variable that unmaps its registers, because the two
     * halves are useless apart: a trap with no model declines every access,
     * and a model nothing traps into never sees a register. The APU walks
     * Xbox physical RAM to find voice buffers, so it gets the guest RAM
     * base. */
    if (getenv("RECOMP_AC97_READY")) {
        g_apu_state = mcpx_apu_init_standalone((uint8_t *)xbox_GetMemoryBase());
        fprintf(stderr, "[BOOT] emulated APU %s\n",
                g_apu_state ? "up" : "FAILED to initialise");
    }

    /* Step 3: Initialize Xbox kernel */
    printf("Initializing Xbox kernel replacement...\n");
    xbox_kernel_init();

    /* Step 4: Set game directory for file I/O path translation */
    {
        extern void xbox_path_init(const char *game_dir, const char *save_dir);
        xbox_path_init(YOUR_GAME_DIR, NULL);
    }

    /* Step 5: Initialize kernel bridge (thunk table in Xbox memory) */
    printf("Initializing kernel bridge...\n");
    xbox_kernel_bridge_init();

    /* Step 6: Initialize stack */
    g_esp = XBOX_STACK_TOP;

    /*
     * TODO: Pre-initialize CRT globals if needed.
     *
     * Many Xbox games use the MSVC CRT. The CRT's __heap_init sets up a
     * heap descriptor at a game-specific address. You may need to
     * pre-initialize __active_heap to avoid small-block heap issues:
     *
     *   uint32_t *active_heap = (uint32_t *)((uint8_t *)g_xbox_mem_offset + ACTIVE_HEAP_VA);
     *   *active_heap = 1;  // 1 = system heap (HeapAlloc), avoids SBH init
     *
     * Find ACTIVE_HEAP_VA by searching the disassembly for __heap_init
     * or by looking for the CRT's __active_heap global in the data section.
     */

    printf("\n=== Initialization complete ===\n");
    printf("Entry point: 0x%08X\n", YOUR_GAME_ENTRY_POINT);
    printf("ESP: 0x%08X\n", g_esp);

    /* Build the flat dispatch table before the guest runs.
     *
     * Without this recomp_lookup falls back to a binary search over the
     * whole function table -- roughly log2(n) branches on *every*
     * indirect call, which for a 45,000-function C++ title is about 16
     * every time the game goes through a vtable. Half-Life 2 spent most
     * of its static initialisation inside recomp_lookup for exactly this
     * reason, and it read as a hang.
     *
     * Optional by design: if the allocation fails the search still works,
     * so a failure is worth one line and not a fatal error. */
    if (!recomp_dispatch_init())
        fprintf(stderr, "[BOOT] flat dispatch unavailable; "
                        "indirect calls will use the binary search\n");

    /* Arm the hang watchdog. Does nothing unless RECOMP_WATCHDOG_SECS is set,
     * and must be called from this thread -- the guest registers it samples are
     * thread-local, so it has to be handed the copies belonging to the thread
     * that runs guest code.
     *
     * Not optional boilerplate: without this call RECOMP_WATCHDOG_SECS is
     * silently inert, and the one diagnostic that tells a hang from slowness
     * does nothing while appearing to be set. */
    xbox_WatchdogStart();
    if (getenv("RECOMP_FN_WATCH")) { int m = atoi(getenv("RECOMP_FN_WATCH")); g_fn_watch_on = (m >= 2 && m <= 5) ? m : 1; }
    { const char *f6 = getenv("RECOMP_FPS60"); g_fps60 = f6 && atoi(f6); if (g_fps60 && !g_fn_watch_on) g_fn_watch_on = 7; }
    if (getenv("RECOMP_SAMPLE")) CreateThread(NULL, 0, pd_sampler, NULL, 0, NULL);
    if (getenv("RECOMP_MEM_LIVE")) CreateThread(NULL, 0, pd_mem_live, NULL, 0, NULL);
    if (getenv("RECOMP_KEYBOARD")) CreateThread(NULL, 0, pd_nameentry_thread, NULL, 0, NULL);
    if (getenv("RECOMP_MOVIE_CLOCKS") || getenv("RECOMP_MOVIE_FCLOCKS")) CreateThread(NULL, 0, pd_movie_pace_thread, NULL, 0, NULL);   /* pd-cab experiment */

    /* Step 7: Call the recompiled entry point */
    printf("\nStarting game...\n");
    fflush(stdout);

    if (getenv("PD_SELFTEST_SCRAMBLE")) {
        extern void sub_000B5F00(void); extern void sub_000B5F40(void);
        uint8_t *mem = (uint8_t *)g_xbox_mem_offset; uint32_t va = 0x02F00000u, i, bad = 0; uint8_t orig[512];
        g_esp = 0x00F7F000u;
        for (i = 0; i < 512; i++) { mem[va + i] = (uint8_t)(i * 7 + 3 + (i >> 3)); orig[i] = mem[va + i]; }
        g_esi = va; g_edi = 512; sub_000B5F00();
        { uint32_t h = 0; for (i = 0; i < 512; i++) h = h * 131u + mem[va + i]; fprintf(stderr, "[SELFTEST] scrambled hash=%08X first=%02X%02X%02X%02X%c", h, mem[va], mem[va+1], mem[va+2], mem[va+3], 10); }
        g_esi = va; g_edi = 512; sub_000B5F40();
        for (i = 0; i < 512; i++) if (mem[va + i] != orig[i]) { if (bad++ < 8) fprintf(stderr, "[SELFTEST] mismatch at %u: got %02X want %02X%c", i, mem[va + i], orig[i], 10); }
        fprintf(stderr, "[SELFTEST] roundtrip mismatches=%u%c", bad, 10); fflush(stderr);
    }
    xbe_entry_point();

    printf("\nGame returned. Cleaning up...\n");

    /* Cleanup */
    xbox_kernel_shutdown();
    xbox_MemoryLayoutShutdown();
    free(xbe_data);

    return 0;
}

/* ── XBE Loading ───────────────────────────────────────────── */

static BOOL load_xbe(const char *path, void **out_data, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open XBE: %s\n", path);
        return FALSE;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        fclose(f);
        return FALSE;
    }

    void *data = malloc((size_t)size);
    if (!data) {
        fclose(f);
        return FALSE;
    }

    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return FALSE;
    }

    fclose(f);
    *out_data = data;
    *out_size = (size_t)size;
    return TRUE;
}

/* Console entry point (for debugging -- lets you see printf output) */
/* Hybrid-GPU laptops: ask the NVIDIA Optimus / AMD PowerXpress drivers for the discrete GPU. */
__declspec(dllexport) unsigned long NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return WinMain(GetModuleHandle(NULL), NULL, GetCommandLineA(), SW_SHOW);
}
