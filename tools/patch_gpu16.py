#!/usr/bin/env python3
"""patch_gpu16.py <runtime dir>: 16-bit D3D texture formats in runtime/web/gpu_web.c.
Lock buffers stay 16-bit (what the game writes); the GPU texture is A8R8G8B8 and every upload is
expanded 16->32 bit (R5G6B5=23, X1R5G5B5=24, A1R5G5B5=25, A4R4G4B4=26)."""
import sys, pathlib
p = pathlib.Path(sys.argv[1]) / "web/gpu_web.c"; s = p.read_text()
def rep(a, b):
    global s
    assert a in s, a[:60]; s = s.replace(a, b, 1)
rep("""    case 21: case 22: *bpb = 4; return 1;                 /* A8R8G8B8, X8R8G8B8 */""",
    """    case 21: case 22: *bpb = 4; return 1;                 /* A8R8G8B8, X8R8G8B8 */
    case 23: case 24: case 25: case 26: *bpb = 2; return 1; /* HP2: 16-bit, expanded on upload */""")
rep("""            c->a[5] = (rt ? GW_TF_RT : 0) | (cube ? GW_TF_CUBE : 0); rq_commit(c); }""",
    """            c->a[5] = (rt ? GW_TF_RT : 0) | (cube ? GW_TF_CUBE : 0);
            if (f >= 23u && f <= 26u) c->a[4] = 21u;   /* HP2: GPU side is A8R8G8B8 */
            rq_commit(c); }""")
rep("""    g_tex_uploaded++;
    uint32_t bytes = pitch * rows;""",
    """    g_tex_uploaded++;
    if (f >= 23u && f <= 26u){                         /* HP2: expand 16-bit texels to A8R8G8B8 */
        uint32_t op = lw * 4u, ob = op * rows;
        GwCmd *c16 = rq_begin(GW_TEX_UPLOAD, ob);
        if (!c16) return;
        c16->a[0] = id; c16->a[1] = level | (face << 16); c16->a[2] = lw; c16->a[3] = lh; c16->a[4] = op; c16->a[5] = rows;
        uint32_t *o = (uint32_t *)((uint8_t *)c16 + GW_CMD_HDR);
        for (unsigned y = 0; y < rows; y++){
            const uint16_t *src = (const uint16_t *)((const uint8_t *)data + (size_t)y * pitch);
            for (unsigned x = 0; x < lw; x++){
                unsigned v = src[x], a = 255, r, g, b;
                if (f == 23u){ r = (v >> 11) & 31; g = (v >> 5) & 63; b = v & 31; r = r << 3 | r >> 2; g = g << 2 | g >> 4; b = b << 3 | b >> 2; }
                else if (f == 26u){ a = (v >> 12) * 17; r = ((v >> 8) & 15) * 17; g = ((v >> 4) & 15) * 17; b = (v & 15) * 17; }
                else { r = (v >> 10) & 31; g = (v >> 5) & 31; b = v & 31; r = r << 3 | r >> 2; g = g << 3 | g >> 2; b = b << 3 | b >> 2;
                       if (f == 25u) a = (v & 0x8000u) ? 255 : 0; }
                o[(size_t)y * lw + x] = a << 24 | r << 16 | g << 8 | b;   /* A8R8G8B8 little-endian = BGRA bytes */
            }
        }
        rq_commit(c16);
        return;
    }
    uint32_t bytes = pitch * rows;""")
p.write_text(s); print("gpu_web.c: 16-bit formats patched")
