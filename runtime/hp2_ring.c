/* HP2 bring-up diagnostic: ring buffer of the last indirect guest transfers (dispatch_indirect)
 * per process. HP2_RING=<n> enables it; hp2_ring_dump() prints the last n at guest exit. */
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>

#define RING 4096
static uint32_t g_to[RING], g_from[RING], g_tid[RING];
static unsigned g_pos; static int g_on = -1;

void hp2_ring_note(CPU *c, uint32_t addr){
    if (g_on < 0){ const char *e = getenv("HP2_RING"); g_on = e ? atoi(e) : 0; }
    if (!g_on) return;
    unsigned i = __atomic_fetch_add(&g_pos, 1u, __ATOMIC_RELAXED) % RING;
    g_to[i] = addr;
    g_from[i] = mem_is_committed(c->esp) ? MR32(c->esp) : 0;   /* return address of the indirect call */
    g_tid[i] = (c->fs_base && mem_is_committed(c->fs_base + 0x24u)) ? MR32(c->fs_base + 0x24u) : 0;
}

void hp2_ring_dump(void){
    if (g_on <= 0) return;
    unsigned n = (unsigned)g_on < RING ? (unsigned)g_on : RING, end = g_pos;
    fprintf(stderr, "\n=== last %u indirect transfers (to <- return address, thread) ===\n", n);
    for (unsigned k = (end > n ? end - n : 0); k < end; k++){
        unsigned i = k % RING;
        fprintf(stderr, "  %08x <- %08x  t%u\n", g_to[i], g_from[i], g_tid[i]);
    }
}
