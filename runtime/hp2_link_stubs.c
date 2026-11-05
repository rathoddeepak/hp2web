/* HP2: neutral definitions for NFSU2 feature modules that are NOT linked into the HP2 runtime
 * (widescreen.c, framerate.c, enhance.c, headlights.c, online.c, roam_game.c, overrides.c
 * reports, and the server.dll second module). Each one answers "feature off / unchanged".
 * HP2's own widescreen comes later from hp2wsfix (step 6). */
#include <stdint.h>
#include "module.h"

/* widescreen.c: identity mapping */
int    ws_fill_target(void){ return 0; }
double ws_max_pixels(void){ return 9.0e6; }       /* WS_MAX_PIXELS_DEFAULT */
float  ws_persp_x(void){ return 1.f; }
float  ws_persp_y(void){ return 1.f; }
double ws_render_factor(void){ return 1.0; }
void   ws_report(void){}
void   ws_set_mapping(float k){ (void)k; }
float  ws_ui_x(void){ return 1.f; }
float  ws_ui_y(void){ return 1.f; }

/* framerate.c (high frame rate): off */
int      hfr_display_live(void){ return 0; }
int      hfr_mailbox(void){ return 0; }
int      hfr_mode_off(void){ return 1; }
void     hfr_note_vsync(double ts, double dur){ (void)ts; (void)dur; }
uint64_t hfr_vclock_floor_us(void){ return 0; }

/* enhance.c: leave render targets alone (ENH_NONE) */
void enh_report(void){}
int  enh_rt_size(uint32_t ret, uint32_t bb_w, uint32_t bb_h, uint32_t *w, uint32_t *h){
    (void)ret; (void)bb_w; (void)bb_h; (void)w; (void)h; return 0;
}

/* headlights.c, online.c, roam_game.c, overrides.c */
void headlights_report(void){}
void headlights_vs_ramp(float s, float o){ (void)s; (void)o; }
void online_lobby_read(uint32_t link, const uint8_t *p, int n){ (void)link; (void)p; (void)n; }
void roam_game_parked(int on){ (void)on; }
void overrides_report(void){}
uint32_t g_fe_mgr = 0;

/* server.dll second module: not present in HP2 (same as nfsu1web's empty gen/server) */
typedef struct { uint32_t iat; const char *dll; const char *name; int nargs; } ImpEnt;
const uint32_t g_srv_fn_count = 0;
const uint32_t g_srv_fn_addr[1] = {0};
const mod_fn   g_srv_fn_ptr[1] = {0};
const uint32_t g_srv_imp_count = 0;
const ImpEnt   g_srv_imp[1] = {{0}};
const ModInfo  g_srv_modinfo = {""};

/* autotest.c script commands for NFSU2 career/roam/headlights/framerate: not available in HP2 */
struct CPU;
uint32_t autopilot_vehicle(void){ return 0; }
int    career_cmd(struct CPU *c, const char *line, long *wait_start){ (void)c; (void)line; (void)wait_start; return 0; }
int    career_is_cmd(const char *cmd){ (void)cmd; return 0; }
int    roam_cmd(struct CPU *c, const char *line, long *wait_start){ (void)c; (void)line; (void)wait_start; return 0; }
int    headlights_set(const char *mode){ (void)mode; return 0; }
double hfr_race_start_ms(void){ return -1; }
void   hfr_set_display_hz(int hz){ (void)hz; }
double hfr_sim_ms(void){ return 0; }
