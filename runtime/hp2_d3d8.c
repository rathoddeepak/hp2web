/* HP2: Direct3D 8 on top of the nfsu2-web Direct3D 9 COM bridge (com.c).
 *
 * Guest-visible D3D8 objects ARE the com.c D3D9 objects; only their vtable pointer ([obj+0]) is
 * swapped for a D3D8 vtable whose slots hold 0xE0000000 | ((0x100 + k) << 16) | slot.
 * com_call() forwards iface >= 0x100 here (patched in by tools/apply_hp2_runtime.sh), and
 * com_valid() accepts the D3D8 vtables too.
 *
 * Each D3D8 call is translated into one or more D3D9 calls: the D3D9 argument frame is pushed
 * on the guest stack with a dummy return address and com_call(c, d3d9 magic) runs it (it pops
 * the return address and its own arguments). Then the D3D8 frame is popped and the guest
 * returns to its own caller.
 *
 * Layout references: d3d8.h / d3d8types.h / d3d8caps.h (DirectX 8.1 SDK).
 */
#include "cpu.h"
int hp2ws_mode(uint32_t *w, uint32_t *h);   /* runtime/hp2_widescreen.c */
#include "d3d9_tables.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int      com_call(CPU *c, uint32_t magic);
uint32_t com_scratch(uint32_t n);
uint32_t com_direct3dcreate9(void);
uint32_t com_obj_release(CPU *c, uint32_t o);
uint32_t com_obj_addref(CPU *c, uint32_t o);

/* com.c interface ids (enum in com.c) */
enum { IF_D3D9 = 1, IF_DEV, IF_TEX, IF_VB, IF_IB, IF_SURF, IF_SIMPLE, IF_VSHADER, IF_VDECL,
       IF_STATEBLOCK, IF_SWAPCHAIN, IF_QUERY };
#define CF_CUBE 1u

/* ---- D3D8 interfaces ---------------------------------------------------------------------- */
enum { K_D3D, K_DEV, K_TEX, K_CUBE, K_VB, K_IB, K_SURF, K_SWAP, K_N };
#define K_BASE 0x100u

typedef struct { const char *name; unsigned char nargs; } M8;

static const M8 D3D8_[] = {
 {"QueryInterface",3},{"AddRef",1},{"Release",1},{"RegisterSoftwareDevice",2},{"GetAdapterCount",1},
 {"GetAdapterIdentifier",4},{"GetAdapterModeCount",2},{"EnumAdapterModes",4},{"GetAdapterDisplayMode",3},
 {"CheckDeviceType",6},{"CheckDeviceFormat",7},{"CheckDeviceMultiSampleType",6},{"CheckDepthStencilMatch",6},
 {"GetDeviceCaps",4},{"GetAdapterMonitor",2},{"CreateDevice",7},
};
static const M8 DEV8_[] = {
 {"QueryInterface",3},{"AddRef",1},{"Release",1},{"TestCooperativeLevel",1},{"GetAvailableTextureMem",1},
 {"ResourceManagerDiscardBytes",2},{"GetDirect3D",2},{"GetDeviceCaps",2},{"GetDisplayMode",2},
 {"GetCreationParameters",2},{"SetCursorProperties",4},{"SetCursorPosition",4},{"ShowCursor",2},
 {"CreateAdditionalSwapChain",3},{"Reset",2},{"Present",5},{"GetBackBuffer",4},{"GetRasterStatus",2},
 {"SetGammaRamp",3},{"GetGammaRamp",2},{"CreateTexture",8},{"CreateVolumeTexture",9},{"CreateCubeTexture",7},
 {"CreateVertexBuffer",6},{"CreateIndexBuffer",6},{"CreateRenderTarget",7},{"CreateDepthStencilSurface",6},
 {"CreateImageSurface",5},{"CopyRects",6},{"UpdateTexture",3},{"GetFrontBuffer",2},{"SetRenderTarget",3},
 {"GetRenderTarget",2},{"GetDepthStencilSurface",2},{"BeginScene",1},{"EndScene",1},{"Clear",7},
 {"SetTransform",3},{"GetTransform",3},{"MultiplyTransform",3},{"SetViewport",2},{"GetViewport",2},
 {"SetMaterial",2},{"GetMaterial",2},{"SetLight",3},{"GetLight",3},{"LightEnable",3},{"GetLightEnable",3},
 {"SetClipPlane",3},{"GetClipPlane",3},{"SetRenderState",3},{"GetRenderState",3},{"BeginStateBlock",1},
 {"EndStateBlock",2},{"ApplyStateBlock",2},{"CaptureStateBlock",2},{"DeleteStateBlock",2},
 {"CreateStateBlock",3},{"SetClipStatus",2},{"GetClipStatus",2},{"GetTexture",3},{"SetTexture",3},
 {"GetTextureStageState",4},{"SetTextureStageState",4},{"ValidateDevice",2},{"GetInfo",4},
 {"SetPaletteEntries",3},{"GetPaletteEntries",3},{"SetCurrentTexturePalette",2},{"GetCurrentTexturePalette",2},
 {"DrawPrimitive",4},{"DrawIndexedPrimitive",6},{"DrawPrimitiveUP",5},{"DrawIndexedPrimitiveUP",9},
 {"ProcessVertices",6},{"CreateVertexShader",5},{"SetVertexShader",2},{"GetVertexShader",2},
 {"DeleteVertexShader",2},{"SetVertexShaderConstant",4},{"GetVertexShaderConstant",4},
 {"GetVertexShaderDeclaration",4},{"GetVertexShaderFunction",4},{"SetStreamSource",4},{"GetStreamSource",4},
 {"SetIndices",3},{"GetIndices",3},{"CreatePixelShader",3},{"SetPixelShader",2},{"GetPixelShader",2},
 {"DeletePixelShader",2},{"SetPixelShaderConstant",4},{"GetPixelShaderConstant",4},
 {"GetPixelShaderFunction",4},{"DrawRectPatch",4},{"DrawTriPatch",4},{"DeletePatch",2},
};
#define RES8 {"QueryInterface",3},{"AddRef",1},{"Release",1},{"GetDevice",2},{"SetPrivateData",5},\
 {"GetPrivateData",4},{"FreePrivateData",2},{"SetPriority",2},{"GetPriority",1},{"PreLoad",1},{"GetType",1}
static const M8 TEX8_[] = { RES8, {"SetLOD",2},{"GetLOD",1},{"GetLevelCount",1},
 {"GetLevelDesc",3},{"GetSurfaceLevel",3},{"LockRect",5},{"UnlockRect",2},{"AddDirtyRect",2} };
static const M8 CUBE8_[] = { RES8, {"SetLOD",2},{"GetLOD",1},{"GetLevelCount",1},
 {"GetLevelDesc",3},{"GetCubeMapSurface",4},{"LockRect",6},{"UnlockRect",3},{"AddDirtyRect",3} };
static const M8 VB8_[] = { RES8, {"Lock",5},{"Unlock",1},{"GetDesc",2} };
static const M8 IB8_[] = { RES8, {"Lock",5},{"Unlock",1},{"GetDesc",2} };
static const M8 SURF8_[] = { {"QueryInterface",3},{"AddRef",1},{"Release",1},{"GetDevice",2},
 {"SetPrivateData",5},{"GetPrivateData",4},{"FreePrivateData",2},{"GetContainer",3},{"GetDesc",2},
 {"LockRect",4},{"UnlockRect",1} };
static const M8 SWAP8_[] = { {"QueryInterface",3},{"AddRef",1},{"Release",1},{"Present",5},{"GetBackBuffer",4} };

#define NEL(a) ((unsigned)(sizeof(a) / sizeof(*(a))))
static const struct { const char *name; const M8 *m; unsigned n; int if9; } K8[K_N] = {
    {"IDirect3D8",              D3D8_, NEL(D3D8_), IF_D3D9},
    {"IDirect3DDevice8",        DEV8_, NEL(DEV8_), IF_DEV},
    {"IDirect3DTexture8",       TEX8_, NEL(TEX8_), IF_TEX},
    {"IDirect3DCubeTexture8",   CUBE8_, NEL(CUBE8_), IF_TEX},
    {"IDirect3DVertexBuffer8",  VB8_, NEL(VB8_), IF_VB},
    {"IDirect3DIndexBuffer8",   IB8_, NEL(IB8_), IF_IB},
    {"IDirect3DSurface8",       SURF8_, NEL(SURF8_), IF_SURF},
    {"IDirect3DSwapChain8",     SWAP8_, NEL(SWAP8_), IF_SWAPCHAIN},
};
static uint32_t g_vt8[K_N];

