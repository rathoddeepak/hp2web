/* hp2wsfix (xan1242/hp2wsfix, MIT, (c) 2019 Lovro Plese) for the recompiled NFSHP2 v242.
 *
 * The DLL wrote resolution/aspect values into code and data, re-routed some calls and NOPed some
 * stores. tools/hp2_widescreen.py makes the lifted code read the patched immediates from guest
 * memory and call the hooks below at the same call sites; this file does what the DLL's
 * InitRenderCaps/InjectRes and hooks did, with its defaults (FixHUD=2, no letterbox/map options).
 *
 * Resolution: the game's own resolution becomes the canvas aspect (height 1080, or the canvas
 * height when smaller, at least 600), so com.c's device size keeps that aspect and its generic
 * stretch correction stays at 1.0. HP2_RES=WxH picks a size; HP2_WS=0 gives the original game.
 * [Graphics]/[GraphicsFE] Width/Height/LimitResolution answer from here instead of rendercaps.ini
 * (hp2wsfix rewrote the ini; the save is in the browser's storage here). */
#include "cpu.h"
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sub_0059b840(CPU *);
void sub_0059bae0(CPU *);
void sub_00527650(CPU *);
void sub_00527560(CPU *);
void dispatch_indirect(CPU *, uint32_t);
void metal_view_size(unsigned *, unsigned *);

/* HP2_242_Addresses.h */
#define FE_XPOS_ADDRESS             0x0084BC48u
#define FE_YPOS_ADDRESS             0x0084BC4Cu
#define FE_XSCALE_ADDRESS           0x006C2DA8u
#define FE_FUNCTIONS_XSCALE_ADDRESS 0x006C2DB0u
#define FE_FUNCTIONS_YSCALE_ADDRESS 0x006C2DB4u
#define LIMITRES_ADDRESS            0x0083EFACu
#define FOUR_BY_THREE_ASPECT   1.3333333333333333333333333333333
#define LETTERBOX_ASPECT       2.1052631578947368421052631578947
#define INTRO_LETTERBOX_ASPECT 2.9411764705882352941176470588235

int g_hp2ws_on = 1;

/* hp2wsfix globals (same names: the generated HUD table uses them) */
static volatile int resX = 1280, resY = 720;
static volatile float resX_43f = 960.0f, resX_600height = 1067.0f, resX_480height = 853.333334f;
static volatile float aspect = 1.7777778f, aspect_diff = 0.75f, xscale_800 = 1.33375f,
                      xscale_640 = (float)FOUR_BY_THREE_ASPECT;
static char *CurrentFEElement;
static char CurrentFEShape[255];
static bool bDisableLetterboxing = false, bClassicMapPosition = false, bAlternativeBackground = false;
static int FixHUD = 2;
static int OptionsMenuXsize = 650;
static int Tach_XOffset, Tach_YOffset, Map_XOffset, Map_YOffset, Time_XOffset, Time_YOffset,
           RVM_XOffset, RVM_YOffset, POS_XOffset, POS_YOffset, Cuffo_XOffset, Cuffo_YOffset,
           CopAmmo_XOffset, CopAmmo_YOffset, Score_XOffset, Score_YOffset, Dash_XOffset, Dash_YOffset;

typedef struct { int unk, topY, topX, botX, botY; } mBorders;

static uint32_t A(CPU *c, int i){ return MR32(c->esp + 4u * (uint32_t)i); }   /* at entry: 1 = first arg */
static const char *gstr(uint32_t p){ return (p && mem_is_committed(p)) ? (const char *)(g_mem + p) : ""; }

static int env_on(const char *n, int def){ const char *e = getenv(n); return e && *e ? atoi(e) : def; }

static void write_f(uint32_t a, float v){ MRF32(a) = v; }
static void write_i(uint32_t a, int v){ MR32(a) = (uint32_t)v; }

static float RecalculateFOV_4by3(float InFOV){
    float hfovRad = (float)(InFOV * M_PI / 180.0);
    float vfovRad = 2.0f * (float)atan(tan(hfovRad * 0.5) / (4.0 / 3.0));
    float newfovRad = 2.0f * (float)atan(aspect * tan(vfovRad * 0.5f));
    return (float)((newfovRad * 180.0) / M_PI);
}

static void pick_res(void){
    const char *e = getenv("HP2_RES");
    unsigned w = 0, h = 0;
    if (e && sscanf(e, "%ux%u", &w, &h) == 2 && w >= 640 && h >= 480 && w <= 8192 && h <= 8192){
        resX = (int)w; resY = (int)h; return;
    }
    unsigned vw = 0, vh = 0;
    metal_view_size(&vw, &vh);
    if (!vw || !vh){ vw = 1280; vh = 720; }
    unsigned hh = vh < 1080u ? vh : 1080u;
    if (hh < 600u) hh = 600u;
    resY = (int)hh;
    resX = (int)((unsigned)((double)hh * vw / vh + 0.5) & ~1u);
}

