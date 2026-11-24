// Unit tests for the PlayStation controller code of DSHudToggle (includes the mod source).
#include "dshudtoggle.c"

static int fails;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } else printf("ok:   %s\n", msg); } while (0)

int main(void)
{
    g_log = GetStdHandle(STD_OUTPUT_HANDLE);
    uint8_t r[80];
    unsigned bits = 0;

    memset(r, 0, sizeof r); r[0] = 0x01; r[8] = 0x08; r[9] = 0xC0;              // DualSense USB, L3+R3
    CHECK(ps_parse(1, 0, r, 64, &bits) && bits == 0xC0, "DualSense USB: L3+R3 -> 0xC0");
    memset(r, 0, sizeof r); r[0] = 0x01; r[8] = 0x20 | 0x0;                       // hat up + cross
    CHECK(ps_parse(1, 0, r, 64, &bits) && bits == (0x1 | 0x1000), "DualSense USB: dpad up + cross");
    memset(r, 0, sizeof r); r[0] = 0x01; r[8] = 0x08; r[10] = 0x04;               // mute
    CHECK(ps_parse(1, 0, r, 64, &bits) && bits == PAD_MUTE, "DualSense USB: mute");
    memset(r, 0, sizeof r); r[0] = 0x31; r[9] = 0x08; r[10] = 0xC0; r[11] = 0x04; // BT full: L3+R3+mute
    CHECK(ps_parse(1, 1, r, 78, &bits) && bits == (0xC0 | PAD_MUTE), "DualSense BT full: L3+R3+mute");
    memset(r, 0, sizeof r); r[0] = 0x31; r[9] = 0x01 | 0x80; r[10] = 0x01;         // up-right + triangle + L1
    CHECK(ps_parse(1, 1, r, 78, &bits) && bits == (0x1 | 0x8 | 0x8000 | 0x0100), "DualSense BT full: diagonal + triangle + L1");
    memset(r, 0, sizeof r); r[0] = 0x01; r[5] = 0x08; r[6] = 0xC0; r[7] = 0xFC;   // BT simple, counter bits set
    CHECK(ps_parse(1, 1, r, 78, &bits) && bits == 0xC0, "DualSense BT simple: L3+R3, counter ignored");
    memset(r, 0, sizeof r); r[0] = 0x01; r[5] = 0x88; r[7] = 0xFD;                // DS4 USB: triangle, PS, counter
    CHECK(ps_parse(0, 0, r, 64, &bits) && bits == (0x8000 | PAD_PS), "DS4 USB: triangle + PS, no mute");
    memset(r, 0, sizeof r); r[0] = 0x11; r[7] = 0x08; r[8] = 0x40;                // DS4 BT full: L3
    CHECK(ps_parse(0, 1, r, 78, &bits) && bits == 0x40, "DS4 BT full: L3");
    memset(r, 0, sizeof r); r[0] = 0x05;
    CHECK(!ps_parse(1, 0, r, 64, &bits), "unknown report id ignored");
    memset(r, 0, sizeof r); r[0] = 0x01; r[8] = 0x08; r[9] = 0x30;                // create + options
    CHECK(ps_parse(1, 0, r, 64, &bits) && bits == (0x0020 | 0x0010), "DualSense USB: create/options -> back/start");

    CHECK(parse_pad(L"L3+R3") == 0xC0, "parse L3+R3");
    CHECK(parse_pad(L"mute") == PAD_MUTE, "parse mute (lower case)");
    CHECK(parse_pad(L" PS + touchpad ") == (PAD_PS | PAD_TOUCHPAD), "parse PS + touchpad with spaces");
    CHECK(parse_pad(L"L1+R1+CROSS") == (0x0100 | 0x0200 | 0x1000), "parse L1+R1+CROSS");
    CHECK(parse_pad(L"NONE") == 0, "parse NONE");
    CHECK(parse_pad(L"bogus") == 0xFFFFFFFFu, "parse unknown name");

    const wchar_t *p = L"\\?\hid#test";
    CHECK(!ps_ready_to_open(p, 1000), "new controller: not opened at once");
    CHECK(!ps_ready_to_open(p, 4000), "3 s later: still waiting");
    CHECK(ps_ready_to_open(p, 6500), "5.5 s later: open");
    ps_retry_later(p, 6500);
    CHECK(!ps_ready_to_open(p, 20000), "after a failed open: wait");
    CHECK(ps_ready_to_open(p, 37000), "30 s after the failed open: retry");

    CHECK(ps_load(), "hid.dll / setupapi.dll functions loaded");
    ps_scan();
    int any = 0;
    for (int i = 0; i < PS_MAX; i++) any |= g_ps[i].open;
    printf("HID scan finished; controllers opened: %d (expect 0: controller is off and first sight waits 5 s)\n", any);
    printf(fails ? "SOME TESTS FAILED (%d)\n" : "ALL CONTROLLER TESTS PASSED\n", fails);
    return fails;
}
