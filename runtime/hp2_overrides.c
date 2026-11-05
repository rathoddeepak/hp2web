/* HP2 runtime overrides: guest-callable host functions, keyed by NFSHP2.exe v242 VA.
 * Replaces nfsu2-web runtime/overrides.c (NFSU2 addresses).
 *
 * Win2kHook.dll (shipped with the game): a low-level keyboard hook that blocks the Windows key.
 * The game LoadLibrary()s it and GetProcAddress()es three exports; without them init fails with
 * error 7. In the browser there is nothing to block, so the exports are host functions placed at
 * unused int3 padding in .text (never executed by the game itself). */
#include "cpu.h"
#include <stdio.h>
#include <string.h>

void dispatch_patch(uint32_t a, void (*f)(CPU *));

#define VA_LLKBPROC  0x0062c3e0u   /* _LowLevelKeyboardProc@12 (stdcall, 3 args) */
#define VA_SETHOOK   0x0062c3e4u   /* SetHookHandle(HHOOK)     (cdecl)           */
#define VA_GETVALUES 0x0062c3e8u   /* GetValues()              (cdecl)           */

static void ret_pop(CPU *c, uint32_t eax, uint32_t nargs){
    uint32_t ret; POP32(ret);
    c->esp += 4u * nargs;
    c->eax = eax;
    c->eip = ret;
}
static void llkbproc(CPU *c){ ret_pop(c, 0, 3); }      /* CallNextHookEx semantics: pass the key on */
static void sethook(CPU *c){ ret_pop(c, 0, 0); }
static void getvalues(CPU *c){ ret_pop(c, 0, 0); }

/* win32.c GetProcAddress hook: names served here before the IAT search */
uint32_t hp2_getproc(const char *name){
    if (!strcmp(name, "_LowLevelKeyboardProc@12")) return VA_LLKBPROC;
    if (!strcmp(name, "SetHookHandle")) return VA_SETHOOK;
    if (!strcmp(name, "GetValues")) return VA_GETVALUES;
    return 0;
}

void overrides_install(void){
    dispatch_patch(VA_LLKBPROC, llkbproc);
    dispatch_patch(VA_SETHOOK, sethook);
    dispatch_patch(VA_GETVALUES, getvalues);
    fprintf(stderr, "hp2: overrides: Win2kHook.dll exports (3)\n");
    { extern void hp2ws_init(void); hp2ws_init(); }       /* hp2wsfix (runtime/hp2_widescreen.c) */
}

/* EA's task pump (sub_005d8a00) is what the game's polling loops call while they wait for the
 * async file threads (e.g. sub_0054acf0 waiting on a read). On Windows the OS preempts; here guest
 * threads only switch at system calls, so the poller would starve the file threads (livelock).
 * Link-time --wrap (tools/step3_build.sh HP2_FIXED_WRAPS) gives the other threads a turn first. */
void guest_fair_yield(void);
void __real_sub_005d8a00(CPU *c);
void __wrap_sub_005d8a00(CPU *c){ guest_fair_yield(); __real_sub_005d8a00(c); }
