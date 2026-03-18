//
// dllmain.cpp - DLL entry points for the NetworkDirect dummy provider
//
// Exports DllGetClassObject and DllCanUnloadNow as required by the
// ndutil framework to load and instantiate the IND2Provider.
//
// For NDv2, DllGetClassObject is called directly with IID_IND2Provider
// (no IClassFactory indirection needed).
//

#include "ndprov.h"
#include <ws2spi.h>
#include <new>

static volatile LONG g_cServerLocks = 0;
static volatile LONG g_cObjects = 0;

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
    {
        LogToFile("[ndprov] DLL loaded (pid=%lu)\n", GetCurrentProcessId());
        DisableThreadLibraryCalls(hModule);
        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);
        break;
    }
    case DLL_PROCESS_DETACH:
        WSACleanup();
        LogToFile("[ndprov] DLL unloaded (pid=%lu)\n", GetCurrentProcessId());
        break;
    }
    return TRUE;
}

extern "C" HRESULT STDAPICALLTYPE DllGetClassObject(
    _In_ REFCLSID rclsid,
    _In_ REFIID riid,
    _Outptr_ LPVOID* ppv)
{
    WCHAR guidStr[64], iidStr[64];
    StringFromGUID2(rclsid, guidStr, 64);
    StringFromGUID2(riid, iidStr, 64);
    LogToFile("[ndprov] DllGetClassObject: clsid=%ls, iid=%ls\n", guidStr, iidStr);

    if (ppv == nullptr)
    {
        return E_POINTER;
    }
    *ppv = nullptr;

    if (InlineIsEqualGUID(riid, IID_IND2Provider))
    {
        NdProvider* pProvider = new (std::nothrow) NdProvider();
        if (pProvider == nullptr)
        {
            return E_OUTOFMEMORY;
        }
        *ppv = static_cast<IND2Provider*>(pProvider);
        InterlockedIncrement(&g_cObjects);
        LogToFile("[ndprov] Returning IND2Provider OK\n");
        return S_OK;
    }

    LogToFile("[ndprov] Unknown IID, returning E_NOINTERFACE\n");
    return E_NOINTERFACE;
}

extern "C" HRESULT STDAPICALLTYPE DllCanUnloadNow()
{
    LogToFile("[ndprov] DllCanUnloadNow: objects=%ld, locks=%ld\n", g_cObjects, g_cServerLocks);
    return (g_cObjects == 0 && g_cServerLocks == 0) ? S_OK : S_FALSE;
}

//
// WSPStartup - Required by the Winsock catalog validator.
// Never actually called for NetworkDirect providers, but the export must exist.
//
extern "C" int WSPAPI WSPStartup(
    WORD wVersionRequested,
    LPWSPDATA lpWSPData,
    LPWSAPROTOCOL_INFOW lpProtocolInfo,
    WSPUPCALLTABLE UpcallTable,
    LPWSPPROC_TABLE lpProcTable)
{
    return WSAEPROCLIM;
}
