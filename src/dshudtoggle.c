// DSHudToggle - hotkey HUD toggle for Death Stranding Director's Cut (Steam ds.exe, build dsq 179).
//
// Loaded by Ultimate ASI Loader (winhttp.dll in the game folder). Hides/shows the gameplay HUD by
// flipping a few bytes of the game's own HUD code in memory. No graphics API hooks, no input hooks,
// nothing written to disk except DSHudToggle.log next to the .asi.
//
// Modes (DSHudToggle.ini):
//   draw  Skip the two HUD draw calls (je -> jmp, 1 byte each). The HUD keeps updating.
//   gate  Force the engine's "draw HUD?" check to false (cmovne -> 4-byte NOP): the path of the
//         unused Horizon Zero Dawn "DrawHUD = Off" setting. Also pauses HUD logic while hidden.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define DSHT_VERSION "1.1.0"

// ------------------------------------------------------------------------------------ log

static HANDLE g_log = INVALID_HANDLE_VALUE;

static void logf_(const char *fmt, ...)
{
    if (g_log == INVALID_HANDLE_VALUE)
        return;
    char buf[600];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int n = snprintf(buf, sizeof buf, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(buf + n, sizeof buf - n, fmt, ap);
    va_end(ap);
    n = (m < 0) ? n : (n + m > (int)sizeof buf - 3 ? (int)sizeof buf - 3 : n + m);
    buf[n++] = '\r';
    buf[n++] = '\n';
    DWORD written;
    WriteFile(g_log, buf, (DWORD)n, &written, NULL);
}

// ------------------------------------------------------------------------------- patch sites

typedef struct {
    const char *name;
    const char *aob;        // "??" = any byte; the patched bytes are wildcards too
    int patch_off;          // offset of the patched bytes from the pattern start
    int len;                // 1 or 4
    uint8_t on[4];          // original bytes (HUD shown)
    uint8_t off[4];         // patched bytes (HUD hidden)
    int call_off;           // offset of an E8 call to the gate function, -1 = none
    uint8_t *addr;          // resolved patch address
} Site;

// Byte patterns checked unique in ds.exe dsq 179. The gate must stay first.
static Site g_sites[] = {
    {"gate", "48 8B 05 ?? ?? ?? ?? 45 33 C0 48 8B 90 78 01 00 00 41 0F B6 C1 80 3A 02 ?? ?? ?? ??",
     0x18, 4, {0x44, 0x0F, 0x45, 0xC0}, {0x0F, 0x1F, 0x40, 0x00}, -1, NULL},   // cmovne r8d,eax / nop
    {"draw1", "48 83 B9 10 06 00 00 00 49 8B F8 C5 F8 29 74 24 20 48 8B F2 C5 F8 28 F3 48 8B D9 74 ?? E8 ?? ?? ?? ?? 84 C0 ??",
     36, 1, {0x74}, {0xEB}, 29, NULL},                                             // je / jmp
    {"draw2", "48 83 B9 10 06 00 00 00 49 8B F9 C5 F8 29 74 24 20 48 8B F2 C5 F8 28 F2 48 8B D9 74 ?? E8 ?? ?? ?? ?? 84 C0 ??",
     36, 1, {0x74}, {0xEB}, 29, NULL},
};
#define NSITES ((int)(sizeof g_sites / sizeof g_sites[0]))
#define GATE_FUNC_FROM_AOB (-0x40)      // the gate function starts 0x40 bytes before its pattern

static uint8_t *g_base;
static CRITICAL_SECTION g_lock;
static volatile LONG g_ready;           // 1 = sites resolved and checked
static volatile LONG g_broken;          // 1 = a site changed under us, toggling stopped
static int g_hidden;
static int g_use_gate;

static int parse_aob(const char *s, uint8_t *bytes, uint8_t *mask, int max)
{
    int n = 0;
    while (*s) {
        while (*s == ' ')
            s++;
        if (!*s)
            break;
        if (n >= max)
            return -1;
        if (s[0] == '?') {
            bytes[n] = 0;
            mask[n] = 0;
            s += (s[1] == '?') ? 2 : 1;
        } else {
            unsigned v;
            if (sscanf(s, "%2x", &v) != 1)
                return -1;
            bytes[n] = (uint8_t)v;
            mask[n] = 1;
            s += 2;
        }
        n++;
    }
    return n;
}

// Counts matches of a pattern (first byte must not be a wildcard); *first = first match.
static int scan(const uint8_t *p, size_t size, const uint8_t *pat, const uint8_t *mask, int n, const uint8_t **first)
{
    int count = 0;
    if (n <= 0 || size < (size_t)n)
        return 0;
    for (size_t i = 0; i + (size_t)n <= size; i++) {
        if (p[i] != pat[0])
            continue;
        int j = 1;
        while (j < n && (!mask[j] || p[i + j] == pat[j]))
            j++;
        if (j == n) {
            if (count++ == 0)
                *first = p + i;
        }
    }
    return count;
}

static int find_text(uint8_t **start, size_t *size)
{
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)g_base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return 0;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(g_base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return 0;
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        if (memcmp(sec->Name, ".text", 6) == 0) {
            *start = g_base + sec->VirtualAddress;
            *size = sec->Misc.VirtualSize;
            return 1;
        }
    }
    return 0;
}

