//
// ndprov_install.cpp - Registers/unregisters the ndprov dummy provider
//                      in the Winsock catalog via WSCInstallProvider.
//
// Usage:
//   ndprov_install.exe install <full_path_to_ndprov.dll>
//   ndprov_install.exe uninstall
//
// Must run as Administrator.
//

#include <winsock2.h>
#include <ws2spi.h>
#include <stdio.h>
#include <initguid.h>

// {1F6A4848-5D04-4C77-B96C-4AB28A39449D}
DEFINE_GUID(CLSID_NdDummyProvider,
    0x1f6a4848, 0x5d04, 0x4c77, 0xb9, 0x6c, 0x4a, 0xb2, 0x8a, 0x39, 0x44, 0x9d);


// NetworkDirect service flags (must match what ndutil filters for)
#ifndef PFL_NETWORKDIRECT_PROVIDER
#define PFL_NETWORKDIRECT_PROVIDER 0x00000010
#endif

#ifndef ND_VERSION_2
#define ND_VERSION_2 0x20000
#endif

static int InstallProvider(const WCHAR* dllPath)
{
    WSAPROTOCOL_INFOW protocolInfo = { 0 };

    // Service flags required by NetworkDirect
    protocolInfo.dwServiceFlags1 =
        XP1_GUARANTEED_DELIVERY |
        XP1_GUARANTEED_ORDER |
        XP1_MESSAGE_ORIENTED |
        XP1_CONNECT_DATA;

    // Provider flags: hidden + NetworkDirect
    protocolInfo.dwProviderFlags = PFL_HIDDEN | PFL_NETWORKDIRECT_PROVIDER;

    // NDv2
    protocolInfo.iVersion = ND_VERSION_2;

    // Address family, socket type, protocol as required
    protocolInfo.iAddressFamily = AF_INET;
    protocolInfo.iSocketType = -1;
    protocolInfo.iProtocol = 0;
    protocolInfo.iProtocolMaxOffset = 0;

    // Address sizes and byte order (required for validation)
    protocolInfo.iMinSockAddr = sizeof(SOCKADDR_IN);
    protocolInfo.iMaxSockAddr = sizeof(SOCKADDR_IN);
    protocolInfo.iNetworkByteOrder = BIGENDIAN;

    // Message size (0 = not applicable, but some providers set this)
    protocolInfo.dwMessageSize = 0xFFFFFFFF;

    // Protocol chain — base provider
    protocolInfo.ProtocolChain.ChainLen = BASE_PROTOCOL;

    // Display name
    wcscpy_s(protocolInfo.szProtocol, L"NetworkDirect Dummy Provider");

    INT err = 0;

    // Verify the DLL can be loaded
    printf("Verifying DLL: %ls\n", dllPath);
    HMODULE hMod = LoadLibraryExW(dllPath, nullptr, 0);
    if (hMod == nullptr)
    {
        printf("Failed to load DLL: error %lu\n", GetLastError());
        return 1;
    }
    FARPROC pfn = GetProcAddress(hMod, "DllGetClassObject");
    if (pfn == nullptr)
    {
        printf("DllGetClassObject export not found: error %lu\n", GetLastError());
        FreeLibrary(hMod);
        return 1;
    }
    printf("DLL verified OK. DllGetClassObject found at %p\n", pfn);
    FreeLibrary(hMod);

    // Use a mutable copy of the GUID
    GUID guid = CLSID_NdDummyProvider;
    int ret = WSCInstallProvider64_32(
        &guid,
        dllPath,
        &protocolInfo,
        1,      // 1 protocol entry
        &err
    );

    if (ret == SOCKET_ERROR)
    {
        printf("WSCInstallProvider failed: error %d\n", err);
        return 1;
    }

    printf("Provider installed successfully.\n");
    printf("  DLL:  %ls\n", dllPath);
    printf("  Version: NDv2 (0x%x)\n", ND_VERSION_2);
    return 0;
}

static int UninstallProvider()
{
    GUID guid = CLSID_NdDummyProvider;
    INT err = 0;
    int failed = 0;

    // Remove from 64-bit catalog
    int ret = WSCDeinstallProvider(&guid, &err);
    if (ret == SOCKET_ERROR)
    {
        printf("WSCDeinstallProvider (64-bit) failed: error %d\n", err);
        failed++;
    }
    else
    {
        printf("Removed from 64-bit catalog.\n");
    }

    // Also remove from 32-bit catalog
    err = 0;
    ret = WSCDeinstallProvider32(&guid, &err);
    if (ret == SOCKET_ERROR)
    {
        printf("WSCDeinstallProvider32 (32-bit) failed: error %d\n", err);
        failed++;
    }
    else
    {
        printf("Removed from 32-bit catalog.\n");
    }

    if (failed == 2)
    {
        printf("Provider not found in either catalog.\n");
        return 1;
    }

    printf("Provider uninstalled.\n");
    return 0;
}