static void make_vtables(void){
    if (g_vt8[0]) return;
    for (int k = 0; k < K_N; k++){
        uint32_t vt = com_scratch(K8[k].n * 4u);
        for (unsigned i = 0; i < K8[k].n; i++) MR32(vt + 4u * i) = 0xE0000000u | ((K_BASE + (uint32_t)k) << 16) | i;
        g_vt8[k] = vt;
    }
}

/* com_valid() hook: is vt one of our D3D8 vtables for com.c interface if9? */
int hp2_d3d8_is_vtab(uint32_t if9, uint32_t vt){
    for (int k = 0; k < K_N; k++) if (g_vt8[k] == vt && (uint32_t)K8[k].if9 == if9) return 1;
    return 0;
}

/* Give a com.c object (D3D9 vtable) its D3D8 vtable before the guest sees it. */
static uint32_t wrap(uint32_t o){
    if (!o || !mem_is_committed(o - 16u) || !mem_is_committed(o + 3u)) return o;
    uint32_t if9 = MR32(o - 4u) & 0xFFFFu, flags = MR32(o - 12u);
    int k = -1;
    switch (if9){
    case IF_D3D9: k = K_D3D; break;
    case IF_DEV: k = K_DEV; break;
    case IF_TEX: k = (flags & CF_CUBE) ? K_CUBE : K_TEX; break;
    case IF_VB: k = K_VB; break;
    case IF_IB: k = K_IB; break;
    case IF_SURF: k = K_SURF; break;
    case IF_SWAPCHAIN: k = K_SWAP; break;
    }
    if (k >= 0) MR32(o) = g_vt8[k];
    return o;
}
static void wrap_out(uint32_t pp){ if (pp && mem_is_committed(pp) && MR32(pp)) wrap(MR32(pp)); }

/* ---- calling into com.c ------------------------------------------------------------------ */
static const Method *tab9(int if9, unsigned *n){
    switch (if9){
    case IF_D3D9: *n = NEL(M_D3D9); return M_D3D9;
    case IF_DEV: *n = NEL(M_DEVICE); return M_DEVICE;
    case IF_TEX: *n = NEL(M_TEXTURE); return M_TEXTURE;
    case IF_VB: *n = NEL(M_VBUF); return M_VBUF;
    case IF_IB: *n = NEL(M_IBUF); return M_IBUF;
    case IF_SURF: *n = NEL(M_SURFACE); return M_SURFACE;
    case IF_STATEBLOCK: *n = NEL(M_STATEBLOCK); return M_STATEBLOCK;
    case IF_SWAPCHAIN: *n = NEL(M_SWAPCHAIN); return M_SWAPCHAIN;
    }
    *n = 0; return 0;
}
static int slot9(int if9, const char *name){
    unsigned n; const Method *m = tab9(if9, &n);
    for (unsigned i = 0; i < n; i++) if (!strcmp(m[i].name, name)) return (int)i;
    return -1;
}

/* Run D3D9 method if9::name with the given arguments (args[0] = this). Returns eax. */
static uint32_t call9(CPU *c, int if9, const char *name, const uint32_t *args, int n){
    int s = slot9(if9, name);
    if (s < 0){ fprintf(stderr, "hp2_d3d8: no D3D9 method %d::%s\n", if9, name); return 0x80004001u; }
    extern uint64_t g_com_unimpl;
    uint64_t un0 = g_com_unimpl;
    uint32_t eip = c->eip, esp0 = c->esp;
    for (int i = n - 1; i >= 0; i--) PUSH32(args[i]);
    PUSH32(0xD3D80000u);                         /* dummy return address, popped by com_call */
    com_call(c, 0xE0000000u | ((uint32_t)if9 << 16) | (uint32_t)s);
    if (c->esp != esp0){                         /* com.c popped a different argument count */
        static int warned;
        if (warned++ < 20)
            fprintf(stderr, "hp2_d3d8: STACK MISMATCH after D3D9 %d::%s: pushed %d args, esp off by %d bytes (fixed)\n",
                    if9, name, n, (int)(c->esp - esp0));
        c->esp = esp0;
    }
    if (g_com_unimpl != un0){
        static const char *seen[64]; static int ns;
        int k2; for (k2 = 0; k2 < ns && seen[k2] != name; k2++){}
        if (k2 == ns && ns < 64){ seen[ns++] = name;
            fprintf(stderr, "hp2_d3d8: D3D9 %d::%s is NOT implemented in com.c (returned 0x%x)\n", if9, name, c->eax); }
    }
    c->eip = eip;
    return c->eax;
}
#define C9(if9, name, ...) ({ uint32_t _a[] = { __VA_ARGS__ }; call9(c, (if9), (name), _a, (int)NEL(_a)); })

/* per-call scratch in guest memory (COM calls run under the guest lock) */
static uint32_t scratch(uint32_t n){
    static uint32_t base, cap;
    if (cap < n){ base = com_scratch(n < 4096u ? 4096u : n); cap = n < 4096u ? 4096u : n; }
    for (uint32_t i = 0; i < n; i += 4) MR32(base + i) = 0;
    return base;
}

/* ---- format sizes (D3DSURFACE_DESC8.Size) ------------------------------------------------- */
static uint32_t fmt_size(uint32_t fmt, uint32_t w, uint32_t h){
    switch (fmt){
    case 0x31545844u: return ((w + 3) / 4) * ((h + 3) / 4) * 8u;            /* DXT1 */
    case 0x32545844u: case 0x33545844u: case 0x34545844u: case 0x35545844u:
        return ((w + 3) / 4) * ((h + 3) / 4) * 16u;                          /* DXT2-5 */
    case 20: return w * h * 3u;                                               /* R8G8B8 */
    case 21: case 22: case 70: case 71: case 75: case 77: return w * h * 4u;  /* 32-bit, D24S8/X8 */
    case 28: case 41: case 50: return w * h;                                  /* A8, P8, L8 */
    default: return w * h * 2u;                                               /* 16-bit formats */
    }
}
/* D3DSURFACE_DESC9 (Format,Type,Usage,Pool,MSType,MSQuality,Width,Height) at s9 ->
 * D3DSURFACE_DESC8 (Format,Type,Usage,Pool,Size,MSType,Width,Height) at d8 */
static void desc9to8(uint32_t s9, uint32_t d8){
    uint32_t f = MR32(s9), t = MR32(s9 + 4), u = MR32(s9 + 8), p = MR32(s9 + 12), ms = MR32(s9 + 16),
             w = MR32(s9 + 24), h = MR32(s9 + 28);
    mem_commit(d8, 32);
    MR32(d8) = f; MR32(d8 + 4) = t; MR32(d8 + 8) = u; MR32(d8 + 12) = p;
    MR32(d8 + 16) = fmt_size(f, w, h); MR32(d8 + 20) = ms; MR32(d8 + 24) = w; MR32(d8 + 28) = h;
}
/* D3DPRESENT_PARAMETERS8 (13 dwords) <-> D3DPRESENT_PARAMETERS9 (14 dwords: MultiSampleQuality after MSType) */
static void pp8to9(uint32_t p8, uint32_t p9){
    for (int i = 0; i < 5; i++) MR32(p9 + 4u * i) = MR32(p8 + 4u * i);
    MR32(p9 + 20) = 0;
    for (int i = 5; i < 13; i++) MR32(p9 + 4u * (i + 1)) = MR32(p8 + 4u * i);
}
static void pp9to8(uint32_t p9, uint32_t p8){
    for (int i = 0; i < 5; i++) MR32(p8 + 4u * i) = MR32(p9 + 4u * i);
    for (int i = 5; i < 13; i++) MR32(p8 + 4u * i) = MR32(p9 + 4u * (i + 1));
}

/* ---- device-side D3D8 state the D3D9 API keeps elsewhere ---------------------------------- */
static uint32_t g_base_vertex;          /* SetIndices(ib, BaseVertexIndex) */
static uint32_t g_cur_vs;               /* current D3D8 vertex shader handle (FVF or shader) */
static uint32_t g_d3d8obj;              /* the IDirect3D8 object */

#define MAXVS8 512
typedef struct { uint32_t decl9, vs9, consts, nconst; int used; } VS8;
static VS8 g_vs8[MAXVS8];

/* D3D8 vertex declaration -> D3DVERTEXELEMENT9 array (in scratch), constant block remembered.
 * Register r gets a fixed (usage, index) pair: D3D8's fixed-function register meaning
 * (D3DVSDE_*), which is also unique per register for programmable shaders (dcl injected below). */