// The whole range must be committed and readable before we scan it.
static int readable(const uint8_t *p, size_t size)
{
    const uint8_t *end = p + size;
    while (p < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(p, &mbi, sizeof mbi))
            return 0;
        DWORD prot = mbi.Protect & 0xFF;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) || prot == PAGE_NOACCESS || prot == PAGE_EXECUTE)
            return 0;
        p = (const uint8_t *)mbi.BaseAddress + mbi.RegionSize;
    }
    return 1;
}

static int resolve_sites(void)
{
    uint8_t *text;
    size_t size;
    if (!find_text(&text, &size)) {
        logf_("No .text section found in the game module.");
        return 0;
    }
    if (!readable(text, size)) {
        logf_("The game's code section is not fully readable.");
        return 0;
    }
    const uint8_t *gate_func = NULL;
    for (int k = 0; k < NSITES; k++) {
        Site *s = &g_sites[k];
        uint8_t pat[64], mask[64];
        int n = parse_aob(s->aob, pat, mask, 64);
        const uint8_t *hit = NULL;
        int count = scan(text, size, pat, mask, n, &hit);
        if (count != 1) {
            logf_("%s: pattern found %d times (need exactly 1). Game version not supported?", s->name, count);
            return 0;
        }
        s->addr = (uint8_t *)hit + s->patch_off;
        if (k == 0)
            gate_func = hit + GATE_FUNC_FROM_AOB;
        if (s->call_off >= 0) {
            const uint8_t *call = hit + s->call_off;
            int32_t rel;
            memcpy(&rel, call + 1, 4);
            if (call[0] != 0xE8 || call + 5 + rel != gate_func) {
                logf_("%s: does not call the HUD gate function.", s->name);
                return 0;
            }
        }
        logf_("%-5s site: ds.exe+0x%llX", s->name, (unsigned long long)(s->addr - g_base));
    }
    return 1;
}

// 0 = original (HUD shown), 1 = patched (HUD hidden), -1 = unexpected bytes
static int site_state(const Site *s)
{
    if (memcmp(s->addr, s->on, (size_t)s->len) == 0)
        return 0;
    if (memcmp(s->addr, s->off, (size_t)s->len) == 0)
        return 1;
    return -1;
}