static void DumpNdProviders()
{
    DWORD len = 0;
    INT err;
    WSCEnumProtocols(nullptr, nullptr, &len, &err);
    if (len == 0) return;

    WSAPROTOCOL_INFOW* pProtos = (WSAPROTOCOL_INFOW*)malloc(len);
    if (!pProtos) return;

    INT count = WSCEnumProtocols(nullptr, pProtos, &len, &err);
    if (count == SOCKET_ERROR) { free(pProtos); return; }

    printf("\n=== Existing NetworkDirect providers ===\n");
    for (INT i = 0; i < count; i++)
    {
        DWORD ndFlags = XP1_GUARANTEED_DELIVERY | XP1_GUARANTEED_ORDER |
                        XP1_MESSAGE_ORIENTED | XP1_CONNECT_DATA;
        if ((pProtos[i].dwServiceFlags1 & ndFlags) != ndFlags) continue;
        if (pProtos[i].iSocketType != -1) continue;

        WCHAR guidStr[64];
        StringFromGUID2(pProtos[i].ProviderId, guidStr, 64);

        printf("  [%d] %ls\n", i, pProtos[i].szProtocol);
        printf("      GUID:          %ls\n", guidStr);
        printf("      Version:       0x%x\n", pProtos[i].iVersion);
        printf("      ServiceFlags1: 0x%lx\n", pProtos[i].dwServiceFlags1);
        printf("      ProviderFlags: 0x%lx\n", pProtos[i].dwProviderFlags);
        printf("      AddressFamily: %d\n", pProtos[i].iAddressFamily);
        printf("      SocketType:    %d\n", pProtos[i].iSocketType);
        printf("      Protocol:      %d\n", pProtos[i].iProtocol);
        printf("      ProtoMaxOff:   %d\n", pProtos[i].iProtocolMaxOffset);
        printf("      MessageSize:   0x%lx\n", pProtos[i].dwMessageSize);
        printf("      CatalogEntry:  %lu\n", pProtos[i].dwCatalogEntryId);
        printf("      ChainLen:      %d\n", pProtos[i].ProtocolChain.ChainLen);

        // Get the DLL path via WSCGetProviderPath
        WCHAR dllPath[MAX_PATH] = { 0 };
        INT pathLen = MAX_PATH;
        INT pathErr = 0;
        if (WSCGetProviderPath(&pProtos[i].ProviderId, dllPath, &pathLen, &pathErr) == 0)
        {
            printf("      DllPath:       %ls\n", dllPath);
        }
        else
        {
            printf("      DllPath:       (error %d)\n", pathErr);
        }

        printf("\n");
    }
    free(pProtos);
}

int wmain(int argc, WCHAR* argv[])
{
    if (argc < 2)
    {
        printf("Usage:\n");
        printf("  ndprov_install install <full_path_to_ndprov.dll>\n");
        printf("  ndprov_install uninstall\n");
        return 1;
    }

    WSADATA wsaData;
    int ret = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (ret != 0)
    {
        printf("WSAStartup failed: %d\n", ret);
        return 1;
    }

    int result = 1;
    if (_wcsicmp(argv[1], L"install") == 0)
    {
        if (argc < 3)
        {
            printf("Missing DLL path.\n");
            printf("Usage: ndprov_install install <full_path_to_ndprov.dll>\n");
        }
        else
        {
            DumpNdProviders();
            result = InstallProvider(argv[2]);
        }
    }
    else if (_wcsicmp(argv[1], L"uninstall") == 0)
    {
        result = UninstallProvider();
    }
    else if (_wcsicmp(argv[1], L"dump") == 0)
    {
        DumpNdProviders();
        result = 0;
    }
    else
    {
        printf("Unknown command: %ls\n", argv[1]);
        printf("Usage:\n");
        printf("  ndprov_install install <full_path_to_ndprov.dll>\n");
        printf("  ndprov_install uninstall\n");
    }

    WSACleanup();
    return result;
}