/* InitRenderCaps (the part after reading the resolution) */
static void init_rendercaps(void){
    resX_43f = (float)resY * (4.0f / 3.0f);
    float FE_horscale = resX_43f / 800.0f;
    write_f(FE_XSCALE_ADDRESS, FE_horscale);
    write_f(FE_FUNCTIONS_YSCALE_ADDRESS, (float)(1 / (resY / 600.0)));
    write_f(FE_FUNCTIONS_XSCALE_ADDRESS, MRF32(FE_FUNCTIONS_YSCALE_ADDRESS));
    aspect = (float)resX / (float)resY;
    aspect_diff = (float)(FOUR_BY_THREE_ASPECT / aspect);
    resX_600height = aspect * 600;
    resX_480height = aspect * 480;
    xscale_800 = resX_600height / 800;
    xscale_640 = resX_480height / 640;
}

/* InjectRes: the same writes as injector::WriteMemory (code immediates are read from these bytes) */
static void inject_res(void){
    write_i(LIMITRES_ADDRESS, 0);
    static const uint32_t asp[] = {0x40C501u, 0x53EB84u, 0x53ED7Cu, 0x65D900u};
    for (unsigned i = 0; i < 4; i++) write_f(asp[i], aspect);
    static const uint32_t rx[] = {0x541610u, 0x541489u, 0x5414A5u, 0x5415F4u, 0x4D6A39u, 0x5414DDu, 0x541899u};
    static const uint32_t ry[] = {0x541617u, 0x541484u, 0x5414A0u, 0x5415EFu, 0x4D6A42u, 0x5414E4u, 0x5418A1u};
    for (unsigned i = 0; i < 7; i++){ write_i(rx[i], resX); write_i(ry[i], resY); }
    static const uint32_t fx[] = {0x69D004u, 0x69D014u, 0x6639F0u};
    static const uint32_t fy[] = {0x69D008u, 0x69D018u, 0x6639F4u};
    for (unsigned i = 0; i < 3; i++){ write_f(fx[i], (float)resX); write_f(fy[i], (float)resY); }
    static const uint32_t ys[] = {0x445BE3u, 0x44D59Du, 0x49C3F3u};
    for (unsigned i = 0; i < 3; i++) write_f(ys[i], aspect_diff);
    write_f(0x45A8ECu, (float)resX); write_f(0x45A8F3u, (float)resY);
}

void hp2ws_init(void){
    g_hp2ws_on = env_on("HP2_WS", 1);
    if (!g_hp2ws_on){ fprintf(stderr, "hp2ws: off (HP2_WS=0), original 4:3 game\n"); return; }
    /* the operand bytes must still be the v242 originals */
    if (MR32(0x541489u) != 0x320u || MR32(0x541484u) != 0x258u || MR32(0x40C501u) != 0x3faaaaabu){
        fprintf(stderr, "hp2ws: unexpected code bytes, widescreen fix disabled\n");
        g_hp2ws_on = 0; return;
    }
    pick_res();
    init_rendercaps();
    inject_res();
    fprintf(stderr, "hp2ws: game resolution %dx%d (aspect %.4f, FE x scale %.4f)\n",
            resX, resY, (double)aspect, (double)xscale_800);
}

/* the extra display mode the D3D8 layer reports (1 if on) */
int hp2ws_mode(uint32_t *w, uint32_t *h){
    if (!g_hp2ws_on) return 0;
    if (w) *w = (uint32_t)resX;
    if (h) *h = (uint32_t)resY;
    return 1;
}

/* ---- call-site hooks (same calling conventions as the callees they replace) ---- */

/* rendercaps.ini reader's ReadIni_Int(section, key, int *value), thiscall, ret 0xC */
void hp2ws_readini_int(CPU *c){
    uint32_t sec = A(c, 1), key = A(c, 2), p = A(c, 3);
    sub_00527560(c);
    if (!g_hp2ws_on || !p) return;
    const char *s = gstr(sec), *k = gstr(key);
    if (strcmp(s, "Graphics") && strcmp(s, "GraphicsFE")) return;
    if (!strcmp(k, "Width")) MR32(p) = (uint32_t)resX;
    else if (!strcmp(k, "Height")) MR32(p) = (uint32_t)resY;
    else if (!strcmp(k, "LimitResolution")) MR32(p) = 0;
}