static int write_code(uint8_t *addr, const uint8_t *bytes, int len)
{
    DWORD old, tmp;
    if (!VirtualProtect(addr, (SIZE_T)len, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    if (len == 1) {
        __atomic_exchange_n((volatile uint8_t *)addr, bytes[0], __ATOMIC_SEQ_CST);
    } else if (len == 4 && ((uintptr_t)addr & 3) == 0) {
        uint32_t v;
        memcpy(&v, bytes, 4);
        __atomic_exchange_n((volatile uint32_t *)addr, v, __ATOMIC_SEQ_CST);   // whole instruction at once
    } else {
        memcpy(addr, bytes, (size_t)len);
    }
    VirtualProtect(addr, (SIZE_T)len, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), addr, (SIZE_T)len);
    return 1;
}

// Sites of the inactive mode are always kept original.
static int set_hidden_locked(int hidden)
{
    for (int k = 0; k < NSITES; k++) {
        Site *s = &g_sites[k];
        int active = g_use_gate ? (k == 0) : (k != 0);
        int want = (hidden && active) ? 1 : 0;
        int st = site_state(s);
        if (st < 0) {
            logf_("%s changed by something else; HUD toggle stopped.", s->name);
            InterlockedExchange(&g_broken, 1);
            return 0;
        }
        if (st != want && !write_code(s->addr, want ? s->off : s->on, s->len)) {
            logf_("%s: VirtualProtect failed (error %lu).", s->name, GetLastError());
            return 0;
        }
    }
    g_hidden = hidden;
    return 1;
}

static int set_hidden(int hidden)
{
    if (!g_ready || g_broken)
        return 0;
    EnterCriticalSection(&g_lock);
    int ok = set_hidden_locked(hidden);
    LeaveCriticalSection(&g_lock);
    return ok;
}

// For tests and other mods: 1 = done, 0 = not ready / failed.
__declspec(dllexport) int DSHudToggle_SetHidden(int hidden)
{
    int ok = set_hidden(hidden ? 1 : 0);
    if (ok)
        logf_("HUD %s (API)", hidden ? "hidden" : "shown");
    return ok;
}

// 1 = hidden, 0 = shown, -1 = not active
__declspec(dllexport) int DSHudToggle_IsHidden(void)
{
    return (g_ready && !g_broken) ? g_hidden : -1;
}

// ---------------------------------------------------------------------------------- config

typedef struct {
    int vk, ctrl, alt, shift;
} KeyBind;

static const struct {
    const wchar_t *name;
    int vk;
} kKeyNames[] = {
    {L"DELETE", 0x2E}, {L"DEL", 0x2E}, {L"INSERT", 0x2D}, {L"HOME", 0x24}, {L"END", 0x23},
    {L"PAGEUP", 0x21}, {L"PAGEDOWN", 0x22}, {L"PAUSE", 0x13}, {L"SCROLLLOCK", 0x91},
    {L"BACKSPACE", 0x08}, {L"TAB", 0x09}, {L"CAPSLOCK", 0x14}, {L"SPACE", 0x20},
    {L"MOUSE3", 0x04}, {L"MOUSE4", 0x05}, {L"MOUSE5", 0x06},
    {L"NUMPADMULTIPLY", 0x6A}, {L"NUMPADADD", 0x6B}, {L"NUMPADSUBTRACT", 0x6D},
    {L"NUMPADDECIMAL", 0x6E}, {L"NUMPADDIVIDE", 0x6F},
};

#define PAD_LT 0x10000u
#define PAD_RT 0x20000u
#define PAD_PS 0x40000u         // PlayStation controllers only (read through HID)
#define PAD_TOUCHPAD 0x80000u
#define PAD_MUTE 0x100000u      // DualSense mic button
static const struct {
    const wchar_t *name;
    unsigned bits;
} kPadNames[] = {
    {L"DPAD_UP", 0x0001}, {L"DPAD_DOWN", 0x0002}, {L"DPAD_LEFT", 0x0004}, {L"DPAD_RIGHT", 0x0008},
    {L"START", 0x0010}, {L"MENU", 0x0010}, {L"OPTIONS", 0x0010},
    {L"BACK", 0x0020}, {L"VIEW", 0x0020}, {L"SELECT", 0x0020}, {L"SHARE", 0x0020},
    {L"L3", 0x0040}, {L"LS", 0x0040}, {L"R3", 0x0080}, {L"RS", 0x0080},
    {L"LB", 0x0100}, {L"L1", 0x0100}, {L"RB", 0x0200}, {L"R1", 0x0200},
    {L"A", 0x1000}, {L"CROSS", 0x1000}, {L"B", 0x2000}, {L"CIRCLE", 0x2000},
    {L"X", 0x4000}, {L"SQUARE", 0x4000}, {L"Y", 0x8000}, {L"TRIANGLE", 0x8000},
    {L"LT", PAD_LT}, {L"L2", PAD_LT}, {L"RT", PAD_RT}, {L"R2", PAD_RT},
    {L"CREATE", 0x0020}, {L"PS", PAD_PS}, {L"TOUCHPAD", PAD_TOUCHPAD}, {L"MUTE", PAD_MUTE},
};

static void upper_trim(wchar_t *s)
{
    wchar_t *src = s, *dst = s;
    for (; *src; src++)
        if (*src != L' ' && *src != L'\t')
            *dst++ = (*src >= L'a' && *src <= L'z') ? (wchar_t)(*src - 32) : *src;
    *dst = 0;
}

// Next '+'-separated token of *p (split in place), NULL at the end.
static wchar_t *next_token(wchar_t **p)
{
    wchar_t *s = *p;
    if (!*s)
        return NULL;
    wchar_t *plus = wcschr(s, L'+');
    if (plus) {
        *plus = 0;
        *p = plus + 1;
    } else {
        *p = s + wcslen(s);
    }
    return s;
}

static int key_from_name(const wchar_t *t)
{
    for (size_t i = 0; i < sizeof kKeyNames / sizeof kKeyNames[0]; i++)
        if (wcscmp(t, kKeyNames[i].name) == 0)
            return kKeyNames[i].vk;
    if (t[0] == L'F' && t[1] >= L'1' && t[1] <= L'9') {
        int f = _wtoi(t + 1);
        if (f >= 1 && f <= 24)
            return 0x6F + f;
    }
    if (wcsncmp(t, L"NUMPAD", 6) == 0 && t[6] >= L'0' && t[6] <= L'9' && !t[7])
        return 0x60 + (t[6] - L'0');
    if (t[0] && !t[1] && ((t[0] >= L'A' && t[0] <= L'Z') || (t[0] >= L'0' && t[0] <= L'9')))
        return t[0];
    if (t[0] >= L'0' && t[0] <= L'9') {
        long v = wcstol(t, NULL, 0);
        if (v > 0 && v < 256)
            return (int)v;
    }
    return 0;
}

// "DELETE", "F10", "CTRL+H", "0x2E"
static int parse_key(const wchar_t *in, KeyBind *kb)
{
    wchar_t buf[128];
    wcsncpy(buf, in, 127);
    buf[127] = 0;
    upper_trim(buf);
    memset(kb, 0, sizeof *kb);
    wchar_t *cur = buf, *tok;
    while ((tok = next_token(&cur))) {
        if (!wcscmp(tok, L"CTRL") || !wcscmp(tok, L"CONTROL"))
            kb->ctrl = 1;
        else if (!wcscmp(tok, L"ALT"))
            kb->alt = 1;
        else if (!wcscmp(tok, L"SHIFT"))
            kb->shift = 1;
        else if (!(kb->vk = key_from_name(tok)))
            return 0;
    }
    return kb->vk != 0;
}

// "L3+R3", "BACK+DPAD_DOWN"; empty / NONE / 0 = off
static unsigned parse_pad(const wchar_t *in)
{
    wchar_t buf[128];
    wcsncpy(buf, in, 127);
    buf[127] = 0;
    upper_trim(buf);
    if (!buf[0] || !wcscmp(buf, L"NONE") || !wcscmp(buf, L"OFF") || !wcscmp(buf, L"0"))
        return 0;
    unsigned mask = 0;
    wchar_t *cur = buf, *tok;
    while ((tok = next_token(&cur))) {
        unsigned bits = 0;
        for (size_t i = 0; i < sizeof kPadNames / sizeof kPadNames[0]; i++)
            if (!wcscmp(tok, kPadNames[i].name))
                bits = kPadNames[i].bits;
        if (!bits)
            return 0xFFFFFFFFu;     // unknown name
        mask |= bits;
    }
    return mask;
}

// --------------------------------------------------------------------------------- input

typedef struct {
    WORD wButtons;
    BYTE bLeftTrigger, bRightTrigger;
    SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY;
} XPad;
typedef struct {
    DWORD dwPacketNumber;
    XPad Gamepad;
} XState;
typedef DWORD(WINAPI *PFN_XInputGetState)(DWORD, XState *);

// ------------------------------------------------------------- PlayStation controllers (HID)
// The game reads DualSense/DS4 controllers through HID itself (with OptiScaler FG, Steam Input is
// blocked), so XInput doesn't see them. We open the controller read-only and shared: every open HID
// handle gets its own copy of each input report, so this takes nothing away from the game. We never
// write to the controller, and we only open it after the game had time to open it first.

typedef struct {
    ULONG Size;
    USHORT VendorID, ProductID, VersionNumber;
} HidAttrs;
typedef struct {
    USHORT Usage, UsagePage, InputReportByteLength, OutputReportByteLength, FeatureReportByteLength;
    USHORT Reserved[17], Rest[10];
} HidCaps;
typedef void(WINAPI *PFN_HidD_GetHidGuid)(GUID *);
typedef BOOLEAN(WINAPI *PFN_HidD_GetAttributes)(HANDLE, HidAttrs *);
typedef BOOLEAN(WINAPI *PFN_HidD_GetPreparsedData)(HANDLE, void **);
typedef BOOLEAN(WINAPI *PFN_HidD_FreePreparsedData)(void *);
typedef LONG(WINAPI *PFN_HidP_GetCaps)(void *, HidCaps *);
typedef HDEVINFO(WINAPI *PFN_SetupDiGetClassDevsW)(const GUID *, PCWSTR, HWND, DWORD);
typedef BOOL(WINAPI *PFN_SetupDiEnumDeviceInterfaces)(HDEVINFO, PSP_DEVINFO_DATA, const GUID *, DWORD,
                                                      PSP_DEVICE_INTERFACE_DATA);
typedef BOOL(WINAPI *PFN_SetupDiGetDeviceInterfaceDetailW)(HDEVINFO, PSP_DEVICE_INTERFACE_DATA,
                                                           PSP_DEVICE_INTERFACE_DETAIL_DATA_W, DWORD, PDWORD,
                                                           PSP_DEVINFO_DATA);
typedef BOOL(WINAPI *PFN_SetupDiDestroyDeviceInfoList)(HDEVINFO);

static struct {
    PFN_HidD_GetHidGuid GetHidGuid;
    PFN_HidD_GetAttributes GetAttributes;
    PFN_HidD_GetPreparsedData GetPreparsedData;
    PFN_HidD_FreePreparsedData FreePreparsedData;
    PFN_HidP_GetCaps GetCaps;
    PFN_SetupDiGetClassDevsW GetClassDevs;
    PFN_SetupDiEnumDeviceInterfaces EnumInterfaces;
    PFN_SetupDiGetDeviceInterfaceDetailW GetInterfaceDetail;
    PFN_SetupDiDestroyDeviceInfoList DestroyList;
} H;

#define HIDP_STATUS_OK 0x00110000
#define PS_MAX 4
#define PS_REPORT_MAX 600

typedef struct {
    volatile LONG open;         // 1 while the reader thread owns the handle
    volatile LONG buttons;      // current buttons as pad bits
    HANDLE h;
    USHORT pid, len;
    int bt, ds5;
    wchar_t path[300];
} PsDev;
static PsDev g_ps[PS_MAX];

typedef struct {
    wchar_t path[300];
    DWORD first_seen, retry_at;
} PsSeen;
static PsSeen g_seen[8];

static int ps_load(void)
{
    HMODULE hid = LoadLibraryW(L"hid.dll"), sapi = LoadLibraryW(L"setupapi.dll");
    if (!hid || !sapi)
        return 0;
    H.GetHidGuid = (PFN_HidD_GetHidGuid)(void *)GetProcAddress(hid, "HidD_GetHidGuid");
    H.GetAttributes = (PFN_HidD_GetAttributes)(void *)GetProcAddress(hid, "HidD_GetAttributes");
    H.GetPreparsedData = (PFN_HidD_GetPreparsedData)(void *)GetProcAddress(hid, "HidD_GetPreparsedData");
    H.FreePreparsedData = (PFN_HidD_FreePreparsedData)(void *)GetProcAddress(hid, "HidD_FreePreparsedData");
    H.GetCaps = (PFN_HidP_GetCaps)(void *)GetProcAddress(hid, "HidP_GetCaps");
    H.GetClassDevs = (PFN_SetupDiGetClassDevsW)(void *)GetProcAddress(sapi, "SetupDiGetClassDevsW");
    H.EnumInterfaces = (PFN_SetupDiEnumDeviceInterfaces)(void *)GetProcAddress(sapi, "SetupDiEnumDeviceInterfaces");
    H.GetInterfaceDetail =
        (PFN_SetupDiGetDeviceInterfaceDetailW)(void *)GetProcAddress(sapi, "SetupDiGetDeviceInterfaceDetailW");
    H.DestroyList = (PFN_SetupDiDestroyDeviceInfoList)(void *)GetProcAddress(sapi, "SetupDiDestroyDeviceInfoList");
    return H.GetHidGuid && H.GetAttributes && H.GetPreparsedData && H.FreePreparsedData && H.GetCaps &&
           H.GetClassDevs && H.EnumInterfaces && H.GetInterfaceDetail && H.DestroyList;
}

static int ps_supported(USHORT pid, int *ds5)
{
    *ds5 = (pid == 0x0CE6 || pid == 0x0DF2);            // DualSense, DualSense Edge
    return *ds5 || pid == 0x05C4 || pid == 0x09CC;      // DualShock 4 v1 / v2
}

// Buttons block of a report (hat+face, shoulders/sticks, PS/touchpad/mute) -> pad bits.
static unsigned ps_map(const uint8_t *b, int mute_ok)
{
    static const unsigned hat[8] = {0x1, 0x1 | 0x8, 0x8, 0x8 | 0x2, 0x2, 0x2 | 0x4, 0x4, 0x4 | 0x1};
    unsigned bits = 0, h = b[0] & 0x0F;
    if (h < 8)
        bits |= hat[h];
    if (b[0] & 0x10) bits |= 0x4000;    // square   -> X
    if (b[0] & 0x20) bits |= 0x1000;    // cross    -> A
    if (b[0] & 0x40) bits |= 0x2000;    // circle   -> B
    if (b[0] & 0x80) bits |= 0x8000;    // triangle -> Y
    if (b[1] & 0x01) bits |= 0x0100;    // L1
    if (b[1] & 0x02) bits |= 0x0200;    // R1
    if (b[1] & 0x04) bits |= PAD_LT;    // L2
    if (b[1] & 0x08) bits |= PAD_RT;    // R2
    if (b[1] & 0x10) bits |= 0x0020;    // Create / Share -> BACK
    if (b[1] & 0x20) bits |= 0x0010;    // Options -> START
    if (b[1] & 0x40) bits |= 0x0040;    // L3
    if (b[1] & 0x80) bits |= 0x0080;    // R3
    if (b[2] & 0x01) bits |= PAD_PS;
    if (b[2] & 0x02) bits |= PAD_TOUCHPAD;
    if (mute_ok && (b[2] & 0x04))       // in the other layouts bit 2+ is a frame counter
        bits |= PAD_MUTE;
    return bits;
}

// Returns 1 and the pad bits for a report we understand.
static int ps_parse(int ds5, int bt, const uint8_t *r, DWORD n, unsigned *out)
{
    if (ds5) {
        if (!bt && r[0] == 0x01 && n >= 11) { *out = ps_map(r + 8, 1); return 1; }    // USB
        if (bt && r[0] == 0x31 && n >= 12) { *out = ps_map(r + 9, 1); return 1; }     // Bluetooth, full
        if (bt && r[0] == 0x01 && n >= 8) { *out = ps_map(r + 5, 0); return 1; }      // Bluetooth, simple
    } else {
        if (r[0] == 0x01 && n >= 8) { *out = ps_map(r + 5, 0); return 1; }            // USB / BT simple
        if (bt && r[0] == 0x11 && n >= 10) { *out = ps_map(r + 7, 0); return 1; }     // Bluetooth, full
    }
    return 0;
}

static DWORD WINAPI ps_reader(LPVOID arg)
{
    PsDev *d = (PsDev *)arg;
    uint8_t buf[PS_REPORT_MAX];
    int first = 1;
    DWORD n = 0;
    while (ReadFile(d->h, buf, d->len, &n, NULL) && n) {
        unsigned bits;
        if (first) {
            logf_("First controller report: id 0x%02X, %lu bytes.", buf[0], n);
            first = 0;
        }
        if (ps_parse(d->ds5, d->bt, buf, n, &bits))
            InterlockedExchange(&d->buttons, (LONG)bits);
    }
    DWORD err = GetLastError();
    InterlockedExchange(&d->buttons, 0);
    CloseHandle(d->h);
    d->h = NULL;
    logf_("PlayStation controller disconnected (error %lu).", err);
    InterlockedExchange(&d->open, 0);
    return 0;
}

static int ps_is_open(const wchar_t *path)
{
    for (int i = 0; i < PS_MAX; i++)
        if (g_ps[i].open && !_wcsicmp(g_ps[i].path, path))
            return 1;
    return 0;
}

// Wait 5 s after a controller shows up so the game opens it first; after a failed open wait 30 s.
static int ps_ready_to_open(const wchar_t *path, DWORD now)
{
    int free_slot = -1, oldest = 0;
    for (int i = 0; i < 8; i++) {
        if (g_seen[i].path[0] && !_wcsicmp(g_seen[i].path, path))
            return (LONG)(now - g_seen[i].first_seen) >= 5000 && (LONG)(now - g_seen[i].retry_at) >= 0;
        if (!g_seen[i].path[0] && free_slot < 0)
            free_slot = i;
        if ((LONG)(g_seen[i].first_seen - g_seen[oldest].first_seen) < 0)
            oldest = i;
    }
    PsSeen *s = &g_seen[free_slot >= 0 ? free_slot : oldest];
    wcsncpy(s->path, path, 299);
    s->path[299] = 0;
    s->first_seen = s->retry_at = now;
    return 0;
}

static void ps_retry_later(const wchar_t *path, DWORD now)
{
    for (int i = 0; i < 8; i++)
        if (!_wcsicmp(g_seen[i].path, path))
            g_seen[i].retry_at = now + 30000;
}

static void ps_open(const wchar_t *path, USHORT pid, int ds5, DWORD now)
{
    int slot = -1;
    for (int i = 0; i < PS_MAX && slot < 0; i++)
        if (!g_ps[i].open)
            slot = i;
    if (slot < 0)
        return;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        logf_("PlayStation controller %04X found but could not be opened (error %lu); retrying in 30 s.", pid,
              GetLastError());
        ps_retry_later(path, now);
        return;
    }
    void *pp = NULL;
    HidCaps caps;
    memset(&caps, 0, sizeof caps);
    int ok = H.GetPreparsedData(h, &pp) && H.GetCaps(pp, &caps) == HIDP_STATUS_OK;
    if (pp)
        H.FreePreparsedData(pp);
    if (!ok || !caps.InputReportByteLength || caps.InputReportByteLength > PS_REPORT_MAX) {
        CloseHandle(h);
        ps_retry_later(path, now);
        return;
    }
    PsDev *d = &g_ps[slot];
    d->h = h;
    d->pid = pid;
    d->ds5 = ds5;
    d->len = caps.InputReportByteLength;
    d->bt = caps.InputReportByteLength > 64;          // USB reports are 64 bytes, Bluetooth 78+
    d->buttons = 0;
    wcsncpy(d->path, path, 299);
    d->path[299] = 0;
    InterlockedExchange(&d->open, 1);
    HANDLE t = CreateThread(NULL, 0, ps_reader, d, 0, NULL);
    if (!t) {
        CloseHandle(h);
        InterlockedExchange(&d->open, 0);
        return;
    }
    CloseHandle(t);
    logf_("PlayStation controller connected: %s over %s (%u-byte reports).",
          ds5 ? (pid == 0x0DF2 ? "DualSense Edge" : "DualSense") : "DualShock 4", d->bt ? "Bluetooth" : "USB",
          (unsigned)d->len);
}

