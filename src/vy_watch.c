/* Who writes one guest float (diagnostic; issue #25's trampolines).
 *
 * A CPU data breakpoint on the calling thread: vy_watch_arm() raises an
 * exception whose handler loads debug register 0 with the host address of
 * the watched guest dword (write-only, 4 bytes), so every store to it traps
 * once, after the store, on the thread that armed it. The handler records
 * the host instruction (resolve with turok_evolution.map: the generated
 * sub_XXXXXXXX holding it) and the guest return address on top of the guest
 * stack, with the value written; vy_watch_report() prints what the last
 * period saw, by writer, and the run of values. Nothing is armed unless a
 * caller arms it. */
#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

extern ptrdiff_t g_xbox_mem_offset;
extern __declspec(thread) uint32_t g_esp;

#define VY_WATCH_ARM_CODE 0xE0A7C401u
#define VY_WRITERS 32
#define VY_VALUES 64

static PVOID s_handler;
static uintptr_t s_watch_host;
static DWORD s_thread;
static struct { uintptr_t rip; uint32_t guest_return; unsigned count; float last; } s_writers[VY_WRITERS];
static unsigned s_writer_count, s_lost;
static struct { unsigned writer; float value; } s_values[VY_VALUES];
static unsigned s_value_count;

static LONG CALLBACK vy_handler(PEXCEPTION_POINTERS ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    CONTEXT *c = ep->ContextRecord;
    if (code == VY_WATCH_ARM_CODE) {
        c->ContextFlags |= CONTEXT_DEBUG_REGISTERS;
        c->Dr0 = s_watch_host;
        /* L0, R/W0 = 01 (writes), LEN0 = 11 (4 bytes) */
        c->Dr7 = (c->Dr7 & ~0x000F0003ull) | (s_watch_host ? (1ull | (1ull << 16) | (3ull << 18)) : 0ull);
        c->Dr6 = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == EXCEPTION_SINGLE_STEP && (c->Dr6 & 1) && GetCurrentThreadId() == s_thread) {
        uintptr_t rip = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
        uint32_t guest_return = 0;
        float value;
        unsigned i;
        if (g_esp) memcpy(&guest_return, (const void *)((uintptr_t)g_esp + g_xbox_mem_offset), 4);
        memcpy(&value, (const void *)s_watch_host, 4);
        for (i = 0; i < s_writer_count; i++)
            if (s_writers[i].rip == rip && s_writers[i].guest_return == guest_return) break;
        if (i == s_writer_count) {
            if (s_writer_count < VY_WRITERS) {
                s_writers[i].rip = rip;
                s_writers[i].guest_return = guest_return;
                s_writers[i].count = 0;
                s_writer_count++;
            } else {
                s_lost++;
                i = VY_WRITERS;
            }
        }
        if (i < VY_WRITERS) {
            s_writers[i].count++;
            s_writers[i].last = value;
        }
        if (s_value_count < VY_VALUES) {
            s_values[s_value_count].writer = i;
            s_values[s_value_count].value = value;
            s_value_count++;
        }
        c->Dr6 = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* Watch guest address `guest` on this thread (0: stop). */
void vy_watch_arm(uint32_t guest)
{
    if (!s_handler) s_handler = AddVectoredExceptionHandler(1, vy_handler);
    s_thread = GetCurrentThreadId();
    s_watch_host = guest ? (uintptr_t)guest + (uintptr_t)g_xbox_mem_offset : 0;
    RaiseException(VY_WATCH_ARM_CODE, 0, 0, NULL);
}

/* Print and clear what was seen since the last report. */
void vy_watch_report(const char *label)
{
    unsigned i;
    HMODULE image = GetModuleHandleW(NULL);
    if (!s_writer_count && !s_value_count) return;
    fprintf(stderr, "[VYW] %s: %u writers (image %p)%s\n", label, s_writer_count, (void *)image,
            s_lost ? " and more" : "");
    for (i = 0; i < s_writer_count; i++)
        fprintf(stderr, "[VYW]   writer %u: host +%06llX, guest return %06X: %u writes, last %.3f\n", i,
                (unsigned long long)(s_writers[i].rip - (uintptr_t)image), s_writers[i].guest_return,
                s_writers[i].count, s_writers[i].last);
    fprintf(stderr, "[VYW]   first values:");
    for (i = 0; i < s_value_count; i++) fprintf(stderr, " %u:%.2f", s_values[i].writer, s_values[i].value);
    fprintf(stderr, "\n");
    s_writer_count = s_value_count = s_lost = 0;
}