static const uint8_t REG_USAGE[17][2] = {
    {0,0},{1,0},{2,0},{3,0},{4,0},{10,0},{10,1},{5,0},{5,1},{5,2},{5,3},{5,4},{5,5},{5,6},{5,7},{0,1},{3,1} };
static const uint8_t TYPE_SIZE[8] = { 4, 8, 12, 16, 4, 4, 4, 8 };

static int parse_decl(uint32_t d, uint32_t el9, int maxel, uint32_t *consts, uint32_t *nconst, uint32_t *regmask){
    int n = 0; uint32_t stream = 0, off = 0;
    *consts = 0; *nconst = 0; *regmask = 0;
    for (int guard = 0; guard < 256 && mem_is_committed(d); guard++){
        uint32_t t = MR32(d); d += 4;
        uint32_t kind = t >> 29;
        if (t == 0xFFFFFFFFu) break;                          /* D3DVSD_END */
        if (kind == 1){ stream = t & 0xFu; off = 0; }         /* D3DVSD_STREAM */
        else if (kind == 2){                                  /* D3DVSD_REG / D3DVSD_SKIP */
            if (t & 0x10000000u){ off += 4u * ((t >> 16) & 0xFu); continue; }
            uint32_t type = (t >> 16) & 0xFu, reg = t & 0x1Fu;
            if (type > 7 || reg > 16 || n >= maxel - 1) continue;
            uint32_t e = el9 + 8u * (uint32_t)n++;
            MR16(e) = (uint16_t)stream; MR16(e + 2) = (uint16_t)off;
            MR8(e + 4) = (uint8_t)type; MR8(e + 5) = 0;
            MR8(e + 6) = REG_USAGE[reg][0]; MR8(e + 7) = REG_USAGE[reg][1];
            *regmask |= 1u << reg;
            off += TYPE_SIZE[type];
        }
        else if (kind == 4){                                  /* D3DVSD_CONST(addr, count) + data */
            uint32_t cnt = (t >> 25) & 0xFu;
            if (!*consts){ *consts = d - 4; }
            *nconst += 1;
            d += 16u * cnt;
        }
        /* NOP (0), tessellator (3), extension (5): ignored */
    }
    uint32_t e = el9 + 8u * (uint32_t)n;                      /* D3DDECL_END */
    MR16(e) = 0xFF; MR16(e + 2) = 0; MR8(e + 4) = 17; MR8(e + 5) = 0; MR8(e + 6) = 0; MR8(e + 7) = 0;
    return n;
}
static void apply_decl_consts(CPU *c, uint32_t dev, VS8 *v){
    uint32_t d = v->consts;
    for (uint32_t k = 0; d && k < v->nconst; k++){
        uint32_t t = MR32(d), addr = t & 0x7Fu, cnt = (t >> 25) & 0xFu;
        C9(IF_DEV, "SetVertexShaderConstantF", dev, addr, d + 4, cnt);
        d += 4u + 16u * cnt;
        while ((MR32(d) >> 29) != 4 && k + 1 < v->nconst) d += 4;   /* next const block */
    }
}
/* vs.1.x bytecode from D3D8 has no dcl: copy it, inserting dcl_<usage> v<reg> for every declared register */
static uint32_t inject_dcl(uint32_t func, uint32_t regmask){
    uint32_t len = 1;
    while (len < 65536 && MR32(func + 4u * len) != 0x0000FFFFu) len++;
    len++;                                                    /* include END */
    uint32_t ndcl = (uint32_t)__builtin_popcount(regmask);
    uint32_t out = com_scratch(4u * (len + 3u * ndcl));
    uint32_t o = out;
    MR32(o) = MR32(func); o += 4;                             /* version token */
    for (uint32_t r = 0; r < 17; r++) if (regmask & (1u << r)){
        MR32(o) = 0x0000001Fu;                                /* dcl */
        MR32(o + 4) = 0x80000000u | REG_USAGE[r][0] | ((uint32_t)REG_USAGE[r][1] << 16);
        MR32(o + 8) = 0x900F0000u | r;                        /* v<r>, full mask */
        o += 12;
    }
    for (uint32_t i = 1; i < len; i++){ MR32(o) = MR32(func + 4u * i); o += 4; }
    return out;
}

/* ---- D3D8 render / texture-stage state translation ---------------------------------------- */
static int tss_to_sampler(uint32_t t){
    switch (t){
    case 13: return 1; case 14: return 2; case 25: return 3; case 15: return 4; case 16: return 5;
    case 17: return 6; case 18: return 7; case 19: return 8; case 20: return 9; case 21: return 10;
    }
    return 0;
}

/* ---- fixed-function vertex processing ----------------------------------------------------
 * com.c's no-shader path takes positions as clip space in NFSU2's layout (pos@0 col@12 uv@16) and
 * ignores the FVF. HP2 draws its front end and movies with FVFs (XYZRHW screen coordinates or XYZ
 * through World*View*Projection), so FVF draws are converted here into a canonical clip-space vertex
 * {float4 pos, D3DCOLOR diffuse, float2 uv0} with a matching D3D9 declaration. No lighting yet. */
static uint32_t g_tss8[8][32], g_rs8[256], g_tex8[8];
static uint64_t g_presents8;  /* D3D8 state, tracked for diagnostics/emulation */
static float g_mw[16], g_mv[16], g_mp[16];          /* World (256), View (2), Projection (3) */
static int g_mat_init;
static uint32_t g_vp[6];                             /* X Y W H (u32), MinZ MaxZ (float bits) */
static uint32_t g_bbw = 640, g_bbh = 480;            /* back buffer size the game asked for */
static uint32_t g_vb0, g_vb0_stride, g_ib;           /* stream 0, indices */
static uint32_t g_cdecl;                             /* canonical D3D9 declaration */
static uint32_t g_ffbuf, g_ffcap;                    /* converted vertices (guest memory) */
#define CSTRIDE 28u

static void mat_ident(float *m){ memset(m, 0, 64); m[0] = m[5] = m[10] = m[15] = 1.f; }
static void mat_mul(const float *a, const float *b, float *o){   /* row-major, row vectors: o = a*b */
    float t[16];
    for (int r = 0; r < 4; r++) for (int c2 = 0; c2 < 4; c2++){
        float v = 0; for (int k = 0; k < 4; k++) v += a[r*4+k] * b[k*4+c2]; t[r*4+c2] = v; }
    memcpy(o, t, 64);
}
static float *mat_for(uint32_t st){ return st == 256 ? g_mw : st == 2 ? g_mv : st == 3 ? g_mp : 0; }
static void mats_init(void){ if (!g_mat_init){ mat_ident(g_mw); mat_ident(g_mv); mat_ident(g_mp); g_mat_init = 1; } }

