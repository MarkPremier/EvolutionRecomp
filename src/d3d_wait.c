/* D3D BlockOnTime's lost wake-up (the freeze with every thread waiting).
 *
 * BlockOnTime (sub_00198650, stdcall: time, flags) waits until the GPU's time
 * (the semaphore *(device+0x34)) reaches `time`. Rather than spin, it patches
 * a NOP 5 software method into the fence slot of that time in the
 * pushbuffer and sleeps on the event at device+0x2564, which the software
 * method sets. It trusts the slot only while the GPU's read position (DMA
 * GET) is at least 8 KB short of it. The runtime's executor publishes GET
 * once a whole submission is walked, so GET can lag far behind what the
 * executor has already read: the patch then lands in a slot it has passed,
 * the method never runs, and the wait -- which does not look at the GPU time
 * again -- sleeps for ever while the GPU sits idle past the fence.
 *
 * A watcher sets the event when a wait has lasted over 5 ms and the GPU's
 * time was past the one waited for on two looks running, the very condition
 * the method reports: the lost wake-up costs a few milliseconds instead of the
 * game. It can only release the call it checked (s_lock, held around the
 * check and the release as BlockOnTime returns), and a set it leaves behind
 * is harmless, as BlockOnTime clears the event (+4, SignalState) before it
 * patches the slot. RECOMP_BLOCK_WATCH=0 turns it off. */
#include "platform/xbox_winnt.h"
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

extern ptrdiff_t g_xbox_mem_offset;
extern int xbox_KeObjectSignal(uint32_t guest_va);
extern int xbox_Nv2aSoftwareMethodPending(void);

#define D3D_DEVICE_POINTER 0x001A1AD0u

static uint32_t mem32(uint32_t va)
{
    return *(volatile uint32_t *)((uintptr_t)va + g_xbox_mem_offset);
}

static CRITICAL_SECTION s_lock;
static HANDLE s_started;
static volatile LONG s_active;      /* the call being waited in, 0 for none */
static volatile LONG s_call;
static uint32_t s_time;
static DWORD s_since;
static int s_enabled = -1;
static DWORD s_after = 5;       /* ms; RECOMP_BLOCK_WATCH_MS */

/* The GPU's time has reached `time` (BlockOnTime's own test). */
static int reached(uint32_t time)
{
    uint32_t device = mem32(D3D_DEVICE_POINTER), cpu, gpu;
    if (!device) return 0;
    cpu = mem32(device + 0x30u);
    gpu = mem32(mem32(device + 0x34u));
    return cpu - time >= cpu - gpu;
}

static DWORD WINAPI watch(LPVOID unused)
{
    static unsigned released;
    LONG seen = 0;          /* the call the GPU was already past at the last look */
    (void)unused;
    for (;;) {
        WaitForSingleObject(s_started, INFINITE);
        while (s_active) {
            Sleep(2);
            EnterCriticalSection(&s_lock);
            if (s_active && GetTickCount() - s_since > s_after && reached(s_time)) {
                /* Past the time on two looks running: a wait in BlockOnTime's
                 * spin loop, or one just woken, returns between them. */
                if (seen == s_active) {
                    uint32_t device = mem32(D3D_DEVICE_POINTER);
                    if (released++ < 50u)
                        fprintf(stderr, "[D3D] BlockOnTime: the GPU passed time %08X (now %08X) without waking the wait; released it after %lu ms (event state %u, software method %s)\n",
                                s_time, mem32(mem32(device + 0x34u)), (unsigned long)(GetTickCount() - s_since),
                                mem32(device + 0x2568u), xbox_Nv2aSoftwareMethodPending() ? "pending" : "idle");
                    xbox_KeObjectSignal(device + 0x2564u);
                    s_since = GetTickCount();
                    seen = 0;
                } else {
                    seen = s_active;
                }
            } else {
                seen = 0;
            }
            LeaveCriticalSection(&s_lock);
        }
    }
}

/* Starts watching a BlockOnTime call for `time`; `outer` takes the call it
 * is nested in (BlockOnTime can come back through D3D's make-space). Returns
 * 0 when there is nothing to watch. */
int d3d_wait_begin(uint32_t time, uint32_t outer[3])
{
    if (s_enabled < 0) {
        const char *value = getenv("RECOMP_BLOCK_WATCH");
        s_enabled = !(value && *value == '0');
        value = getenv("RECOMP_BLOCK_WATCH_MS");
        if (value && atoi(value) > 0) s_after = (DWORD)atoi(value);
        if (s_enabled) {
            HANDLE thread;
            InitializeCriticalSection(&s_lock);
            s_started = CreateEventA(NULL, FALSE, FALSE, NULL);
            thread = s_started ? CreateThread(NULL, 0, watch, NULL, 0, NULL) : NULL;
            if (thread) CloseHandle(thread);
            else s_enabled = 0;
        }
    }
    if (!s_enabled || reached(time)) return 0;
    EnterCriticalSection(&s_lock);
    outer[0] = (uint32_t)s_active;
    outer[1] = s_time;
    outer[2] = s_since;
    s_time = time;
    s_since = GetTickCount();
    s_active = InterlockedIncrement(&s_call);
    LeaveCriticalSection(&s_lock);
    SetEvent(s_started);
    return 1;
}

#ifndef _WIN32
#include <time.h>
/* Before BlockOnTime's own spin: while the toolkit's frame cap holds a flip
 * the GPU's time cannot move, so sleep until the hold ends rather than spin
 * through it (~70% of a core at the 60 fps cap in the Chapter 1 jungle).
 * Once no hold is in progress, BlockOnTime runs as it always did.
 * RECOMP_BLOCK_SLEEP=0 turns it off. */
void d3d_wait_paced(uint32_t time)
{
    extern long long xbox_Nv2aFlipPaceUntil(void);
    static int enabled = -1;
    long long until;
    if (enabled < 0) {
        const char *value = getenv("RECOMP_BLOCK_SLEEP");
        enabled = !(value && *value == '0');
    }
    while (enabled && !reached(time) && (until = xbox_Nv2aFlipPaceUntil()) != 0) {
        struct timespec wake = {(time_t)(until / 1000000000LL), (long)(until % 1000000000LL)};
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &wake, NULL);
    }
}
#endif

void d3d_wait_end(const uint32_t outer[3])
{
    EnterCriticalSection(&s_lock);
    s_active = (LONG)outer[0];
    s_time = outer[1];
    s_since = outer[2];
    LeaveCriticalSection(&s_lock);
}
