#include "dshudtoggle.c"
int main(void)
{
    g_log = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!ps_load()) { printf("ps_load failed\n"); return 1; }
    ps_scan(); printf("scan 1 done\n");
    Sleep(6000);
    ps_scan(); printf("scan 2 done\n");
    for (int t = 0; t < 30; t++) {
        Sleep(100);
        for (int i = 0; i < PS_MAX; i++)
            if (g_ps[i].open && t % 10 == 0)
                printf("slot %d: bt=%d len=%u buttons=0x%05lX\n", i, g_ps[i].bt, (unsigned)g_ps[i].len, (unsigned long)g_ps[i].buttons);
    }
    int any = 0; for (int i = 0; i < PS_MAX; i++) any |= g_ps[i].open;
    printf("opened: %d\n", any);
    return 0;
}
