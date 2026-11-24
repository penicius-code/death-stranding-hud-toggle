#include "dshudtoggle.c"
int main(void)
{
    uint8_t r[80] = {0};
    unsigned bits = 0, mask = parse_pad(L"MUTE");
    r[0] = 0x31; r[9] = 0x08; r[11] = 0x04;                 /* DualSense Bluetooth full report, mute held */
    int bt_ok = ps_parse(1, 1, r, 78, &bits) && (bits & mask) == mask;
    memset(r, 0, sizeof r); r[0] = 0x01; r[8] = 0x08; r[10] = 0x04;   /* DualSense USB, mute held */
    int usb_ok = ps_parse(1, 0, r, 64, &bits) && (bits & mask) == mask;
    memset(r, 0, sizeof r); r[0] = 0x31; r[9] = 0x08; r[10] = 0xC0;  /* L3+R3 only: must NOT trigger */
    int no_false = ps_parse(1, 1, r, 78, &bits) && (bits & mask) != mask;
    printf("mask=0x%X  BT mute: %s  USB mute: %s  L3+R3 ignored: %s\n", mask, bt_ok ? "ok" : "FAIL", usb_ok ? "ok" : "FAIL", no_false ? "ok" : "FAIL");
    return !(bt_ok && usb_ok && no_false);
}
