#include "dshudtoggle.c"
int main(void)
{
    g_log = GetStdHandle(STD_OUTPUT_HANDLE);
    printf("sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) = %u (Windows x64 needs 8)\n", (unsigned)sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W));
    if (!ps_load()) { printf("ps_load failed\n"); return 1; }
    GUID g; H.GetHidGuid(&g);
    HDEVINFO info = H.GetClassDevs(&g, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    int n = 0, detail_ok = 0;
    for (DWORD i = 0;; i++) {
        SP_DEVICE_INTERFACE_DATA did; memset(&did, 0, sizeof did); did.cbSize = sizeof did;
        if (!H.EnumInterfaces(info, NULL, &g, i, &did)) break;
        n++;
        union { SP_DEVICE_INTERFACE_DETAIL_DATA_W d; BYTE raw[sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) + 600]; } u;
        u.d.cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (H.GetInterfaceDetail(info, &did, &u.d, sizeof u, NULL, NULL)) detail_ok++;
        else if (i == 0) printf("GetInterfaceDetail failed, error %lu\n", GetLastError());
    }
    H.DestroyList(info);
    printf("interfaces %d, details ok %d\n", n, detail_ok);
    return 0;
}
