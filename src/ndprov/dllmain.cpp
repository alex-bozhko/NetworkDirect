// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

//
// dllmain.cpp - DLL entry points for the NetworkDirect USB4 provider
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

volatile LONG g_objectCount = 0;

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    UNREFERENCED_PARAMETER(lpReserved);
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
    {
        TraceLoggingRegister(g_ndprovTraceProvider);
        NdTrace("DLL loaded (pid=%lu)", GetCurrentProcessId());
        DisableThreadLibraryCalls(hModule);
        break;
    }
    case DLL_PROCESS_DETACH:
        NdTrace("DLL unloaded (pid=%lu)", GetCurrentProcessId());
        TraceLoggingUnregister(g_ndprovTraceProvider);
        break;
    }
    return TRUE;
}

extern "C" HRESULT STDAPICALLTYPE DllGetClassObject(
    _In_ REFCLSID rclsid,
    _In_ REFIID riid,
    _Outptr_ LPVOID* ppv)
{
    WCHAR guidStr[64]{};
    WCHAR iidStr[64]{};
    StringFromGUID2(rclsid, guidStr, ARRAYSIZE(guidStr));
    StringFromGUID2(riid, iidStr, ARRAYSIZE(iidStr));
    NdTrace("DllGetClassObject: clsid=%ls, iid=%ls", guidStr, iidStr);

    if (ppv == nullptr)
    {
        return E_POINTER;
    }
    *ppv = nullptr;

    if (InlineIsEqualGUID(riid, IID_IND2Provider))
    {
        NdProvider *provider{ nullptr };
        const HRESULT status = NdProvider::Create(&provider);
        if (FAILED(status))
        {
            return status;
        }
        *ppv = static_cast<IND2Provider *>(provider);
        NdTrace("Returning IND2Provider OK");
        return S_OK;
    }

    NdTrace("Unknown IID, returning E_NOINTERFACE");
    return E_NOINTERFACE;
}

extern "C" HRESULT STDAPICALLTYPE DllCanUnloadNow()
{
    const LONG objectCount = InterlockedCompareExchange(&g_objectCount, 0, 0);
    NdTrace("DllCanUnloadNow: objects=%ld", objectCount);
    return objectCount == 0 ? S_OK : S_FALSE;
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
    UNREFERENCED_PARAMETER(wVersionRequested);
    UNREFERENCED_PARAMETER(lpWSPData);
    UNREFERENCED_PARAMETER(lpProtocolInfo);
    UNREFERENCED_PARAMETER(UpcallTable);
    UNREFERENCED_PARAMETER(lpProcTable);
    return WSAEPROCLIM;
}