static void ps_scan(void)
{
    GUID g;
    H.GetHidGuid(&g);
    HDEVINFO info = H.GetClassDevs(&g, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (info == INVALID_HANDLE_VALUE)
        return;
    DWORD now = GetTickCount();
    for (DWORD i = 0;; i++) {
        SP_DEVICE_INTERFACE_DATA did;
        memset(&did, 0, sizeof did);
        did.cbSize = sizeof did;
        if (!H.EnumInterfaces(info, NULL, &g, i, &did))
            break;
        union {
            SP_DEVICE_INTERFACE_DETAIL_DATA_W d;
            BYTE raw[sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) + 600];
        } u;
        u.d.cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!H.GetInterfaceDetail(info, &did, &u.d, sizeof u, NULL, NULL))
            continue;
        const wchar_t *path = u.d.DevicePath;
        if (wcslen(path) >= 300 || ps_is_open(path))
            continue;
        HANDLE h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        HidAttrs a;
        memset(&a, 0, sizeof a);
        a.Size = sizeof a;
        BOOLEAN got = H.GetAttributes(h, &a);
        CloseHandle(h);
        int ds5;
        if (got && a.VendorID == 0x054C && ps_supported(a.ProductID, &ds5) && ps_ready_to_open(path, now))
            ps_open(path, a.ProductID, ds5, now);
    }
    H.DestroyList(info);
}