static uint32_t prim_verts(uint32_t pt, uint32_t n){
    switch (pt){ case 1: return n; case 2: return 2*n; case 3: return n+1; case 4: return 3*n;
                 case 5: case 6: return n+2; } return 0;
}
static uint32_t ff_cdecl(CPU *c, uint32_t dev){
    if (g_cdecl) return g_cdecl;
    uint32_t el = com_scratch(8u * 4u);
    static const uint8_t E[4][8] = { {0,0, 0,0, 3,0, 0,0},      /* FLOAT4 POSITION0 @0  */
                                     {0,0, 16,0, 4,0, 10,0},    /* D3DCOLOR COLOR0 @16  */
                                     {0,0, 20,0, 1,0, 5,0},     /* FLOAT2 TEXCOORD0 @20 */
                                     {0xFF,0, 0,0, 17,0, 0,0} };/* END */
    for (int i = 0; i < 4; i++) for (int j = 0; j < 8; j++) MR8(el + 8u*i + j) = E[i][j];
    uint32_t pp = scratch(4);
    C9(IF_DEV, "CreateVertexDeclaration", dev, el, pp);
    g_cdecl = MR32(pp);
    return g_cdecl;
}
/* convert nv FVF vertices at guest src (stride) into g_ffbuf; returns guest address or 0 */
static uint32_t ff_convert(uint32_t fvf, uint32_t src, uint32_t nv, uint32_t stride){
    if (!nv || !stride || nv > 1000000u) return 0;
    if (g_ffcap < nv * CSTRIDE){ g_ffcap = nv * CSTRIDE + 65536u; g_ffbuf = com_scratch(g_ffcap); }
    mats_init();
    uint32_t pos = fvf & 0x00Eu, off = 0;
    int rhw = (pos == 0x004u);
    off = pos == 0x002u ? 12 : pos == 0x004u ? 16 : pos >= 0x006u ? 12 + 4 * ((pos - 0x004u) / 2u) : 12;
    if (fvf & 0x010u) off += 12;                      /* normal */
    if (fvf & 0x020u) off += 4;                       /* point size */
    int c_off = (fvf & 0x040u) ? (int)off : -1; if (fvf & 0x040u) off += 4;
    if (fvf & 0x080u) off += 4;                       /* specular */
    int t_off = ((fvf >> 8) & 0xFu) ? (int)off : -1;
    float wvp[16]; mat_mul(g_mw, g_mv, wvp); mat_mul(wvp, g_mp, wvp);
    /* com.c ignores SetViewport (NFSU2 always draws full screen). XYZRHW positions are absolute
     * render-target pixels in D3D, so map them against the whole target; XYZ clip coordinates get the
     * viewport mapping folded in (D3D maps NDC into the viewport rectangle). */
    float rw = (float)g_bbw, rh = (float)g_bbh;
    float vx = (float)g_vp[0], vy = (float)g_vp[1], vw = (float)(g_vp[2] ? g_vp[2] : g_bbw), vh = (float)(g_vp[3] ? g_vp[3] : g_bbh);
    float sx = vw / rw, sy = vh / rh, ox = (2.f * vx + vw) / rw - 1.f, oy = 1.f - (2.f * vy + vh) / rh;
    int fullvp = (vx == 0.f && vy == 0.f && vw == rw && vh == rh);
    for (uint32_t i = 0; i < nv; i++){
        uint32_t v = src + i * stride, o = g_ffbuf + i * CSTRIDE;
        float x = MRF32(v), y = MRF32(v + 4), z = MRF32(v + 8), cx, cy, cz, cw;
        if (rhw){
            float r = MRF32(v + 12); cw = (r > 0.f) ? 1.f / r : 1.f;
            cx = (x / rw * 2.f - 1.f) * cw; cy = (1.f - y / rh * 2.f) * cw; cz = z * cw;
        } else {
            cx = x*wvp[0] + y*wvp[4] + z*wvp[8]  + wvp[12];
            cy = x*wvp[1] + y*wvp[5] + z*wvp[9]  + wvp[13];
            cz = x*wvp[2] + y*wvp[6] + z*wvp[10] + wvp[14];
            cw = x*wvp[3] + y*wvp[7] + z*wvp[11] + wvp[15];
            if (!fullvp){ cx = cx * sx + cw * ox; cy = cy * sy + cw * oy; }
        }
        uint32_t u; memcpy(&u, &cx, 4); MR32(o) = u; memcpy(&u, &cy, 4); MR32(o + 4) = u;
        memcpy(&u, &cz, 4); MR32(o + 8) = u; memcpy(&u, &cw, 4); MR32(o + 12) = u;
        MR32(o + 16) = c_off >= 0 ? MR32(v + (uint32_t)c_off) : 0xFFFFFFFFu;
#ifdef HP2_EXPERIMENT_TINT_FONT
        if (g_tex8[0] == HP2_EXPERIMENT_TINT_FONT) MR32(o + 16) = 0xFFFF0000u;   /* red */
#endif
        MR32(o + 20) = t_off >= 0 ? MR32(v + (uint32_t)t_off) : 0u;
        MR32(o + 24) = t_off >= 0 ? MR32(v + (uint32_t)t_off + 4) : 0u;
    }
    { static int lg = -1; static long nlog; static uint32_t seen[64]; static int ns;
      if (lg < 0){ const char *e = getenv("HP2_FFLOG"); lg = e ? atoi(e) : 0; }
      int fresh = 1; for (int q = 0; q < ns; q++) if (seen[q] == (fvf << 8 | (nv & 0xFF))) fresh = 0;
      int frame_dump = (lg == 3 && g_presents8 > 0 && (g_presents8 % 20000u) == 0);
      if (lg && nlog < 600 && (fresh || lg == 2 || frame_dump)){
          nlog++; if (fresh && ns < 64) seen[ns++] = fvf << 8 | (nv & 0xFF);
          float x0 = MRF32(src), y0 = MRF32(src + 4), z0 = MRF32(src + 8), w0 = MRF32(src + 12);
          fprintf(stderr, "[ff] fvf=%03x nv=%u stride=%u v0=(%.1f %.1f %.3f %.3f) clip0=(%.3f %.3f %.3f %.3f) col=%08x uv=(%.3f %.3f) vp=%u,%u %ux%u proj00=%.3f view00=%.3f world00=%.3f\n",
                  fvf, nv, stride, x0, y0, z0, w0, MRF32(g_ffbuf), MRF32(g_ffbuf + 4), MRF32(g_ffbuf + 8), MRF32(g_ffbuf + 12),
                  MR32(g_ffbuf + 16), MRF32(g_ffbuf + 20), MRF32(g_ffbuf + 24), g_vp[0], g_vp[1], g_vp[2], g_vp[3],
                  g_mp[0], g_mv[0], g_mw[0]);
          if (fvf == 0x42 && nv == 4){
              fprintf(stderr, "[ff]    proj:"); for (int q = 0; q < 16; q++) fprintf(stderr, " %.4g", g_mp[q]);
              fprintf(stderr, "\n[ff]    view:"); for (int q = 0; q < 16; q++) fprintf(stderr, " %.4g", g_mv[q]);
              fprintf(stderr, "\n[ff]    world:"); for (int q = 0; q < 16; q++) fprintf(stderr, " %.4g", g_mw[q]);
              fprintf(stderr, "\n[ff]    verts:");
              for (uint32_t q = 0; q < 4; q++) fprintf(stderr, " (%.3f %.3f %.3f)->(%.4f %.4f %.6f %.4f)", MRF32(src + q*stride), MRF32(src + q*stride + 4), MRF32(src + q*stride + 8),
                  MRF32(g_ffbuf + q*CSTRIDE), MRF32(g_ffbuf + q*CSTRIDE + 4), MRF32(g_ffbuf + q*CSTRIDE + 8), MRF32(g_ffbuf + q*CSTRIDE + 12));
              fprintf(stderr, "\n");
          }
          uint32_t t0 = g_tex8[0];
          fprintf(stderr, "[ff]    tex0=%08x fmt=%u | st0 cop=%u a1=%u a2=%u aop=%u a1=%u a2=%u | st1 cop=%u aop=%u | tfactor=%08x blend=%u src=%u dst=%u atest=%u aref=%u light=%u zen=%u zw=%u zfunc=%u stencil=%u cwrite=%x\n",
                  t0, (t0 && mem_is_committed(t0 + 23u)) ? MR32(t0 + 20) : 0u,
                  g_tss8[0][1], g_tss8[0][2], g_tss8[0][3], g_tss8[0][4], g_tss8[0][5], g_tss8[0][6], g_tss8[1][1], g_tss8[1][4],
                  g_rs8[60], g_rs8[27], g_rs8[19], g_rs8[20], g_rs8[15], g_rs8[24], g_rs8[137], g_rs8[7], g_rs8[14], g_rs8[23], g_rs8[52], g_rs8[168]);
      } }
    return g_ffbuf;
}
/* com.c has no DrawIndexedPrimitiveUP (NFSU2 never used it; the call was a silent no-op). Expand the
 * indices into a plain vertex list: vertex i = verts[index[i]], same primitive type and count. */
static uint32_t g_xbuf, g_xcap;
static uint32_t expand_indexed(uint32_t verts, uint32_t stride, uint32_t idx, uint32_t ifmt, uint32_t nidx, uint32_t nverts){
    if (!nidx || !stride || nidx > 2000000u) return 0;
    if (g_xcap < nidx * stride){ g_xcap = nidx * stride + 65536u; g_xbuf = com_scratch(g_xcap); }
    for (uint32_t i = 0; i < nidx; i++){
        uint32_t k = ifmt == 102u ? MR32(idx + 4u * i) : MR16(idx + 2u * i);
        if (k >= nverts) k = 0;
        memcpy(g_mem + g_xbuf + i * stride, g_mem + verts + k * stride, stride);
    }
    return g_xbuf;
}
static int ff_active(void){ return !(g_cur_vs & 1u) && g_cur_vs; }
static int g_ff_dirty;                               /* canonical decl bound since the last SetVertexShader */
static void ff_bind(CPU *c, uint32_t dev){
    C9(IF_DEV, "SetVertexShader", dev, 0u);
    C9(IF_DEV, "SetVertexDeclaration", dev, ff_cdecl(c, dev));
    g_ff_dirty = 1;
}

