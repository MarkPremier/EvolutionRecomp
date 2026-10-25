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
#include <stddef.h>   /* ptrdiff_t */

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

#define TUROK_MEM32(va) (*(volatile uint32_t *)((uintptr_t)(va) + g_xbox_mem_offset))

/* ── D3D fence wait (D3D_BlockOnTime, XDK 4627) ────────────── */

/*
 * The stock XDK D3D waits for a fence by spinning on the GPU semaphore until
 * it passes the requested time. Turok's (non-LTCG) device keeps the write
 * sequence at device+0x30 and the semaphore pointer at device+0x34 -- four
 * bytes later than the D3D8LTCG layout in docs/technical/d3d8ltcg-device-
 * context.md. The pushbuffer executor writes BACK_END_WRITE_SEMAPHORE_RELEASE
 * values only where nv2a_pb_set_semaphore_target() points it, and nothing
 * pointed it anywhere, so the semaphore sat at the value D3D initialised it to
 * and the first fence wait never returned (watchdog: PUT==GET, REF 0).
 *
 * Wraps rather than reimplements: the generated body keeps the kickoff of the
 * pushbuffer holding the release, which a replacement would have to repeat.
 * Re-read every call, because D3D rewrites the field on device reset.
 * Needs RECOMP_PB_EXEC=1; without the executor nothing releases anything.
 */
#define TUROK_D3D_DEVICE_PTR   0x001A1AD0u
#define NV2A_USER_DMA_PUT_VA   0xFD800040u
#define NV2A_USER_DMA_GET_VA   0xFD800044u

/*
 * Lost-release rescue.
 *
 * When the executor desyncs inside a CALLed pushbuffer (it reads vertex data
 * -- 0x3F800000, a float 1.0 -- as a JUMP, logged as [PB] bad target), it
 * abandons the rest of that subroutine, and any semaphore releases in it are
 * never written. The GPU is then idle (GET == PUT) with the semaphore stuck
 * behind the fence D3D is waiting on, and the unbounded spin in
 * D3D_BlockOnTime never ends. Turok froze this way right after loading its
 * first interactive screen: semaphore 0x70A7, fence 0x70B9, PUT == GET.
 *
 * docs/technical/d3d8ltcg-device-context.md bounds the wait for the same
 * reason. This does it from the outside so the generated body, and its
 * kickoff, stay untouched: once the GPU has been idle with the semaphore
 * unchanged for STALL_MS, everything submitted has executed, so every fence
 * D3D has written is in fact passed. The semaphore is advanced to D3D's write
 * sequence. A healthy run never trips it -- the semaphore moves every frame.
 */
#define FENCE_RESCUE_STALL_MS  250u

#include <windows.h>

static DWORD WINAPI fence_rescue_thread(LPVOID unused)
{
    uint32_t last_sem = 0, rescues = 0;
    DWORD still_since = GetTickCount();

    (void)unused;
    for (;;) {
        uint32_t dev = TUROK_MEM32(TUROK_D3D_DEVICE_PTR);
        uint32_t sem_va = dev ? TUROK_MEM32(dev + 0x34) : 0;
        uint32_t seq, sem;
        DWORD now = GetTickCount();

        Sleep(50);
        if (!sem_va)
            continue;
        seq = TUROK_MEM32(dev + 0x30);
        sem = TUROK_MEM32(sem_va);
        if (sem != last_sem || sem == seq ||
            TUROK_MEM32(NV2A_USER_DMA_PUT_VA) != TUROK_MEM32(NV2A_USER_DMA_GET_VA)) {
            last_sem = sem;
            still_since = now;
            continue;
        }
        if (now - still_since < FENCE_RESCUE_STALL_MS)
            continue;
        TUROK_MEM32(sem_va) = seq;
        last_sem = seq;
        still_since = now;
        if (++rescues <= 20 || rescues % 100 == 0) {
            fprintf(stderr, "[D3D] fence rescue #%u: GPU idle, semaphore stuck at "
                    "0x%08X, advanced to 0x%08X (releases lost to a desync)\n",
                    rescues, sem, seq);
            fflush(stderr);
        }
    }
}

extern void nv2a_pb_set_semaphore_target(uint32_t guest_va);
/* This exact form is what tools.recomp --exclude-manual looks for to emit the
 * original body as sub_00198650_gen instead of dropping it. */
extern void sub_00198650_gen(void);

void sub_00198650(void)
{
    static uint32_t s_sem;
    uint32_t dev = TUROK_MEM32(TUROK_D3D_DEVICE_PTR);
    uint32_t sem = dev ? TUROK_MEM32(dev + 0x34) : 0;

    if (sem && sem != s_sem) {
        if (!s_sem)
            CloseHandle(CreateThread(NULL, 0, fence_rescue_thread, NULL, 0, NULL));
        s_sem = sem;
        nv2a_pb_set_semaphore_target(sem);
        fprintf(stderr, "[D3D] GPU semaphore at 0x%08X (device 0x%08X)\n", sem, dev);
        fflush(stderr);
    }
    sub_00198650_gen();
}

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
recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
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
