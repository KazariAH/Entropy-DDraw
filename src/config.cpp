// ddraw.ini loader. Only a handful of keys; everything has a sane default so
// the DLL also works without the ini file.
#include "qdd.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

Config g_cfg;

static bool read_bool(const char *ini, const char *key, bool def)
{
    char v[32];
    GetPrivateProfileStringA("ddraw", key, def ? "true" : "false", v, sizeof(v), ini);
    return v[0] == '1' || v[0] == 't' || v[0] == 'T' || v[0] == 'y' || v[0] == 'Y';
}

static int read_int(const char *ini, const char *key, int def)
{
    char v[32];
    GetPrivateProfileStringA("ddraw", key, "", v, sizeof(v), ini);
    if (!v[0])
        return def;
    return (int)strtol(v, NULL, 0);   // decimal or 0x.. (virtual key codes)
}

void config_load(HMODULE self)
{
    char ini[MAX_PATH];
    GetModuleFileNameA(self, ini, MAX_PATH);
    char *slash = strrchr(ini, '\\');
    if (slash) slash[1] = 0;
    else ini[0] = 0;
    strcat_s(ini, "ddraw.ini");

    g_cfg.windowed = read_bool(ini, "windowed", true);
    g_cfg.windowScale = read_int(ini, "windowscale", 0);
    g_cfg.linear = read_bool(ini, "linear", false);
    g_cfg.vsync = read_bool(ini, "vsync", false);
    g_cfg.maxfps = read_int(ini, "maxfps", 0);
    g_cfg.hookMouse = read_bool(ini, "hookmouse", true);
    g_cfg.resizable = read_bool(ini, "resizable", true);
    {   // Q162: only our template carries [ddraw] client=Entropy-DDraw. A game folder can still hold the ddraw.ini of
        // cnc-ddraw or another renderer (the client used to keep any existing file); its noactivateapp=false would make
        // QEC pause whenever the window loses the focus, so without the marker our default true stays.
        char c[32];
        GetPrivateProfileStringA("ddraw", "client", "", c, sizeof(c), ini);
        g_cfg.ownIni = _stricmp(c, "Entropy-DDraw") == 0;
        g_cfg.noActivateApp = read_bool(ini, "noactivateapp", true);   // Q147: like cnc-ddraw for RA2 / YR
        g_cfg.foreignNoActivate = !g_cfg.ownIni && !g_cfg.noActivateApp;
        if (g_cfg.foreignNoActivate)
            g_cfg.noActivateApp = true;
    }
    g_cfg.log = read_bool(ini, "log", true);
    g_cfg.pollPrimary = read_bool(ini, "pollprimary", true);
    g_cfg.dump = read_bool(ini, "dump", false);
    g_cfg.trace = read_bool(ini, "trace", false);
    g_cfg.pbo = read_bool(ini, "pbo", true);
    g_cfg.normalEmu = read_bool(ini, "normalmode", true);
    g_cfg.defaultBpp = read_int(ini, "bpp", 16);
    if (g_cfg.defaultBpp != 8 && g_cfg.defaultBpp != 16 && g_cfg.defaultBpp != 32)
        g_cfg.defaultBpp = 16;

    g_cfg.border = read_bool(ini, "border", true);
    g_cfg.fullscreen = read_bool(ini, "fullscreen", false);
    g_cfg.refreshRate = read_int(ini, "refresh_rate", 0);
    g_cfg.keyToggleFullscreen = read_int(ini, "keytogglefullscreen", VK_RETURN);
    g_cfg.keyUnlockCursor = read_int(ini, "keyunlockcursor", VK_TAB);
    g_cfg.saveSettings = read_bool(ini, "savesettings", true);
    g_cfg.centerCursorFix = read_bool(ini, "center_cursor_fix", true);
    g_cfg.mouseProbe = read_bool(ini, "mouseprobe", false);
    g_cfg.vsyncTest = read_bool(ini, "vsynctest", false);
    g_cfg.letterboxOffset = read_bool(ini, "letterboxoffset", true);
    g_cfg.fixAltKeyStuck = read_bool(ini, "fix_alt_key_stuck", false);
    g_cfg.fixNotResponding = read_bool(ini, "fix_not_responding", false);
    g_cfg.gameHandlesClose = read_bool(ini, "game_handles_close", false);
    g_cfg.flipClear = read_bool(ini, "flipclear", false);
    {   // fixchilds: 0 / 1 (cnc-ddraw style 2 = 1), true / false
        char f[16];
        GetPrivateProfileStringA("ddraw", "fixchilds", "", f, sizeof(f), ini);
        g_cfg.fixChilds = !f[0] ? 1 : (f[0] >= '0' && f[0] <= '9') ? (strtol(f, NULL, 0) != 0) : read_bool(ini, "fixchilds", true);
    }
    g_cfg.childPaint = read_bool(ini, "childpaint", true);
    g_cfg.childRepaint = read_int(ini, "childrepaint", 0);
    g_cfg.lockCursor = read_int(ini, "lockcursor", 1);
    g_cfg.keepClipChildren = read_bool(ini, "keepclipchildren", true);
    g_cfg.forceGlFail = read_bool(ini, "forceglfail", false);
    char v[32];
    GetPrivateProfileStringA("ddraw", "crashtest", "false", v, sizeof(v), ini);
    g_cfg.crashTest = _stricmp(v, "av") == 0 || strcmp(v, "2") == 0 ? 2 : read_bool(ini, "crashtest", false) ? 1 : 0;
    g_cfg.centerWindow = read_bool(ini, "center_window", false);
    g_cfg.removeMenu = read_bool(ini, "remove_menu", false);
    g_cfg.keyScreenshot = read_int(ini, "keyscreenshot", VK_F12);
    g_cfg.keyNextMonitor = read_int(ini, "keynextmonitor", 0);
    g_cfg.guardLines = read_bool(ini, "guard_lines", false);
    g_cfg.guardTest = read_bool(ini, "guardtest", false);
    g_cfg.maxGameTicks = read_int(ini, "maxgameticks", 0);
    if (g_cfg.maxGameTicks < 0) g_cfg.maxGameTicks = 0;
    g_cfg.stallTest = read_int(ini, "stalltest", 0);
    g_cfg.writeWatch = read_bool(ini, "writewatch", false);   // Q149: costs the game thread page faults, see Q149_REPORT
    g_cfg.presentQuiet = read_int(ini, "presentquiet", 2);
    g_cfg.presentCap = read_int(ini, "presentcap", 8);
    if (g_cfg.presentQuiet < 0) g_cfg.presentQuiet = 0;
    if (g_cfg.presentQuiet > 30) g_cfg.presentQuiet = 30;
    if (g_cfg.presentCap < g_cfg.presentQuiet) g_cfg.presentCap = g_cfg.presentQuiet;
    if (g_cfg.presentCap > 30) g_cfg.presentCap = 30;
    g_cfg.perfAB = read_int(ini, "perfab", 0);
    g_cfg.presentSync = read_bool(ini, "presentsync", false);   // Q163: off - coalescing made QEC stutter
    g_cfg.blitStats = read_bool(ini, "blitstats", false);
    g_cfg.fusedUpload = read_bool(ini, "fusedupload", true);
    g_cfg.renderCluster = read_bool(ini, "rendercluster", false);
    g_cfg.verifyTex = read_int(ini, "verifytex", 0);
    g_cfg.fastStretch = read_bool(ini, "faststretch", true);
    char bc[16];
    GetPrivateProfileStringA("ddraw", "blitcopy", "memmove", bc, sizeof(bc), ini);
    g_cfg.ntCopy = _stricmp(bc, "sse2nt") == 0;
    g_blitMode = (g_cfg.fastStretch ? BM_FASTSTRETCH : 0) | (g_cfg.ntCopy ? BM_NTCOPY : 0);

    // dpi: permonitor (default) / system / off; the older dpiaware=false means off
    GetPrivateProfileStringA("ddraw", "dpi", "", v, sizeof(v), ini);
    if (_stricmp(v, "system") == 0) g_cfg.dpi = DPI_SYSTEM;
    else if (_stricmp(v, "off") == 0 || _stricmp(v, "false") == 0 || v[0] == '0') g_cfg.dpi = DPI_OFF;
    else if (v[0]) g_cfg.dpi = DPI_PERMONITOR;
    else g_cfg.dpi = read_bool(ini, "dpiaware", true) ? DPI_PERMONITOR : DPI_OFF;

    // monitortest=WxH: a simulated second monitor (code path tests of the multi-monitor handling)
    GetPrivateProfileStringA("ddraw", "monitortest", "", v, sizeof(v), ini);
    if (sscanf_s(v, "%dx%d", &g_cfg.monitorTestW, &g_cfg.monitorTestH) != 2 || g_cfg.monitorTestW < 320 ||
        g_cfg.monitorTestH < 200)
        g_cfg.monitorTestW = g_cfg.monitorTestH = 0;

    GetPrivateProfileStringA("ddraw", "resolutions", "", g_cfg.resolutions, sizeof(g_cfg.resolutions), ini);
    // max_resolutions: a count (the largest N sizes) or a size (WxH: nothing bigger)
    GetPrivateProfileStringA("ddraw", "max_resolutions", "", v, sizeof(v), ini);
    if (sscanf_s(v, "%dx%d", &g_cfg.maxResW, &g_cfg.maxResH) != 2) {
        g_cfg.maxResW = g_cfg.maxResH = 0;
        g_cfg.maxResolutions = atoi(v) > 0 ? atoi(v) : 0;
    }

    GetPrivateProfileStringA("ddraw", "renderer", "auto", v, sizeof(v), ini);
    if (_stricmp(v, "gdi") == 0) g_cfg.renderer = RENDERER_GDI;
    else if (_strnicmp(v, "opengl", 6) == 0 || _stricmp(v, "gl") == 0) g_cfg.renderer = RENDERER_OPENGL;
    else g_cfg.renderer = RENDERER_AUTO;

    // crashdump: false / 0 = off, true / 1 / normal = stacks, data segments, handles and the memory the
    // registers / stacks point at (a few MB), full = the whole address space (size of the process)
    GetPrivateProfileStringA("ddraw", "crashdump", "true", v, sizeof(v), ini);
    if (_stricmp(v, "full") == 0) g_cfg.crashDump = CRASHDUMP_FULL;
    else if (v[0] == '0' || _stricmp(v, "false") == 0 || _stricmp(v, "no") == 0 || _stricmp(v, "off") == 0)
        g_cfg.crashDump = CRASHDUMP_OFF;
    else g_cfg.crashDump = CRASHDUMP_NORMAL;

    // adjmouse is a speed factor; cnc-ddraw's boolean values ("true" / "false") mean the neutral 1.0
    GetPrivateProfileStringA("ddraw", "adjmouse", "1.0", v, sizeof(v), ini);
    double f = (v[0] >= '0' && v[0] <= '9') || v[0] == '.' ? atof(v) : 1.0;
    if (f < 0.1) f = 0.1;
    if (f > 10.0) f = 10.0;
    g_cfg.adjMouse = (float)f;

    GetPrivateProfileStringA("ddraw", "scale", "aspect", v, sizeof(v), ini);
    if (_stricmp(v, "stretch") == 0) g_cfg.scale = SCALE_STRETCH;
    else if (_stricmp(v, "integer") == 0) g_cfg.scale = SCALE_INTEGER;
    else g_cfg.scale = SCALE_ASPECT;
}