static int game_in_foreground(void)
{
    HWND w = GetForegroundWindow();
    DWORD pid = 0;
    if (w)
        GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}

static int keybind_down(const KeyBind *kb)
{
    if (!kb->vk || !(GetAsyncKeyState(kb->vk) & 0x8000))
        return 0;
    if (kb->ctrl && !(GetAsyncKeyState(VK_CONTROL) & 0x8000))
        return 0;
    if (kb->alt && !(GetAsyncKeyState(VK_MENU) & 0x8000))
        return 0;
    if (kb->shift && !(GetAsyncKeyState(VK_SHIFT) & 0x8000))
        return 0;
    return 1;
}

// ---------------------------------------------------------------------------------- main

static HMODULE g_self;

static DWORD WINAPI main_thread(LPVOID unused)
{
    (void)unused;
    wchar_t path[MAX_PATH], ini[MAX_PATH + 8], logpath[MAX_PATH + 8];
    DWORD len = GetModuleFileNameW(g_self, path, MAX_PATH);
    if (!len || len >= MAX_PATH)
        return 0;
    wchar_t *dot = wcsrchr(path, L'.');
    if (dot)
        *dot = 0;
    swprintf(ini, MAX_PATH + 8, L"%ls.ini", path);
    swprintf(logpath, MAX_PATH + 8, L"%ls.log", path);

    if (GetPrivateProfileIntW(L"Settings", L"Log", 1, ini))
        g_log = CreateFileW(logpath, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    logf_("DSHudToggle " DSHT_VERSION " - HUD hotkey for Death Stranding Director's Cut");

    wchar_t mode[32], keytxt[128], padtxt[128];
    GetPrivateProfileStringW(L"Settings", L"Mode", L"draw", mode, 32, ini);
    GetPrivateProfileStringW(L"Settings", L"Key", L"DELETE", keytxt, 128, ini);
    GetPrivateProfileStringW(L"Settings", L"Controller", L"L3+R3", padtxt, 128, ini);
    int start_hidden = (int)GetPrivateProfileIntW(L"Settings", L"StartHidden", 0, ini);
    upper_trim(mode);
    g_use_gate = !wcscmp(mode, L"GATE");

    KeyBind kb;
    if (!parse_key(keytxt, &kb)) {
        logf_("Key '%ls' not understood, using DELETE.", keytxt);
        parse_key(L"DELETE", &kb);
    }
    unsigned pad_mask = parse_pad(padtxt);
    if (pad_mask == 0xFFFFFFFFu) {
        logf_("Controller '%ls' not understood, using L3+R3.", padtxt);
        pad_mask = parse_pad(L"L3+R3");
    }
    logf_("Mode %s, key VK 0x%02X%s%s%s, controller %ls", g_use_gate ? "gate" : "draw", kb.vk,
          kb.ctrl ? " +Ctrl" : "", kb.alt ? " +Alt" : "", kb.shift ? " +Shift" : "", pad_mask ? padtxt : L"off");

    g_base = (uint8_t *)GetModuleHandleW(NULL);
    if (!resolve_sites()) {
        logf_("HUD toggle disabled.");
        return 0;
    }
    for (int k = 0; k < NSITES; k++) {
        int st = site_state(&g_sites[k]);
        if (st < 0) {
            logf_("%s has unexpected bytes; another mod may patch this spot. HUD toggle disabled.", g_sites[k].name);
            return 0;
        }
        if (st == 1)
            logf_("%s was already patched; restoring it.", g_sites[k].name);
    }
    InterlockedExchange(&g_ready, 1);
    set_hidden(0);                  // all sites original
    if (start_hidden && set_hidden(1))
        logf_("HUD hidden (StartHidden)");
    logf_("Ready.");

    PFN_XInputGetState xget = NULL;
    if (pad_mask) {
        static const wchar_t *dlls[] = {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"};
        for (int i = 0; i < 3 && !xget; i++) {
            HMODULE h = LoadLibraryW(dlls[i]);
            if (h)
                xget = (PFN_XInputGetState)(void *)GetProcAddress(h, "XInputGetState");
        }
        if (!xget)
            logf_("XInput not available (Xbox-style controllers).");
    }
    int ps_ok = pad_mask && ps_load();
    if (pad_mask && !ps_ok)
        logf_("HID not available; PlayStation controllers off.");
    DWORD ps_next_scan = GetTickCount() + 10000;    // give the game time to open its controller first

    int prev_key = 1, prev_pad = 1;     // ignore anything already held at start
    int connected[4] = {0};
    DWORD retry_at[4] = {0};
    for (;;) {
        Sleep(15);
        if (g_broken)
            return 0;
        int key = keybind_down(&kb);
        int pad = 0;
        if (ps_ok) {
            DWORD now = GetTickCount();
            int any_open = 0;
            for (int i = 0; i < PS_MAX; i++) {
                if (!g_ps[i].open)
                    continue;
                any_open = 1;
                if (((unsigned)g_ps[i].buttons & pad_mask) == pad_mask)
                    pad = 1;
            }
            if (!any_open && (LONG)(now - ps_next_scan) >= 0) {
                ps_scan();
                ps_next_scan = now + 3000;
            }
        }
        if (xget) {
            DWORD now = GetTickCount();
            for (DWORD i = 0; i < 4; i++) {
                if (!connected[i] && (LONG)(now - retry_at[i]) < 0)
                    continue;       // empty slots are slow to poll: retry every 2 s
                XState st;
                if (xget(i, &st) == ERROR_SUCCESS) {
                    if (!connected[i])
                        logf_("Controller %lu connected.", i);
                    connected[i] = 1;
                    unsigned bits = st.Gamepad.wButtons | (st.Gamepad.bLeftTrigger > 128 ? PAD_LT : 0) |
                                    (st.Gamepad.bRightTrigger > 128 ? PAD_RT : 0);
                    if ((bits & pad_mask) == pad_mask)
                        pad = 1;
                } else {
                    if (connected[i])
                        logf_("Controller %lu disconnected.", i);
                    connected[i] = 0;
                    retry_at[i] = now + 2000;
                }
            }
        }
        if (((key && !prev_key) || (pad && !prev_pad)) && game_in_foreground()) {
            if (set_hidden(!g_hidden))
                logf_("HUD %s (%s)", g_hidden ? "hidden" : "shown", (key && !prev_key) ? "key" : "controller");
        }
        prev_key = key;
        prev_pad = pad;
    }
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_lock);
        HANDLE t = CreateThread(NULL, 0, main_thread, NULL, 0, NULL);
        if (t)
            CloseHandle(t);
    }
    return TRUE;
}
