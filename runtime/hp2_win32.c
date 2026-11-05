/* HP2: Win32 imports of NFSHP2.exe v242 that the nfsu2-web runtime does not implement.
 * Hooked into win32.c's import dispatcher after the runtime's own handlers (tools/apply_hp2_runtime.sh).
 * Arguments are read from the guest stack (stdcall; the dispatcher pops them via impargs.c).
 * The virtual install folder stays "C:\\NFSU2" (fileio.c maps it to /data, writes to /save). */
#include "cpu.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void win32_set_last_error(CPU *, uint32_t);

static uint32_t A(CPU *c, int i){ return MR32(c->esp + 4u * (uint32_t)i); }

static void put_str(uint32_t dst, uint32_t cap, const char *s){
    if (!dst || !cap) return;
    size_t n = strlen(s);
    if (n >= cap) n = cap - 1;
    mem_commit(dst, (uint32_t)n + 1);
    memcpy(g_mem + dst, s, n);
    g_mem[dst + n] = 0;
}

static int seen(const char *n){
    /* log each HP2-handled import once, so bring-up logs show what the game used */
    static const char *names[128]; static int k;
    for (int i = 0; i < k; i++) if (names[i] == n) return 1;
    if (k < 128) names[k++] = n;
    return 0;
}

int hp2_win32_call(CPU *c, const char *n, uint32_t *out){
    uint32_t r = 0;
    /* ---- KERNEL32 ---- */
    if (!strcmp(n, "OpenMutexA")){                 /* single-instance check: no other copy running */
        win32_set_last_error(c, 2); r = 0;
    }
    else if (!strcmp(n, "IsDBCSLeadByte")) r = 0;
    else if (!strcmp(n, "GetLogicalDriveStringsA")){   /* "C:\\\0\0" */
        uint32_t cap = A(c, 0), p = A(c, 1);
        static const char drives[] = "C:\\\0";
        if (p && cap >= sizeof drives){ mem_commit(p, sizeof drives); memcpy(g_mem + p, drives, sizeof drives); }
        r = sizeof drives - 1;
    }
    else if (!strcmp(n, "GetVolumeInformationA")){
        uint32_t name = A(c, 1), ncap = A(c, 2), serial = A(c, 3), maxc = A(c, 4), flags = A(c, 5),
                 fs = A(c, 6), fscap = A(c, 7);
        put_str(name, ncap, "HP2");
        if (serial){ mem_commit(serial, 4); MR32(serial) = 0x48503200u; }
        if (maxc){ mem_commit(maxc, 4); MR32(maxc) = 255; }
        if (flags){ mem_commit(flags, 4); MR32(flags) = 0x3u; }
        put_str(fs, fscap, "NTFS");
        r = 1;
    }
    else if (!strcmp(n, "FormatMessageA") || !strcmp(n, "FormatMessageW")) r = 0;
    else if (!strcmp(n, "FatalAppExitA")){
        uint32_t m = A(c, 1);
        fprintf(stderr, "hp2: FatalAppExitA: %s\n", (m && mem_is_committed(m)) ? (char *)(g_mem + m) : "?");
        abort();
    }
    /* ---- D3D8 (runtime/hp2_d3d8.c) ---- */
    else if (!strcmp(n, "Direct3DCreate8")){ extern uint32_t hp2_direct3dcreate8(uint32_t); r = hp2_direct3dcreate8(A(c, 0)); }
    /* ---- DINPUT (v7): IDirectInput / IDirectInputDevice are prefixes of the DI8 vtables in com.c ---- */
    else if (!strcmp(n, "DirectInputCreateA")){       /* (hinst, dwVersion, lplpDirectInput, punkOuter) */
        extern uint32_t com_directinput8create(uint32_t);
        r = com_directinput8create(A(c, 2));
    }
    /* ---- SHELL32 ---- */
    else if (!strcmp(n, "SHGetSpecialFolderPathA")){  /* (hwnd, path, csidl, create): use the game folder */
        put_str(A(c, 1), 260, "C:\\NFSU2");
        r = 1;
    }
    /* ---- USER32 ---- */
    else if (!strcmp(n, "SystemParametersInfoA")) r = 1;
    else if (!strcmp(n, "CharUpperA")){
        uint32_t p = A(c, 0);
        if ((p >> 16) == 0) r = (uint32_t)toupper((int)(p & 0xffu));
        else { for (uint32_t i = 0; i < 4096u && mem_is_committed(p + i) && g_mem[p + i]; i++)
                   g_mem[p + i] = (uint8_t)toupper(g_mem[p + i]);
               r = p; }
    }
    else if (!strcmp(n, "ChangeDisplaySettingsA")) r = 0;      /* DISP_CHANGE_SUCCESSFUL */
    else if (!strcmp(n, "SetRect")){                           /* (lprc, left, top, right, bottom) */
        uint32_t p = A(c, 0);
        if (p){ mem_commit(p, 16); MR32(p) = A(c, 1); MR32(p + 4) = A(c, 2); MR32(p + 8) = A(c, 3); MR32(p + 12) = A(c, 4); }
        r = p ? 1 : 0;
    }
    else if (!strcmp(n, "AdjustWindowRectEx") || !strcmp(n, "MoveWindow") || !strcmp(n, "BringWindowToTop") ||
             !strcmp(n, "ClipCursor") || !strcmp(n, "UnhookWindowsHookEx") || !strcmp(n, "SendNotifyMessageA") ||
             !strcmp(n, "ReleaseDC") || !strcmp(n, "KillTimer")) r = 1;
    else if (!strcmp(n, "SetWindowsHookExA")) r = 0x00B20001u;
    else if (!strcmp(n, "CallNextHookEx") || !strcmp(n, "GetKeyState") || !strcmp(n, "keybd_event") ||
             !strcmp(n, "GetClassLongA") || !strcmp(n, "SetClassLongA")) r = 0;
    else if (!strcmp(n, "GetKeyboardLayout")) r = 0x04090409u;   /* en-US */
    else if (!strcmp(n, "GetDoubleClickTime")) r = 500;
    else if (!strcmp(n, "GetCaretBlinkTime")) r = 530;
    else if (!strcmp(n, "GetWindowDC")) r = 0x00B30001u;
    else if (!strcmp(n, "SetTimer")) r = A(c, 1) ? A(c, 1) : 1;
    /* ---- GDI32 ---- */
    else if (!strcmp(n, "GetDeviceGammaRamp")){                 /* identity ramp, 3 x 256 WORDs */
        uint32_t p = A(c, 1);
        if (p){ mem_commit(p, 1536);
                for (int ch = 0; ch < 3; ch++) for (int i = 0; i < 256; i++)
                    MR16(p + (uint32_t)(ch * 512 + i * 2)) = (uint16_t)(i * 257); }
        r = 1;
    }
    else if (!strcmp(n, "SetDeviceGammaRamp")) r = 1;
    else if (!strcmp(n, "GetDeviceCaps")){            /* (hdc, index) */
        switch (A(c, 1)){
        case 8:   r = 640; break;    /* HORZRES */
        case 10:  r = 480; break;    /* VERTRES */
        case 12:  r = 32;  break;    /* BITSPIXEL */
        case 14:  r = 1;   break;    /* PLANES */
        case 88: case 90: r = 96; break;   /* LOGPIXELSX/Y */
        case 116: r = 60;  break;    /* VREFRESH */
        case 24:  r = 0x0400; break; /* RASTERCAPS: RC_PALETTE off, RC_DI_BITMAP-ish */
        default:  r = 0;
        }
    }
    /* ---- IMM32: no input method editor ---- */
    else if (!strncmp(n, "Imm", 3)) r = 0;
    /* ---- WINMM mixer / MCI: no CD audio, no mixer device ---- */
    else if (!strcmp(n, "mixerGetNumDevs")) r = 0;
    else if (!strncmp(n, "mixer", 5)) r = 2;                   /* MMSYSERR_BADDEVICEID */
    else if (!strcmp(n, "mciSendStringA")) r = 263;            /* MCIERR_INVALID_DEVICE_NAME */
    else return 0;
    if (!seen(n)) fprintf(stderr, "[hp2] %s -> 0x%x (from 0x%08x)\n", n, r, c->eip);
    *out = r;
    return 1;
}