/* level of a 2D texture's child surface (0 = level 0 or not a texture level) */
static uint32_t surf_level(uint32_t sf, uint32_t *parent_out){
    uint32_t parent = (MR32(sf - 12u) & 4u) ? MR32(sf - 8u) : 0u;
    if (parent_out) *parent_out = parent;
    if (!parent || !mem_is_committed(parent + 103u) || (MR32(parent - 12u) & 1u)) return 0;
    for (uint32_t q = 1; q < 10u; q++) if (MR32(parent + 64u + 4u * q) == sf) return q;
    return 0;
}
/* UnlockRect of a surface, uploading a texture level > 0 to its own level (com.c always uses level 0) */
static uint32_t surf_unlock(CPU *c, uint32_t sf){
    uint32_t parent, lvl = surf_level(sf, &parent);
    if (!lvl) return C9(IF_SURF, "UnlockRect", sf);
    unsigned metal_level_pitch(unsigned, unsigned, unsigned);
    void metal_upload_texture(unsigned, unsigned, const void *, unsigned, unsigned, unsigned, unsigned);
    uint32_t buf = MR32(sf + 32), tid = MR32(parent + 28), fmt = MR32(parent + 20), W = MR32(parent + 4), H = MR32(parent + 8);
    if (buf && tid && W && H) metal_upload_texture(tid, lvl, g_mem + buf, metal_level_pitch(W, lvl, fmt), W, H, fmt);
    return 0;
}

static uint64_t g_calls8, g_unimpl8;