/* ReadIni_Float(section, key, float *value) at the camera FOV reads, thiscall, ret 0xC */
void hp2ws_readini_float(CPU *c){
    uint32_t p = A(c, 3);
    sub_00527650(c);
    if (g_hp2ws_on && p) MRF32(p) = RecalculateFOV_4by3(MRF32(p));
}

/* sub_59B840(key, mBorders *, unk), thiscall, ret 0xC */
void hp2ws_59b840_hook(CPU *c){
    uint32_t mb = A(c, 2);
    sub_0059b840(c);
    if (g_hp2ws_on && FixHUD > 1 && mb) ((mBorders *)(g_mem + mb))->botX = (int)resX_600height;
}

static const char *hp2ws_strrchr(const char *s, int ch){ const char *r = strrchr(s, ch); return r ? r : ""; }

void hp2ws_59b840_hook2(CPU *c){
    uint32_t mb = A(c, 2), el = MR32(c->ecx + 8u);
    sub_0059b840(c);
    if (!g_hp2ws_on || !mb || !el || !mem_is_committed(el)) return;
    CurrentFEElement = (char *)(g_mem + el);
    mBorders *unk1 = (mBorders *)(g_mem + mb);
#define strrchr hp2ws_strrchr
#include "hp2ws_hud.inc"
#undef strrchr
}

/* sub_59BAE0(a2, a3), thiscall, ret 8: remember the current shape name for the table above */
void hp2ws_59bae0_hook(CPU *c){
    if (g_hp2ws_on){
        uint32_t th = c->ecx, a2 = A(c, 1);
        uint32_t vf = MR32(MR32(th) + 20u);
        CPU save = *c;
        PUSH32(a2); PUSH32(0xDEADBEEFu);
        dispatch_indirect(c, vf);
        uint32_t v3 = c->eax;
        *c = save;
        uint32_t sp = v3 ? MR32(v3 + 4u) : 0;
        if (sp && mem_is_committed(sp)){
            snprintf(CurrentFEShape, sizeof CurrentFEShape, "%s", (const char *)(g_mem + sp));
            if (bAlternativeBackground && !strcmp(CurrentFEShape, "[PC:S_INTRO.LYR.PCD] back"))
                strcpy((char *)(g_mem + sp), "[PC:S_FEAT.LYR.PCD] feat");
        }
    }
    sub_0059bae0(c);
}

/* sub_5954A0(out, in) rewritten (cdecl): FE rectangle to screen without the x position term */
void sub_005954a0(CPU *);
void hp2ws_5954a0(CPU *c){
    if (!g_hp2ws_on){ sub_005954a0(c); return; }
    uint32_t a1 = A(c, 1), a2 = A(c, 2);
    float ys = MRF32(FE_FUNCTIONS_YSCALE_ADDRESS), xs = MRF32(FE_FUNCTIONS_XSCALE_ADDRESS),
          yp = MRF32(FE_YPOS_ADDRESS);
    float v8  = (float)((double)((int32_t)MR32(a2 + 4) + (int32_t)MR32(a2 + 16)) * ys - yp);
    float v10 = (float)((double)(int32_t)MR32(a2 + 4) * ys - yp);
    float v9  = (float)((double)((int32_t)MR32(a2 + 8) + (int32_t)MR32(a2 + 12)) * xs);
    float v11 = (float)((double)(int32_t)MR32(a2 + 8) * xs);
    MR32(a1 + 8)  = (uint32_t)(int32_t)v11;
    MR32(a1 + 12) = (uint32_t)(int32_t)(v9 - v11);
    MR32(a1)      = 0x65E010u;
    MR32(a1 + 4)  = (uint32_t)(int32_t)v10;
    MR32(a1 + 16) = (uint32_t)(int32_t)(v8 - v10);
    c->eax = a1;
    POP32(c->eip);
}

/* FE_CursorPos(out, in) (cdecl), replaces the call to sub_595550 */
void sub_00595550(CPU *);
void hp2ws_cursorpos(CPU *c){
    if (!g_hp2ws_on){ sub_00595550(c); return; }
    uint32_t out = A(c, 1), in = A(c, 2);
    float v6 = (int32_t)MR32(in + 4) * MRF32(FE_FUNCTIONS_XSCALE_ADDRESS)
             - (MRF32(FE_XPOS_ADDRESS) / MRF32(FE_XSCALE_ADDRESS));
    float v5 = (int32_t)MR32(in + 8) * MRF32(FE_FUNCTIONS_YSCALE_ADDRESS) - MRF32(FE_YPOS_ADDRESS);
    MR32(out)     = 0x65E1F8u;
    MR32(out + 4) = (uint32_t)(int32_t)v6;
    MR32(out + 8) = (uint32_t)(int32_t)v5;
    c->eax = out;
    POP32(c->eip);
}