/* ---- the D3D8 dispatcher ------------------------------------------------------------------ */
int hp2_d3d8_call(CPU *c, uint32_t magic){
    uint32_t k = ((magic >> 16) & 0x1FFFu) - K_BASE, m = magic & 0xFFFFu;
    if (k >= K_N || m >= K8[k].n){
        POP32(c->eip);
        fprintf(stderr, "hp2_d3d8: bad magic 0x%08x\n", magic); c->eax = 0x80004001u; return 1;
    }
    g_calls8++;
    const M8 *M = &K8[k].m[m];
    const char *nm = M->name;
    int nargs = M->nargs, if9 = K8[k].if9;
    POP32(c->eip);
    const uint32_t sv_ebx = c->ebx, sv_esi = c->esi, sv_edi = c->edi, sv_ebp = c->ebp;   /* callee-saved (stdcall) */
    uint32_t a[10] = {0};
    for (int i = 0; i < nargs && i < 10; i++) a[i] = MR32(c->esp + 4u * (uint32_t)i);
    uint32_t r = 0, th = a[0];
    int done = 1;

#define IS(s) (!strcmp(nm, (s)))
    /* -- IUnknown -- */
    if (IS("QueryInterface")){ if (a[2]){ mem_commit(a[2], 4); MR32(a[2]) = th; com_obj_addref(c, th); } r = 0; }
    else if (IS("AddRef") || IS("Release")) r = C9(if9, nm, th);
    else if (k == K_D3D){
        /* hp2wsfix: the game only accepts a resolution that is in this list, so the widescreen
         * size (runtime/hp2_widescreen.c) is appended as one more mode */
        if (IS("GetAdapterModeCount")){ r = C9(IF_D3D9, nm, th, a[1], 22u); if (hp2ws_mode(0, 0)) r++; }
        else if (IS("EnumAdapterModes")){
            uint32_t n = C9(IF_D3D9, "GetAdapterModeCount", th, a[1], 22u), w, h;
            if (a[2] == n && a[3] && hp2ws_mode(&w, &h)){
                mem_commit(a[3], 16); MR32(a[3]) = w; MR32(a[3] + 4) = h; MR32(a[3] + 8) = 60; MR32(a[3] + 12) = 22u;
                r = 0;
            } else r = C9(IF_D3D9, nm, th, a[1], 22u, a[2], a[3]);
        }
        else if (IS("CheckDeviceMultiSampleType")) r = C9(IF_D3D9, nm, th, a[1], a[2], a[3], a[4], a[5], 0u);
        else if (IS("GetAdapterIdentifier")){
            uint32_t s = scratch(1200);
            r = C9(IF_D3D9, nm, th, a[1], a[2], s);
            /* ID9 = Driver[512] Description[512] DeviceName[32] + 44-byte tail; ID8 has no DeviceName */
            if (a[3]){ mem_commit(a[3], 1068); memcpy(g_mem + a[3], g_mem + s, 1024);
                       memcpy(g_mem + a[3] + 1024, g_mem + s + 1056, 44); }
        }
        else if (IS("GetDeviceCaps")){
            uint32_t s = scratch(512);
            r = C9(IF_D3D9, nm, th, a[1], a[2], s);
            if (a[3]){ mem_commit(a[3], 212); memcpy(g_mem + a[3], g_mem + s, 212); }
        }
        else if (IS("CreateDevice")){
            uint32_t p9 = scratch(64);
            pp8to9(a[5], p9);
            if (MR32(a[5])) { g_bbw = MR32(a[5]); g_bbh = MR32(a[5] + 4); }
            r = C9(IF_D3D9, nm, th, a[1], a[2], a[3], a[4], p9, a[6]);
            pp9to8(p9, a[5]);
            wrap_out(a[6]);
            fprintf(stderr, "hp2_d3d8: CreateDevice %ux%u fmt %u windowed %u -> 0x%08x dev 0x%08x\n",
                    MR32(a[5]), MR32(a[5] + 4), MR32(a[5] + 8), MR32(a[5] + 28), r, a[6] ? MR32(a[6]) : 0);
        }
        else if (IS("RegisterSoftwareDevice")) r = 0x8876086Cu;          /* D3DERR_INVALIDCALL */
        else r = call9(c, IF_D3D9, nm, a, nargs);   /* same arguments in D3D9 */
    }
    else if (k == K_DEV){
        uint32_t dev = th;
        if (IS("GetDirect3D")){ if (a[1]){ mem_commit(a[1], 4); MR32(a[1]) = g_d3d8obj; com_obj_addref(c, g_d3d8obj); } r = 0; }
        else if (IS("ResourceManagerDiscardBytes")) r = C9(IF_DEV, "EvictManagedResources", dev);
        else if (IS("GetDeviceCaps")){
            uint32_t s = scratch(512);
            r = C9(IF_DEV, nm, dev, s);
            if (a[1]){ mem_commit(a[1], 212); memcpy(g_mem + a[1], g_mem + s, 212); }
        }
        else if (IS("GetDisplayMode")) r = C9(IF_DEV, nm, dev, 0u, a[1]);
        else if (IS("GetRasterStatus")) r = C9(IF_DEV, nm, dev, 0u, a[1]);
        else if (IS("SetGammaRamp")) r = C9(IF_DEV, nm, dev, 0u, a[1], a[2]);
        else if (IS("GetGammaRamp")) r = C9(IF_DEV, nm, dev, 0u, a[1]);
        else if (IS("CreateAdditionalSwapChain") || IS("Reset")){
            uint32_t p9 = scratch(64); pp8to9(a[1], p9);
            r = IS("Reset") ? C9(IF_DEV, nm, dev, p9) : C9(IF_DEV, nm, dev, p9, a[2]);
            pp9to8(p9, a[1]);
            if (IS("CreateAdditionalSwapChain")) wrap_out(a[2]);
        }
        else if (IS("GetBackBuffer")){ r = C9(IF_DEV, nm, dev, 0u, a[1], a[2], a[3]); wrap_out(a[3]); }
        else if (IS("CreateTexture")){ r = C9(IF_DEV, nm, dev, a[1], a[2], a[3], a[4], a[5], a[6], a[7], 0u); wrap_out(a[7]); }
        else if (IS("CreateVolumeTexture")){ r = 0x8876086Cu; }
        else if (IS("CreateCubeTexture")){ r = C9(IF_DEV, nm, dev, a[1], a[2], a[3], a[4], a[5], a[6], 0u); wrap_out(a[6]); }
        else if (IS("CreateVertexBuffer") || IS("CreateIndexBuffer")){
            r = C9(IF_DEV, nm, dev, a[1], a[2], a[3], a[4], a[5], 0u); wrap_out(a[5]); }
        else if (IS("CreateRenderTarget")){
            r = C9(IF_DEV, nm, dev, a[1], a[2], a[3], a[4], 0u, a[5], a[6], 0u); wrap_out(a[6]); }
        else if (IS("CreateDepthStencilSurface")){
            r = C9(IF_DEV, nm, dev, a[1], a[2], a[3], a[4], 0u, 0u, a[5], 0u); wrap_out(a[5]); }
        else if (IS("CreateImageSurface")){
            r = C9(IF_DEV, "CreateOffscreenPlainSurface", dev, a[1], a[2], a[3], 2u /* SYSTEMMEM */, a[4], 0u);
            /* com.c creates an empty surface object: give it its size and format, or LockRect assumes
             * 256x256 and D3DX overruns the lock buffer (heap corruption) on larger images */
            uint32_t sf = (!r && a[4] && mem_is_committed(a[4])) ? MR32(a[4]) : 0u;
            if (sf && mem_is_committed(sf + 31u)){ MR32(sf + 4) = a[1]; MR32(sf + 8) = a[2]; MR32(sf + 20) = a[3]; }
            wrap_out(a[4]); }
        else if (IS("CopyRects")){
            /* (src, pRects, cRects, dst, pPoints). com.c has no UpdateSurface (a silent no-op), so copy
             * through com.c's own lock buffers: LockRect both, copy the rows, UnlockRect (uploads dst). */
            uint32_t src = a[1], rects = a[2], n = a[3], dst = a[4], pts = a[5];
            r = 0x8876086Cu;
            if (src && dst && mem_is_committed(src + 31u) && mem_is_committed(dst + 31u)){
                uint32_t fmt = MR32(src + 20), sw = MR32(src + 4), sh = MR32(src + 8);
                uint32_t dw = MR32(dst + 4), dh = MR32(dst + 8);
                /* texel blocks: 1x1 for plain formats, 4x4 for DXTn (bpp = bytes per block) */
                int dxt = (fmt >= 0x31545844u && fmt <= 0x35545844u && (fmt & 0xFFu) == 0x44u);
                uint32_t blk = dxt ? 4u : 1u;
                uint32_t bpp = dxt ? (fmt == 0x31545844u ? 8u : 16u) : fmt_size(fmt, 1, 1);
                uint32_t ls = scratch(16), ld = ls + 8;
                if (bpp && !C9(IF_SURF, "LockRect", src, ls, 0u, 0x10u /* READONLY */) &&
                    !C9(IF_SURF, "LockRect", dst, ld, 0u, 0u)){
                    uint32_t sp = MR32(ls), sb = MR32(ls + 4), dp = MR32(ld), db = MR32(ld + 4);
                    uint32_t cnt = n ? n : 1;
                    for (uint32_t i = 0; i < cnt; i++){
                        int32_t x0 = 0, y0 = 0, x1 = (int32_t)sw, y1 = (int32_t)sh, dx = 0, dy = 0;
                        if (n && rects){ x0 = (int32_t)MR32(rects + 16u*i); y0 = (int32_t)MR32(rects + 16u*i + 4);
                                         x1 = (int32_t)MR32(rects + 16u*i + 8); y1 = (int32_t)MR32(rects + 16u*i + 12); }
                        if (n && pts){ dx = (int32_t)MR32(pts + 8u*i); dy = (int32_t)MR32(pts + 8u*i + 4); }
                        else { dx = x0; dy = y0; }
                        if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
                        if (x1 > (int32_t)sw) x1 = (int32_t)sw; if (y1 > (int32_t)sh) y1 = (int32_t)sh;
                        if (dx + (x1 - x0) > (int32_t)dw) x1 = x0 + (int32_t)dw - dx;
                        if (dy + (y1 - y0) > (int32_t)dh) y1 = y0 + (int32_t)dh - dy;
                        if (x1 <= x0 || y1 <= y0 || dx < 0 || dy < 0) continue;
                        /* in blocks (DXT rects are 4-aligned; round the far edge up) */
                        uint32_t bx0 = (uint32_t)x0 / blk, by0 = (uint32_t)y0 / blk;
                        uint32_t bx1 = ((uint32_t)x1 + blk - 1) / blk, by1 = ((uint32_t)y1 + blk - 1) / blk;
                        uint32_t bdx = (uint32_t)dx / blk, bdy = (uint32_t)dy / blk;
                        uint32_t row = (bx1 - bx0) * bpp;
                        for (uint32_t y = by0; y < by1; y++)
                            memmove(g_mem + db + (bdy + y - by0) * dp + bdx * bpp,
                                    g_mem + sb + y * sp + bx0 * bpp, row);
                    }
                    surf_unlock(c, src);
                    surf_unlock(c, dst);
                    r = 0;
                }
            }
            { static int said; if (!said++) fprintf(stderr, "hp2_d3d8: CopyRects via lock buffers (fmt %u) -> 0x%x\n", src ? MR32(src + 20) : 0, r); }
        }
        else if (IS("GetFrontBuffer")) r = C9(IF_DEV, "GetFrontBufferData", dev, 0u, a[1]);
        else if (IS("SetRenderTarget")){
            if (a[1]) r = C9(IF_DEV, "SetRenderTarget", dev, 0u, a[1]);
            uint32_t r2 = C9(IF_DEV, "SetDepthStencilSurface", dev, a[2]);
            if (!r) r = r2;
        }
        else if (IS("GetRenderTarget")){ r = C9(IF_DEV, nm, dev, 0u, a[1]); wrap_out(a[1]); }
        else if (IS("GetDepthStencilSurface")){ r = C9(IF_DEV, nm, dev, a[1]); wrap_out(a[1]); }
        else if (IS("GetTexture")){ r = C9(IF_DEV, nm, dev, a[1], a[2]); wrap_out(a[2]); }
        else if (IS("SetRenderState")){
            uint32_t st = a[1], v = a[2];
            if (st < 256) g_rs8[st] = v;
            if (st == 10 || st == 30 || st == 40 || st == 164) r = 0;    /* LINEPATTERN ZVISIBLE EDGEANTIALIAS PATCHSEGMENTS */
            else if (st == 47){                                           /* ZBIAS (0..16) -> DEPTHBIAS (float) */
                float f = -(float)v * 0.000015f; uint32_t u; memcpy(&u, &f, 4);
                r = C9(IF_DEV, nm, dev, 195u, u);
            }
            else if (st == 153) r = C9(IF_DEV, "SetSoftwareVertexProcessing", dev, v);
            else r = C9(IF_DEV, nm, dev, st, v);
        }
        else if (IS("SetTextureStageState") || IS("GetTextureStageState")){
            if (IS("SetTextureStageState") && a[1] < 8 && a[2] < 32) g_tss8[a[1]][a[2]] = a[3];
            int s9 = tss_to_sampler(a[2]);
            if (s9) r = C9(IF_DEV, IS("SetTextureStageState") ? "SetSamplerState" : "GetSamplerState",
                           dev, a[1], (uint32_t)s9, a[3]);
            else r = C9(IF_DEV, nm, dev, a[1], a[2], a[3]);
        }
        /* state blocks: D3D8 DWORD tokens are the D3D9 IDirect3DStateBlock9 object pointers */
        else if (IS("EndStateBlock") || IS("CreateStateBlock")) r = call9(c, IF_DEV, nm, a, nargs);
        else if (IS("ApplyStateBlock")) r = a[1] ? C9(IF_STATEBLOCK, "Apply", a[1]) : 0x8876086Cu;
        else if (IS("CaptureStateBlock")) r = a[1] ? C9(IF_STATEBLOCK, "Capture", a[1]) : 0x8876086Cu;
        else if (IS("DeleteStateBlock")){ if (a[1]) com_obj_release(c, a[1]); r = 0; }
        else if (IS("GetInfo")) r = 1;                                    /* S_FALSE: no info */
        else if (IS("ValidateDevice")){ if (a[1]){ mem_commit(a[1], 4); MR32(a[1]) = 1; } r = 0; }  /* one pass */
        else if (IS("DrawIndexedPrimitive"))
            r = C9(IF_DEV, nm, dev, a[1], g_base_vertex, a[2], a[3], a[4], a[5]);
        else if (IS("ProcessVertices")) r = C9(IF_DEV, nm, dev, a[1], a[2], a[3], a[4], 0u, a[5]);
        else if (IS("SetStreamSource")){
            if (a[1] == 0){ g_vb0 = a[2]; g_vb0_stride = a[3]; }
            r = C9(IF_DEV, nm, dev, a[1], a[2], 0u, a[3]);
        }
        else if (IS("GetStreamSource")){
            uint32_t s = scratch(4);
            r = C9(IF_DEV, nm, dev, a[1], a[2], s, a[3]); wrap_out(a[2]);
        }
        else if (IS("SetIndices")){ g_base_vertex = a[2]; g_ib = a[1]; r = C9(IF_DEV, nm, dev, a[1]); }
        else if (IS("GetIndices")){
            r = C9(IF_DEV, nm, dev, a[1]); wrap_out(a[1]);
            if (a[2]){ mem_commit(a[2], 4); MR32(a[2]) = g_base_vertex; }
        }
        /* vertex shaders: handle = (index << 1) | 1; even handles are FVF codes */
        else if (IS("CreateVertexShader")){
            int i; for (i = 1; i < MAXVS8 && g_vs8[i].used; i++){}
            if (i >= MAXVS8){ r = 0x8007000Eu; }
            else {
                VS8 *v = &g_vs8[i]; memset(v, 0, sizeof *v);
                uint32_t el = com_scratch(8u * 20u), mask = 0;
                parse_decl(a[1], el, 20, &v->consts, &v->nconst, &mask);
                uint32_t pp = scratch(4);
                r = C9(IF_DEV, "CreateVertexDeclaration", dev, el, pp);
                v->decl9 = MR32(pp);
                if (!r && a[2]){
                    uint32_t code = inject_dcl(a[2], mask);
                    MR32(pp) = 0;
                    r = C9(IF_DEV, "CreateVertexShader", dev, code, pp);
                    v->vs9 = MR32(pp);
                }
                if (!r){ v->used = 1; if (a[3]){ mem_commit(a[3], 4); MR32(a[3]) = ((uint32_t)i << 1) | 1u; } }
                fprintf(stderr, "hp2_d3d8: CreateVertexShader #%d decl9=0x%08x vs9=0x%08x (%s) -> 0x%x\n",
                        i, v->decl9, v->vs9, a[2] ? "programmable" : "fixed-function", r);
            }
        }
        else if (IS("SetVertexShader")){
            uint32_t h = a[1]; g_cur_vs = h; g_ff_dirty = 0;
            if (!(h & 1u)){ C9(IF_DEV, "SetVertexShader", dev, 0u); r = C9(IF_DEV, "SetFVF", dev, h); }
            else {
                uint32_t i = h >> 1;
                if (i < MAXVS8 && g_vs8[i].used){
                    VS8 *v = &g_vs8[i];
                    C9(IF_DEV, "SetVertexDeclaration", dev, v->decl9);
                    r = C9(IF_DEV, "SetVertexShader", dev, v->vs9);
                    apply_decl_consts(c, dev, v);
                } else r = 0x8876086Cu;
            }
        }
        else if (IS("GetVertexShader")){ if (a[1]){ mem_commit(a[1], 4); MR32(a[1]) = g_cur_vs; } r = 0; }
        else if (IS("DeleteVertexShader")){
            uint32_t i = a[1] >> 1;
            if ((a[1] & 1u) && i < MAXVS8 && g_vs8[i].used){
                if (g_vs8[i].vs9) com_obj_release(c, g_vs8[i].vs9);
                if (g_vs8[i].decl9) com_obj_release(c, g_vs8[i].decl9);
                g_vs8[i].used = 0;
            }
            r = 0;
        }
        else if (IS("SetVertexShaderConstant")) r = C9(IF_DEV, "SetVertexShaderConstantF", dev, a[1], a[2], a[3]);
        else if (IS("GetVertexShaderConstant")) r = C9(IF_DEV, "GetVertexShaderConstantF", dev, a[1], a[2], a[3]);
        else if (IS("GetVertexShaderDeclaration") || IS("GetVertexShaderFunction") || IS("GetPixelShaderFunction"))
            r = 0x8876086Cu;
        /* pixel shaders: handle = the D3D9 shader object; ps.1.x bytecode is accepted as is */
        else if (IS("CreatePixelShader")) r = C9(IF_DEV, nm, dev, a[1], a[2]);
        else if (IS("SetPixelShader")) r = C9(IF_DEV, nm, dev, a[1]);
        else if (IS("GetPixelShader")) r = C9(IF_DEV, nm, dev, a[1]);
        else if (IS("DeletePixelShader")){ if (a[1]) com_obj_release(c, a[1]); r = 0; }
        else if (IS("SetPixelShaderConstant")) r = C9(IF_DEV, "SetPixelShaderConstantF", dev, a[1], a[2], a[3]);
        else if (IS("GetPixelShaderConstant")) r = C9(IF_DEV, "GetPixelShaderConstantF", dev, a[1], a[2], a[3]);
        /* a shader draw after a fixed-function draw: put the shader's declaration and program back */
        else if (g_ff_dirty && !ff_active() && (g_cur_vs & 1u) && !strncmp(nm, "Draw", 4) && (g_cur_vs >> 1) < MAXVS8
                 && g_vs8[g_cur_vs >> 1].used && (
                 C9(IF_DEV, "SetVertexDeclaration", dev, g_vs8[g_cur_vs >> 1].decl9),
                 C9(IF_DEV, "SetVertexShader", dev, g_vs8[g_cur_vs >> 1].vs9), g_ff_dirty = 0, 0)){ }
        else if (IS("Present") && (++g_presents8, 0)){ }
        else if (IS("Present") && g_presents8 % 20000u == 1 && getenv("HP2_TEXPEEK") && (({
            uint32_t t = (uint32_t)strtoul(getenv("HP2_TEXPEEK"), 0, 0);
            if (t && mem_is_committed(t + 63u)){
                uint32_t w = MR32(t + 4), h = MR32(t + 8), f = MR32(t + 20), lev = MR32(t + 24);
                fprintf(stderr, "[texpeek] tex %08x %ux%u fmt %u levels %u tid %u:", t, w, h, f, lev, MR32(t + 28));
                for (int L = 0; L < 3; L++){
                    uint32_t buf = MR32(t + 32u + 4u * (uint32_t)L), nz = 0, n = 0;
                    if (buf && w && h && w <= 2048 && h <= 2048){
                        uint32_t lw = w >> L, lh = h >> L; if (!lw) lw = 1; if (!lh) lh = 1;
                        for (uint32_t i = 0; i < lw * lh; i++){ n++; if (MR16(buf + 2u * i) & 0xF000u) nz++; }
                    }
                    fprintf(stderr, " L%d buf=%08x alpha>0 %u/%u", L, buf, nz, n);
                }
                uint32_t sf = MR32(t + 64);
                { uint32_t cb = (sf && mem_is_committed(sf + 35u)) ? MR32(sf + 32) : 0, nz2 = 0, n2 = 0, any = 0;
                  if (cb) for (uint32_t i = 0; i < w * h; i++){ uint32_t v = MR16(cb + 2u * i); n2++; if (v & 0xF000u) nz2++; if (v) any++; }
                  fprintf(stderr, " | child0 %08x buf %08x alpha>0 %u nonzero %u /%u  sf: w=%u fmt=%u tid=%u\n", sf, cb, nz2, any, n2,
                          sf ? MR32(sf + 4) : 0, sf ? MR32(sf + 20) : 0, sf ? MR32(sf + 28) : 0); }
            } }), 0)){ }
        else if (IS("DrawIndexedPrimitiveUP") && !ff_active()){
            uint32_t nidx = prim_verts(a[1], a[4]);
            uint32_t xb = expand_indexed(a[7], a[8], a[5], a[6], nidx, a[2] + a[3]);
            r = xb ? C9(IF_DEV, "DrawPrimitiveUP", dev, a[1], a[4], xb, a[8]) : 0x8876086Cu;
        }
        /* fixed-function draws (FVF bound): convert to clip space, draw as UP with the canonical decl */
#ifdef HP2_EXPERIMENT_SKIP_BLACK
        else if (IS("DrawPrimitiveUP") && ff_active() && g_cur_vs == 0x42 && a[4] == 16 && MR32(a[3] + 12) == 0xff000000u && !g_tex8[0]) r = 0;
#endif
        else if (IS("DrawPrimitiveUP") && ff_active()){
            uint32_t nv = prim_verts(a[1], a[2]), vb = ff_convert(g_cur_vs, a[3], nv, a[4]);
            if (vb){ ff_bind(c, dev); r = C9(IF_DEV, nm, dev, a[1], a[2], vb, CSTRIDE); }
        }
        else if (IS("DrawIndexedPrimitiveUP") && ff_active()){
            /* (pt, MinVertexIndex, NumVertexIndices, PrimitiveCount, pIndexData, IndexDataFormat, pVertexData, stride) */
            uint32_t vb = ff_convert(g_cur_vs, a[7], a[2] + a[3], a[8]);
            uint32_t nidx = prim_verts(a[1], a[4]);
            uint32_t xb = vb ? expand_indexed(vb, CSTRIDE, a[5], a[6], nidx, a[2] + a[3]) : 0;
            if (xb){ ff_bind(c, dev); r = C9(IF_DEV, "DrawPrimitiveUP", dev, a[1], a[4], xb, CSTRIDE); }
        }
        else if (IS("DrawPrimitive") && ff_active()){
            /* (pt, StartVertex, PrimitiveCount) from stream 0 */
            uint32_t nv = prim_verts(a[1], a[3]), base = g_vb0 ? MR32(g_vb0 + 8) : 0;
            uint32_t vb = base ? ff_convert(g_cur_vs, base + a[2] * g_vb0_stride, nv, g_vb0_stride) : 0;
            if (vb){ ff_bind(c, dev); r = C9(IF_DEV, "DrawPrimitiveUP", dev, a[1], a[3], vb, CSTRIDE); }
        }
        else if (IS("DrawIndexedPrimitive") && ff_active()){
            /* D3D8 (pt, MinIndex, NumVertices, StartIndex, PrimCount) + SetIndices base vertex */
            uint32_t vbase = g_vb0 ? MR32(g_vb0 + 8) : 0, ibase = g_ib ? MR32(g_ib + 8) : 0;
            uint32_t ifmt = g_ib ? MR32(g_ib + 12) : 101u, isz = ifmt == 102u ? 4u : 2u;
            uint32_t vb = (vbase && ibase) ? ff_convert(g_cur_vs, vbase + g_base_vertex * g_vb0_stride, a[2] + a[3], g_vb0_stride) : 0;
            uint32_t nidx = prim_verts(a[1], a[5]);
            uint32_t xb = vb ? expand_indexed(vb, CSTRIDE, ibase + a[4] * isz, ifmt, nidx, a[2] + a[3]) : 0;
            if (xb){ ff_bind(c, dev); r = C9(IF_DEV, "DrawPrimitiveUP", dev, a[1], a[5], xb, CSTRIDE); }
        }
        /* identical in D3D9 (same name, same arguments) */
        else if (slot9(IF_DEV, nm) >= 0 && M_DEVICE[slot9(IF_DEV, nm)].nargs == nargs){
            /* fixed-function state the conversion needs */
            if (IS("SetTransform") || IS("MultiplyTransform")){
                float *m = mat_for(a[1]); mats_init();
                if (m && a[2] && mem_is_committed(a[2] + 63u)){
                    float t[16]; for (int q = 0; q < 16; q++) t[q] = MRF32(a[2] + 4u * (uint32_t)q);
                    if (IS("SetTransform")) memcpy(m, t, 64); else mat_mul(m, t, m);
                }
            }
            else if (IS("SetTexture") && a[1] < 8) g_tex8[a[1]] = a[2];
            else if (IS("SetViewport") && a[1] && mem_is_committed(a[1] + 23u))
                for (int q = 0; q < 6; q++) g_vp[q] = MR32(a[1] + 4u * (uint32_t)q);
            r = call9(c, IF_DEV, nm, a, nargs);
        }
        else done = 0;
    }
    else if (k == K_TEX || k == K_CUBE){
        if (IS("GetLevelDesc")){
            uint32_t s = scratch(32);
            r = C9(IF_TEX, nm, th, a[1], s); desc9to8(s, a[2]);
        }
        else if (IS("GetSurfaceLevel")){
            r = C9(IF_TEX, nm, th, a[1], a[2]); wrap_out(a[2]);
            /* com.c gives every level's surface the level-0 size and uploads it to level 0: make the
             * surface the level's size (GetDesc/LockRect), uploads go to that level (Surface8::UnlockRect) */
            uint32_t sf = (!r && a[2] && mem_is_committed(a[2])) ? MR32(a[2]) : 0, L = a[1];
            if (sf && L && L < 16 && mem_is_committed(sf + 31u)){
                uint32_t w = MR32(th + 4) >> L, h = MR32(th + 8) >> L;
                MR32(sf + 4) = w ? w : 1; MR32(sf + 8) = h ? h : 1;
            }
        }
        else if (IS("GetCubeMapSurface")){ r = C9(IF_TEX, nm, th, a[1], a[2], a[3]); wrap_out(a[3]); }
        else if (IS("GetDevice")){ r = C9(IF_TEX, nm, th, a[1]); wrap_out(a[1]); }
        else if (slot9(IF_TEX, nm) >= 0) r = call9(c, IF_TEX, nm, a, nargs);
        else done = 0;
    }
    else if (k == K_SURF){
        if (IS("UnlockRect")) r = surf_unlock(c, th);
        else if (IS("GetDesc")){ uint32_t s = scratch(32); r = C9(IF_SURF, nm, th, s); desc9to8(s, a[1]); }
        else if (IS("GetContainer")){
            /* (riid, ppContainer): a texture level's container is its texture (com.c CH_PARENT of a
             * CF_CHILD surface); IID_IDirect3DDevice8 -> the device. com.c has no GetContainer. */
            static const uint8_t IID_DEV8[16] = {0x60,0x7f,0x5e,0x7d,0x15,0xda,0xe8,0x4b,0x9b,0x11,0xb1,0x13,0x2e,0x6e,0x1d,0x4a};
            uint32_t riid = a[1], pp = a[2], parent = (MR32(th - 12u) & 4u) ? MR32(th - 8u) : 0u;
            int want_dev = riid && mem_is_committed(riid + 15u) && !memcmp(g_mem + riid, IID_DEV8, 16);
            uint32_t out = 0;
            if (want_dev){ uint32_t s4 = scratch(4); C9(IF_SURF, "GetDevice", th, s4); out = MR32(s4); }
            else if (parent){ out = parent; com_obj_addref(c, parent); }
            if (pp){ mem_commit(pp, 4); MR32(pp) = out ? wrap(out) : 0u; }
            r = out ? 0u : 0x80004002u;                                  /* E_NOINTERFACE */
        }
        else if (IS("GetDevice")){ r = call9(c, IF_SURF, nm, a, nargs); if (!r) wrap_out(a[1]); }
        else if (slot9(IF_SURF, nm) >= 0) r = call9(c, IF_SURF, nm, a, nargs);
        else done = 0;
    }
    else if (k == K_VB || k == K_IB){
        if (IS("GetDevice")){ r = call9(c, if9, nm, a, nargs); wrap_out(a[1]); }
        else if (slot9(if9, nm) >= 0) r = call9(c, if9, nm, a, nargs);
        else done = 0;
    }
    else if (k == K_SWAP){
        if (IS("Present")) r = C9(IF_SWAPCHAIN, nm, th, a[1], a[2], a[3], a[4], 0u);
        else if (IS("GetBackBuffer")){ r = C9(IF_SWAPCHAIN, nm, th, a[1], a[2], a[3]); wrap_out(a[3]); }
        else done = 0;
    }
    else done = 0;
#undef IS

    if (!done){
        g_unimpl8++;
        static int warned[K_N][128];
        if (m < 128 && !warned[k][m]){ warned[k][m] = 1;
            fprintf(stderr, "hp2_d3d8: UNIMPLEMENTED %s::%s (from 0x%08x)\n", K8[k].name, nm, c->eip); }
        r = 0x80004001u;                                                  /* E_NOTIMPL */
    }
    { static int tr = -1; static long ntr;
      if (tr < 0){ const char *e = getenv("HP2_D3D8TRACE"); tr = e ? atoi(e) : 0; }
      if (tr && ntr < tr){ ntr++;
          fprintf(stderr, "[d3d8] %s::%s(", K8[k].name + 9, nm);
          for (int i = 1; i < nargs && i < 10; i++) fprintf(stderr, i > 1 ? ", %x" : "%x", a[i]);
          fprintf(stderr, ") = 0x%x  <- 0x%08x\n", r, c->eip); } }
    if (c->ebx != sv_ebx || c->esi != sv_esi || c->edi != sv_edi || c->ebp != sv_ebp){
        static int warned;
        if (warned++ < 20)
            fprintf(stderr, "hp2_d3d8: %s::%s clobbered callee-saved registers (ebx %08x->%08x esi %08x->%08x "
                    "edi %08x->%08x ebp %08x->%08x) - restored\n", K8[k].name, nm, sv_ebx, c->ebx, sv_esi, c->esi,
                    sv_edi, c->edi, sv_ebp, c->ebp);
        c->ebx = sv_ebx; c->esi = sv_esi; c->edi = sv_edi; c->ebp = sv_ebp;
    }
    c->eax = r;
    c->esp += 4u * (uint32_t)nargs;
    return 1;
}

/* Direct3DCreate8(SDKVersion) */
uint32_t hp2_direct3dcreate8(uint32_t sdk){
    make_vtables();
    uint32_t o = com_direct3dcreate9();
    g_d3d8obj = wrap(o);
    fprintf(stderr, "hp2_d3d8: Direct3DCreate8(SDK %u) -> IDirect3D8 0x%08x\n", sdk, o);
    return o;
}

void hp2_d3d8_report(void){
    fprintf(stderr, "  d3d8: %llu calls, %llu unimplemented\n",
            (unsigned long long)g_calls8, (unsigned long long)g_unimpl8);
}
