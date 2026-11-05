// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

//
// ndprov.cpp - NetworkDirect v2 Provider backed by RDMA IOCTLs
//
// Routes IND2 interface calls to the USB4 P2P RDMA kernel driver
// via IOCTLs defined in RdmaIoctl.h.
//

#include <initguid.h>
#include "ndprov.h"
#include <mstcpip.h>
#include <new>

static constexpr WCHAR c_configurationRegistryPath[]{ L"SOFTWARE\\Microsoft\\USB4P2P\\NDProvider" };
static constexpr WCHAR c_cmPortOffsetValueName[]{ L"CmPortOffset" };
static constexpr WCHAR c_notifySpinMicrosecondsValueName[] = L"NotifySpinMicroseconds";
static constexpr WCHAR c_zeroPortFallbackServicePortValueName[]{ L"ZeroPortFallbackServicePort" };
static constexpr DWORD c_defaultCmPortOffset{ 1 };
static constexpr DWORD c_defaultZeroPortFallbackServicePort{ 23517 };
static constexpr DWORD c_maxTcpPort{ 0xffff };
static constexpr DWORD c_maxNotifySpinMicroseconds = 1000;
static constexpr LONGLONG c_microsecondsPerSecond = 1000000;

struct NdProviderConfiguration
{
    DWORD cmPortOffset{ c_defaultCmPortOffset };
    DWORD zeroPortFallbackServicePort{ c_defaultZeroPortFallbackServicePort };
    DWORD notifySpinMicroseconds = 0;
};

static
bool
TryReadConfigurationDword(
    _In_z_ const WCHAR *valueName,
    _Out_ DWORD *value)
{
    DWORD valueSize{ sizeof(*value) };
    const LSTATUS status = RegGetValueW(
        HKEY_LOCAL_MACHINE,
        c_configurationRegistryPath,
        valueName,
        RRF_RT_REG_DWORD | RRF_SUBKEY_WOW6464KEY,
        nullptr,
        value,
        &valueSize);
    return status == ERROR_SUCCESS;
}

static
const NdProviderConfiguration &
GetProviderConfiguration()
{
    static const NdProviderConfiguration configuration = []()
    {
        NdProviderConfiguration result{};
        DWORD value{};
        if (TryReadConfigurationDword(
                c_cmPortOffsetValueName,
                &value) &&
            value <= c_maxTcpPort)
        {
            result.cmPortOffset = value;
        }

        if (TryReadConfigurationDword(
                c_zeroPortFallbackServicePortValueName,
                &value) &&
            value <= c_maxTcpPort)
        {
            result.zeroPortFallbackServicePort = value;
        }

        if (TryReadConfigurationDword(
                c_notifySpinMicrosecondsValueName,
                &value))
        {
            result.notifySpinMicroseconds = value < c_maxNotifySpinMicroseconds ? value : c_maxNotifySpinMicroseconds;
        }

        return result;
    }();

    return configuration;
}

// Microsoft.NetworkDirect.NdProv
// {B71AF425-C11E-54C8-BD6E-20FFB13DCADB}, derived from the provider name.
// Trace consumers depend on this name and GUID; do not change them.
TRACELOGGING_DEFINE_PROVIDER(
    g_ndprovTraceProvider,
    "Microsoft.NetworkDirect.NdProv",
    (0xb71af425, 0xc11e, 0x54c8, 0xbd, 0x6e, 0x20, 0xff, 0xb1, 0x3d, 0xca, 0xdb));

void
NdTrace(
    _In_z_ _Printf_format_string_ const char *format,
    ...)
{
    if (!TraceLoggingProviderEnabled(
            g_ndprovTraceProvider,
            WINEVENT_LEVEL_INFO,
            0))
    {
        return;
    }

    char message[512];
    va_list arguments;
    va_start(
        arguments,
        format);
    _vsnprintf_s(
        message,
        _TRUNCATE,
        format,
        arguments);
    va_end(arguments);
    TraceLoggingWrite(
        g_ndprovTraceProvider,
        "Trace",
        TraceLoggingLevel(WINEVENT_LEVEL_INFO),
        TraceLoggingString(message, "Message"));
}

// ============================================================================
// NdDeviceContext
// ============================================================================

// Returns the path of a file on the RDMA interface, or nullptr when out of
// memory.
static
wistd::unique_ptr<WCHAR[]>
MakeSuffixedPath(
    _In_z_ const WCHAR *symbolicLink,
    _In_z_ const WCHAR *suffix)
{
    const size_t pathLength = wcslen(symbolicLink) + wcslen(suffix) + 1;
    wistd::unique_ptr<WCHAR[]> path = wil::make_unique_nothrow<WCHAR[]>(pathLength);
    if (path != nullptr)
    {
        wcscpy_s(
            path.get(),
            pathLength,
            symbolicLink);
        wcscat_s(
            path.get(),
            pathLength,
            suffix);
    }

    return path;
}

HRESULT
NdDeviceContext::Create(
    _In_z_ const WCHAR *symbolicLink,
    _Outptr_ NdDeviceContext **deviceContext)
{
    if (symbolicLink == nullptr || deviceContext == nullptr)
    {
        return E_INVALIDARG;
    }

    *deviceContext = nullptr;

    wistd::unique_ptr<NdDeviceContext> context = wil::make_unique_nothrow<NdDeviceContext>();
    if (context == nullptr)
    {
        return E_OUTOFMEMORY;
    }

    const HRESULT status = context->Open(symbolicLink);
    if (FAILED(status))
    {
        return status;
    }

    *deviceContext = context.release();
    return S_OK;
}

HRESULT
NdDeviceContext::Open(
    _In_z_ const WCHAR *symbolicLink)
{
    const size_t symbolicLinkLength = wcslen(symbolicLink) + 1;
    m_symbolicLink = wil::make_unique_nothrow<WCHAR[]>(symbolicLinkLength);
    if (m_symbolicLink == nullptr)
    {
        return E_OUTOFMEMORY;
    }
    wcscpy_s(
        m_symbolicLink.get(),
        symbolicLinkLength,
        symbolicLink);

    m_queuePairPath = MakeSuffixedPath(
        symbolicLink,
        RDMA_QUEUE_PAIR_FILE_SUFFIX);
    m_overlappedPath = MakeSuffixedPath(
        symbolicLink,
        RDMA_OVERLAPPED_FILE_SUFFIX);
    if (m_queuePairPath == nullptr || m_overlappedPath == nullptr)
    {
        return E_OUTOFMEMORY;
    }

    wil::unique_hfile deviceHandle{ CreateFileW(
        symbolicLink,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED,
        nullptr) };
    if (!deviceHandle)
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    AcquireSRWLockExclusive(&m_deviceLock);
    m_deviceHandle = std::move(deviceHandle);
    ReleaseSRWLockExclusive(&m_deviceLock);

    CM_NOTIFY_FILTER handleFilter{};
    handleFilter.cbSize = sizeof(handleFilter);
    handleFilter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEHANDLE;
    handleFilter.u.DeviceHandle.hTarget = m_deviceHandle.get();

    HCMNOTIFICATION handleNotification{ nullptr };
    CONFIGRET configResult = CM_Register_Notification(
        &handleFilter,
        this,
        DeviceNotificationCallback,
        &handleNotification);
    if (configResult != CR_SUCCESS)
    {
        CloseForRemoval();
        return HRESULT_FROM_WIN32(CM_MapCrToWin32Err(
            configResult,
            ERROR_GEN_FAILURE));
    }
    m_deviceHandleNotification.reset(handleNotification);

    CM_NOTIFY_FILTER interfaceFilter{};
    interfaceFilter.cbSize = sizeof(interfaceFilter);
    interfaceFilter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
    interfaceFilter.u.DeviceInterface.ClassGuid = GUID_DEVINTERFACE_RDMA;

    HCMNOTIFICATION interfaceNotification{ nullptr };
    configResult = CM_Register_Notification(
        &interfaceFilter,
        this,
        DeviceNotificationCallback,
        &interfaceNotification);
    if (configResult != CR_SUCCESS)
    {
        m_deviceHandleNotification.reset();
        CloseForRemoval();
        return HRESULT_FROM_WIN32(CM_MapCrToWin32Err(
            configResult,
            ERROR_GEN_FAILURE));
    }
    m_deviceInterfaceNotification.reset(interfaceNotification);

    return S_OK;
}

void
NdDeviceContext::CloseForRemoval()
{
    AcquireSRWLockExclusive(&m_deviceLock);
    m_deviceHandle.reset();
    ReleaseSRWLockExclusive(&m_deviceLock);

    const auto queuePairLock = wil::AcquireSRWLockShared(&m_queuePairLock);
    for (LIST_ENTRY *entry = m_queuePairs.Flink; entry != &m_queuePairs; entry = entry->Flink)
    {
        NdQueuePair::FromDeviceLink(entry)->CloseForRemoval();
    }
}

void
NdDeviceContext::RegisterQueuePair(
    _Inout_ LIST_ENTRY *queuePairLink)
{
    // User mode has no InsertTailList.
    const auto queuePairLock = wil::AcquireSRWLockExclusive(&m_queuePairLock);
    queuePairLink->Flink = &m_queuePairs;
    queuePairLink->Blink = m_queuePairs.Blink;
    m_queuePairs.Blink->Flink = queuePairLink;
    m_queuePairs.Blink = queuePairLink;
}

void
NdDeviceContext::UnregisterQueuePair(
    _Inout_ LIST_ENTRY *queuePairLink)
{
    const auto queuePairLock = wil::AcquireSRWLockExclusive(&m_queuePairLock);
    queuePairLink->Blink->Flink = queuePairLink->Flink;
    queuePairLink->Flink->Blink = queuePairLink->Blink;
    queuePairLink->Flink = queuePairLink;
    queuePairLink->Blink = queuePairLink;
}

DWORD CALLBACK
NdDeviceContext::DeviceNotificationCallback(
    HCMNOTIFICATION notification,
    _In_opt_ void *context,
    CM_NOTIFY_ACTION action,
    _In_reads_bytes_(eventDataSize) PCM_NOTIFY_EVENT_DATA eventData,
    DWORD eventDataSize)
{
    UNREFERENCED_PARAMETER(notification);
    UNREFERENCED_PARAMETER(eventDataSize);

    NdDeviceContext *const deviceContext = static_cast<NdDeviceContext *>(context);
    if (deviceContext == nullptr)
    {
        return ERROR_SUCCESS;
    }

    if (action == CM_NOTIFY_ACTION_DEVICEQUERYREMOVE || action == CM_NOTIFY_ACTION_DEVICEREMOVEPENDING ||
        action == CM_NOTIFY_ACTION_DEVICEREMOVECOMPLETE)
    {
        deviceContext->CloseForRemoval();
    }
    else if (action == CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL && eventData != nullptr &&
             _wcsicmp(
                 eventData->u.DeviceInterface.SymbolicLink,
                 deviceContext->m_symbolicLink.get()) == 0)
    {
        deviceContext->CloseForRemoval();
    }

    return ERROR_SUCCESS;
}

HRESULT
NdDeviceContext::SendIoctl(
    DWORD ioctl,
    _In_reads_bytes_opt_(inputSize) const void *input,
    DWORD inputSize,
    _Out_writes_bytes_opt_(outputSize) void *output,
    DWORD outputSize,
    _Out_opt_ DWORD *bytesReturned)
{
    AcquireSRWLockShared(&m_deviceLock);
    if (!m_deviceHandle)
    {
        ReleaseSRWLockShared(&m_deviceLock);
        return HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED);
    }

    const HRESULT status = ::SendIoctl(
        m_deviceHandle.get(),
        ioctl,
        input,
        inputSize,
        output,
        outputSize,
        bytesReturned);
    ReleaseSRWLockShared(&m_deviceLock);
    return status;
}

BOOL
NdDeviceContext::StartIoctl(
    DWORD ioctl,
    _In_reads_bytes_opt_(inputSize) const void *input,
    DWORD inputSize,
    _Out_writes_bytes_opt_(outputSize) void *output,
    DWORD outputSize,
    _Out_opt_ DWORD *bytesReturned,
    _Inout_opt_ OVERLAPPED *overlapped)
{
    AcquireSRWLockShared(&m_deviceLock);
    if (!m_deviceHandle)
    {
        ReleaseSRWLockShared(&m_deviceLock);
        SetLastError(ERROR_DEVICE_NOT_CONNECTED);
        return FALSE;
    }

    const BOOL succeeded = DeviceIoControl(
        m_deviceHandle.get(),
        ioctl,
        const_cast<void *>(input),
        inputSize,
        output,
        outputSize,
        bytesReturned,
        overlapped);
    ReleaseSRWLockShared(&m_deviceLock);
    return succeeded;
}

BOOL
NdDeviceContext::CancelIo(
    _In_opt_ OVERLAPPED *overlapped)
{
    AcquireSRWLockShared(&m_deviceLock);
    if (!m_deviceHandle)
    {
        ReleaseSRWLockShared(&m_deviceLock);
        SetLastError(ERROR_DEVICE_NOT_CONNECTED);
        return FALSE;
    }

    const BOOL succeeded = CancelIoEx(
        m_deviceHandle.get(),
        overlapped);
    ReleaseSRWLockShared(&m_deviceLock);
    return succeeded;
}

BOOL
NdDeviceContext::GetIoResult(
    _In_ OVERLAPPED *overlapped,
    _Out_ DWORD *bytesTransferred,
    BOOL wait)
{
    AcquireSRWLockShared(&m_deviceLock);
    if (!m_deviceHandle)
    {
        ReleaseSRWLockShared(&m_deviceLock);
        SetLastError(ERROR_DEVICE_NOT_CONNECTED);
        return FALSE;
    }

    const BOOL succeeded = ::GetOverlappedResult(
        m_deviceHandle.get(),
        overlapped,
        bytesTransferred,
        wait);
    ReleaseSRWLockShared(&m_deviceLock);
    return succeeded;
}

void
NdDeviceContext::AcquireUsage()
{
    AcquireSRWLockShared(&m_deviceLock);
}

void
NdDeviceContext::ReleaseUsage()
{
    ReleaseSRWLockShared(&m_deviceLock);
}

HANDLE
NdDeviceContext::GetHandleLocked() const
{
    return m_deviceHandle.get();
}


// ============================================================================
// NdCompletionQueue
// ============================================================================

struct NdCompletionTranslation
{
    ND2_REQUEST_TYPE RequestType = Nd2RequestTypeSend;
    HRESULT Status = ND_UNSUCCESSFUL;
};

static constexpr ULONG c_completionEventTypeCount = RdmaCompletionEventTypeTransportError + 1;

using NdCompletionTranslationTable = std::array<NdCompletionTranslation, c_completionEventTypeCount>;

static constexpr
NdCompletionTranslation
TranslateCompletionEvent(
    ULONG event)
{
    switch (event)
    {
        case RdmaCompletionEventTypeSend:
            return { Nd2RequestTypeSend,
                     ND_SUCCESS };
        case RdmaCompletionEventTypeReceive:
            return { Nd2RequestTypeReceive,
                     ND_SUCCESS };
        case RdmaCompletionEventTypeSendFlushError:
            return { Nd2RequestTypeSend,
                     ND_CANCELED };
        case RdmaCompletionEventTypeReceiveFlushError:
            return { Nd2RequestTypeReceive,
                     ND_CANCELED };
        case RdmaCompletionEventTypeReceiveBufferOverflow:
            return { Nd2RequestTypeReceive,
                     ND_BUFFER_OVERFLOW };
        case RdmaCompletionEventTypeLocalLengthError:
            return { Nd2RequestTypeReceive,
                     ND_DATA_OVERRUN };
        case RdmaCompletionEventTypeRemoteOperationError:
            return { Nd2RequestTypeReceive,
                     ND_REMOTE_ERROR };
        case RdmaCompletionEventTypeTransportError:
            return { Nd2RequestTypeReceive,
                     ND_CONNECTION_ABORTED };
        default:
            return {};
    }
}

static constexpr
NdCompletionTranslationTable
BuildCompletionTranslationTable()
{
    NdCompletionTranslationTable table{};
    for (ULONG event = 0; event < c_completionEventTypeCount; ++event)
    {
        table[event] = TranslateCompletionEvent(event);
    }

    return table;
}

static constexpr NdCompletionTranslationTable c_completionTranslationTable = BuildCompletionTranslationTable();
static constexpr NdCompletionTranslation c_unknownCompletionTranslation{};

static
const NdCompletionTranslation &
LookupCompletionTranslation(
    ULONG event)
{
    if (event >= c_completionTranslationTable.size())
    {
        return c_unknownCompletionTranslation;
    }

    return c_completionTranslationTable[event];
}

NdCompletionQueue::NdCompletionQueue(
    _In_ NdDeviceContext *deviceContext,
    _In_opt_ HANDLE overlappedFile,
    wil::unique_virtualalloc_ptr<RDMA_CQ_RING_HEADER> &&ring,
    UINT32 capacity)
    : m_deviceContext{ deviceContext }
    , m_overlappedFile{ overlappedFile == INVALID_HANDLE_VALUE ? nullptr : overlappedFile }
    , m_ring{ std::move(ring) }
    , m_capacity{ capacity }
    , m_positionMask{ capacity - 1 }
{
    m_entries = reinterpret_cast<RDMA_CQ_ENTRY *>(reinterpret_cast<BYTE *>(m_ring.get()) + RDMA_CQ_RING_ENTRIES_OFFSET);

    const DWORD spinMicroseconds = GetProviderConfiguration().notifySpinMicroseconds;
    if (spinMicroseconds > 0)
    {
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        m_notifySpinTicks = (frequency.QuadPart * spinMicroseconds) / c_microsecondsPerSecond;
    }
}

HRESULT
NdCompletionQueue::Initialize()
{
    RDMA_CREATE_COMPLETION_QUEUE_INPUT input{};
    input.RingAddress = static_cast<UINT64>(reinterpret_cast<ULONG_PTR>(m_ring.get()));
    input.RingSize = static_cast<UINT32>(RDMA_CQ_RING_SIZE(m_capacity));
    input.Capacity = m_capacity;

    RDMA_CREATE_COMPLETION_QUEUE_OUTPUT output{};
    const HRESULT createStatus = m_deviceContext->SendIoctl(
        IOCTL_RDMA_CREATE_COMPLETION_QUEUE,
        &input,
        sizeof(input),
        &output,
        sizeof(output));
    if (FAILED(createStatus))
    {
        return createStatus;
    }

    m_cqId = output.CompletionQueueId;
    return S_OK;
}

template <typename Operation>
HRESULT
NdCompletionQueue::RunWithNotifyHandle(
    Operation &&operation)
{
    if (m_overlappedFile != nullptr)
    {
        return operation(m_overlappedFile);
    }

    NdDeviceUsageGuard deviceUsage{ m_deviceContext.Get() };
    if (!deviceUsage.IsAvailable())
    {
        return ND_DEVICE_REMOVED;
    }

    return operation(deviceUsage.GetHandle());
}

RDMA_CQ_ENTRY &
NdCompletionQueue::GetEntry(
    ULONG position) const
{
    return m_entries[position & m_positionMask];
}

LONG
NdCompletionQueue::GetState() const
{
    return ReadAcquire(&m_ring->State);
}

bool
NdCompletionQueue::IsFailed() const
{
    return GetState() != RdmaCompletionQueueStateActive;
}

bool
NdCompletionQueue::IsReady(
    ULONG type) const
{
    const ULONG head = ReadULongAcquire(&m_ring->Head);
    switch (type)
    {
        case ND_CQ_NOTIFY_ERRORS:
            return static_cast<LONG>(ReadULongAcquire(&m_ring->ErrorTail) - head) > 0;
        case ND_CQ_NOTIFY_SOLICITED:
            return static_cast<LONG>(ReadULongAcquire(&m_ring->SolicitedTail) - head) > 0;
        default:
            return ReadULongAcquire(&GetEntry(head).Sequence) == head + 1;
    }
}

bool
NdCompletionQueue::SpinUntilReady(
    ULONG type) const
{
    if (m_notifySpinTicks <= 0)
    {
        return false;
    }

    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    const LONGLONG deadline = now.QuadPart + m_notifySpinTicks;
    do
    {
        YieldProcessor();
        if (IsReady(type))
        {
            return true;
        }

        if (GetState() != RdmaCompletionQueueStateActive)
        {
            return false;
        }

        QueryPerformanceCounter(&now);
    } while (now.QuadPart < deadline);

    return false;
}

HRESULT
NdCompletionQueue::MapNotifyError(
    DWORD error) const
{
    switch (error)
    {
        case ERROR_IO_INCOMPLETE:
            return ND_PENDING;
        case ERROR_OPERATION_ABORTED:
            return ND_CANCELED;
        case ERROR_MORE_DATA:
            return ND_BUFFER_OVERFLOW;
        case ERROR_INSUFFICIENT_BUFFER:
        case ERROR_NO_SYSTEM_RESOURCES:
        case ERROR_NOT_ENOUGH_MEMORY:
        case ERROR_OUTOFMEMORY:
            return ND_INSUFFICIENT_RESOURCES;
        case ERROR_DEVICE_REMOVED:
        case ERROR_DEVICE_NOT_CONNECTED:
            return ND_DEVICE_REMOVED;
        default:
            break;
    }

    const LONG state = GetState();
    if (state == RdmaCompletionQueueStateOverflow)
    {
        return ND_BUFFER_OVERFLOW;
    }

    if (state == RdmaCompletionQueueStateClosed)
    {
        return ND_DEVICE_REMOVED;
    }

    return HRESULT_FROM_WIN32(error);
}

NdCompletionQueue::~NdCompletionQueue()
{
    if (m_cqId == RDMA_INVALID_ID)
    {
        return;
    }

    CancelOverlappedRequests();

    NdDeviceUsageGuard deviceUsage{ m_deviceContext.Get() };
    if (!deviceUsage.IsAvailable())
    {
        // Closing the RDMA handle already made the driver release the ring.
        return;
    }

    const RDMA_DESTROY_COMPLETION_QUEUE_INPUT input{ m_cqId };
    const HRESULT destroyStatus = SendIoctl(
        deviceUsage.GetHandle(),
        IOCTL_RDMA_DESTROY_COMPLETION_QUEUE,
        &input,
        sizeof(input),
        nullptr,
        0);
    if (FAILED(destroyStatus))
    {
        // The driver closes a queue before destroy returns, even one still in
        // use, so this fails only if the request didn't reach it. It may then
        // still write the ring, so the ring must outlive this object.
        const RDMA_CQ_RING_HEADER *const leakedRing = m_ring.release();
        NdTrace(
            "CQ::~NdCompletionQueue destroy failed: 0x%08X, leaking ring %p",
            destroyStatus,
            static_cast<const void *>(leakedRing));
        return;
    }
}

HRESULT NdCompletionQueue::QueryInterface(REFIID riid, LPVOID* ppvObj)
{
    if (InlineIsEqualGUID(riid, IID_IUnknown) || InlineIsEqualGUID(riid, IID_IND2CompletionQueue))
    {
        *ppvObj = static_cast<IND2CompletionQueue*>(this);
        AddRef();
        return S_OK;
    }
    *ppvObj = nullptr;
    return E_NOINTERFACE;
}

static
ULONG_PTR
ReadOverlappedStatus(
    _In_ const OVERLAPPED *overlapped)
{
    return ReadULongPtrAcquire(&overlapped->Internal);
}

// Waits for a request whose OVERLAPPED has no event by polling its status.
// ::GetOverlappedResult would wait on the file instead, which the I/O manager
// doesn't signal: overlapped files skip setting the file's event.
static
void
WaitForOverlappedWithoutEvent(
    _In_ const OVERLAPPED *overlapped)
{
    while (ReadOverlappedStatus(overlapped) == STATUS_PENDING)
    {
        Sleep(1);
    }
}

HRESULT NdCompletionQueue::CancelOverlappedRequests()
{
    return RunWithNotifyHandle(
        [this](_In_ HANDLE notifyHandle) -> HRESULT
        {
            HRESULT cancelStatus = S_OK;
            const wil::rwlock_release_shared_scope_exit lock = wil::AcquireSRWLockShared(&m_notifyLock);
            for (OVERLAPPED *const request : m_notifyRequests)
            {
                if (!CancelIoEx(
                        notifyHandle,
                        request))
                {
                    const DWORD error = GetLastError();
                    if (error != ERROR_NOT_FOUND && SUCCEEDED(cancelStatus))
                    {
                        cancelStatus = HRESULT_FROM_WIN32(error);
                    }
                }
            }

            return cancelStatus;
        });
}

HRESULT NdCompletionQueue::GetOverlappedResult(OVERLAPPED* pOverlapped, BOOL wait)
{
    if (pOverlapped == nullptr)
    {
        return E_INVALIDARG;
    }

    if (wait && pOverlapped->hEvent == nullptr)
    {
        WaitForOverlappedWithoutEvent(pOverlapped);
    }

    return RunWithNotifyHandle(
        [this, pOverlapped, wait](_In_ HANDLE notifyHandle) -> HRESULT
        {
            DWORD bytesTransferred = 0;
            const BOOL succeeded = ::GetOverlappedResult(
                notifyHandle,
                pOverlapped,
                &bytesTransferred,
                wait);
            const DWORD error = succeeded ? ERROR_SUCCESS : GetLastError();
            if (error != ERROR_IO_INCOMPLETE)
            {
                const wil::rwlock_release_exclusive_scope_exit lock = wil::AcquireSRWLockExclusive(&m_notifyLock);
                m_notifyRequests.erase(pOverlapped);
            }

            if (succeeded)
            {
                return GetState() == RdmaCompletionQueueStateOverflow ? ND_BUFFER_OVERFLOW : ND_SUCCESS;
            }

            return MapNotifyError(error);
        });
}

HRESULT NdCompletionQueue::GetNotifyAffinity(USHORT* pGroup, KAFFINITY* pAffinity)
{
    if (pGroup)
    {
        *pGroup = 0;
    }
    if (pAffinity)
    {
        *pAffinity = 1;
    }
    return S_OK;
}

HRESULT NdCompletionQueue::Resize(ULONG queueDepth)
{
    UNREFERENCED_PARAMETER(queueDepth);
    return ND_NOT_SUPPORTED;
}

HRESULT NdCompletionQueue::Notify(ULONG type, OVERLAPPED* pOverlapped)
{
    if (pOverlapped == nullptr)
    {
        return E_INVALIDARG;
    }

    if (type > ND_CQ_NOTIFY_SOLICITED)
    {
        return ND_INVALID_PARAMETER_1;
    }

    const LONG state = GetState();
    if (state == RdmaCompletionQueueStateOverflow)
    {
        return ND_BUFFER_OVERFLOW;
    }

    if (state != RdmaCompletionQueueStateActive)
    {
        return ND_DEVICE_REMOVED;
    }

    if (IsReady(type) || SpinUntilReady(type))
    {
        return ND_SUCCESS;
    }

    return RunWithNotifyHandle(
        [this, type, pOverlapped](_In_ HANDLE notifyHandle) -> HRESULT
        {
            // Holding the lock across the request keeps CancelOverlappedRequests
            // from missing a request that is being issued.
            const wil::rwlock_release_exclusive_scope_exit lock = wil::AcquireSRWLockExclusive(&m_notifyLock);
            if (m_notifyRequests.find(pOverlapped) != m_notifyRequests.end())
            {
                // A completed request whose result was never collected can be reissued.
                if (!HasOverlappedIoCompleted(pOverlapped))
                {
                    return ND_INVALID_PARAMETER_2;
                }
            }
            else
            {
                m_notifyRequests.insert(pOverlapped);
            }

            RDMA_ARM_COMPLETION_QUEUE_INPUT input{};
            input.CompletionQueueId = m_cqId;
            input.Type = type;
            if (DeviceIoControl(
                    notifyHandle,
                    IOCTL_RDMA_ARM_COMPLETION_QUEUE,
                    &input,
                    sizeof(input),
                    nullptr,
                    0,
                    nullptr,
                    pOverlapped))
            {
                m_notifyRequests.erase(pOverlapped);
                return GetState() == RdmaCompletionQueueStateOverflow ? ND_BUFFER_OVERFLOW : ND_SUCCESS;
            }

            const DWORD error = GetLastError();

            // A warning status still signals the event and queues a completion
            // packet, so the caller collects it like a pending request.
            if (error == ERROR_IO_PENDING || error == ERROR_MORE_DATA)
            {
                return ND_PENDING;
            }

            m_notifyRequests.erase(pOverlapped);
            return MapNotifyError(error);
        });
}

ULONG NdCompletionQueue::GetResults(ND2_RESULT results[], ULONG nResults)
{
    if (results == nullptr)
    {
        return 0;
    }

    RDMA_CQ_RING_HEADER *const header = m_ring.get();
    ULONG returned = 0;
    while (returned < nResults)
    {
        const ULONG head = ReadULongAcquire(&header->Head);
        const ULONG remaining = nResults - returned;
        const ULONG wanted = remaining < m_capacity ? remaining : m_capacity;
        ULONG ready = 0;
        ULONG sequence = 0;
        while (ready < wanted)
        {
            sequence = ReadULongAcquire(&GetEntry(head + ready).Sequence);
            if (sequence != head + ready + 1)
            {
                break;
            }

            ++ready;
        }

        if (ready == 0)
        {
            if (static_cast<LONG>(sequence - (head + 1)) < 0)
            {
                break;
            }

            // Another consumer claimed the slot, so Head has moved. A Head that
            // has not moved means the ring is corrupt; stop instead of spinning.
            if (ReadULongAcquire(&header->Head) == head)
            {
                break;
            }

            continue;
        }

        if (InterlockedCompareExchange(
                reinterpret_cast<volatile LONG *>(&header->Head),
                static_cast<LONG>(head + ready),
                static_cast<LONG>(head)) != static_cast<LONG>(head))
        {
            continue;
        }

        for (ULONG index = 0; index < ready; ++index)
        {
            const ULONG position = head + index;
            RDMA_CQ_ENTRY &entry = GetEntry(position);
            const NdCompletionTranslation &translation = LookupCompletionTranslation(entry.Event);
            ND2_RESULT &result = results[returned + index];
            result.Status = translation.Status;
            result.BytesTransferred = entry.BytesTransferred;
            result.QueuePairContext = reinterpret_cast<void *>(entry.QueuePairContext);
            result.RequestContext = reinterpret_cast<void *>(entry.RequestContext);
            result.RequestType = translation.RequestType;
            if (FAILED(translation.Status))
            {
                if (translation.Status == ND_UNSUCCESSFUL)
                {
                    // The enum starts at 1 and the driver zero-fills the ring, so an unknown
                    // event usually means the entry was never written - a ring sync bug.
                    NdTrace(
                        "GetResults: unknown completion event %u at position %u - "
                        "entry was likely never written by the driver",
                        static_cast<UINT32>(entry.Event),
                        position);
                }
                NdTrace(
                    "GetResults: error event %u ctx=0x%llX bytes=%u -> status=0x%08X",
                    static_cast<UINT32>(entry.Event),
                    static_cast<UINT64>(entry.RequestContext),
                    static_cast<UINT32>(entry.BytesTransferred),
                    static_cast<UINT32>(translation.Status));
            }
            WriteULongRelease(
                &entry.Sequence,
                position + m_capacity);
        }

        returned += ready;
    }

    return returned;
}



// ============================================================================
// NdMemoryRegion
// ============================================================================

NdMemoryRegion::NdMemoryRegion(
    _In_ NdDeviceContext *deviceContext)
    : m_deviceContext{ deviceContext }
{
}

NdMemoryRegion::~NdMemoryRegion()
{
    if (m_registered)
    {
        const RDMA_DESTROY_MEMORY_REGION_INPUT input{ m_mrKey };
        const HRESULT status = m_deviceContext->SendIoctl(
            IOCTL_RDMA_DESTROY_MEMORY_REGION,
            &input,
            sizeof(input),
            nullptr,
            0);
        if (FAILED(status))
        {
            NdTrace(
                "MR destructor deregistration failed: 0x%08X",
                status);
        }
    }
}

HRESULT NdMemoryRegion::QueryInterface(REFIID riid, LPVOID* ppvObj)
{
    if (InlineIsEqualGUID(riid, IID_IUnknown) || InlineIsEqualGUID(riid, IID_IND2MemoryRegion))
    {
        *ppvObj = static_cast<IND2MemoryRegion*>(this);
        AddRef();
        return S_OK;
    }
    *ppvObj = nullptr;
    return E_NOINTERFACE;
}

HRESULT NdMemoryRegion::CancelOverlappedRequests()
{
    return S_OK;
}

HRESULT NdMemoryRegion::GetOverlappedResult(OVERLAPPED* pOverlapped, BOOL wait)
{
    return S_OK;
}

static
bool
IsPageAligned(
    const void *address)
{
    return (reinterpret_cast<ULONG_PTR>(address) & (RDMA_PAGE_SIZE - 1)) == 0;
}

HRESULT NdMemoryRegion::Register(const VOID* pBuffer, SIZE_T cbBuffer, ULONG flags, OVERLAPPED* pOverlapped)
{
    if ((flags & ~ND_MR_FLAG_ALLOW_LOCAL_WRITE) != 0)
    {
        return ND_NOT_SUPPORTED;
    }

    if (!IsPageAligned(pBuffer))
    {
        return ND_INVALID_PARAMETER_1;
    }

    if ((cbBuffer % RDMA_PAGE_SIZE) != 0)
    {
        return ND_INVALID_PARAMETER_2;
    }

    if (m_registered)
    {
        return ND_DEVICE_BUSY;
    }

    RDMA_CREATE_MEMORY_REGION_INPUT input = {};
    input.Address = reinterpret_cast<UINT64>(pBuffer);
    input.Size = static_cast<UINT64>(cbBuffer);

    RDMA_CREATE_MEMORY_REGION_OUTPUT output = {};
    const HRESULT hr = m_deviceContext->SendIoctl(
        IOCTL_RDMA_CREATE_MEMORY_REGION,
        &input,
        sizeof(input),
        &output,
        sizeof(output));
    if (FAILED(hr))
    {
        NdTrace("MR::Register failed: 0x%08X", hr);
        return hr;
    }

    m_pBuffer = pBuffer;
    m_cbBuffer = cbBuffer;
    m_mrKey = output.MemoryRegionKey;
    m_registered = true;

    NdTrace("MR::Register OK, key=%u", m_mrKey);
    return S_OK;
}

HRESULT NdMemoryRegion::Deregister(OVERLAPPED* pOverlapped)
{
    if (m_registered)
    {
        const RDMA_DESTROY_MEMORY_REGION_INPUT input{ m_mrKey };
        const HRESULT hr = m_deviceContext->SendIoctl(
            IOCTL_RDMA_DESTROY_MEMORY_REGION,
            &input,
            sizeof(input),
            nullptr,
            0);
        if (FAILED(hr))
        {
            NdTrace("MR::Deregister failed: 0x%08X", hr);
            return hr == HRESULT_FROM_WIN32(ERROR_BUSY) ? ND_DEVICE_BUSY : hr;
        }
    }

    m_pBuffer = nullptr;
    m_cbBuffer = 0;
    m_mrKey = RDMA_INVALID_ID;
    m_registered = false;
    return S_OK;
}

UINT32 NdMemoryRegion::GetLocalToken() { return m_mrKey; }
UINT32 NdMemoryRegion::GetRemoteToken() {
    return RDMA_INVALID_ID;
}


// ============================================================================
// NdQueuePair
// ============================================================================

NdQueuePair::NdQueuePair(
    _In_ NdDeviceContext *deviceContext,
    VOID *context,
    NdCompletionQueue *pSendCq,
    NdCompletionQueue *pRecvCq,
    ULONG receiveQueueDepth,
    ULONG maxReceiveRequestSge,
    ULONG maxInitiatorRequestSge)
    : m_deviceContext{ deviceContext }
    , m_context{ context }
    , m_pSendCq{ pSendCq }
    , m_pRecvCq{ pRecvCq }
    , m_sendCqId{ pSendCq != nullptr ? pSendCq->GetId() : RDMA_INVALID_ID }
    , m_recvCqId{ pRecvCq != nullptr ? pRecvCq->GetId() : RDMA_INVALID_ID }
    , m_receiveQueueDepth{ receiveQueueDepth }
    , m_maxReceiveRequestSge{ maxReceiveRequestSge }
    , m_maxInitiatorRequestSge{ maxInitiatorRequestSge }
{
    if (m_pSendCq)
    {
        m_pSendCq->AddRef();
    }
    if (m_pRecvCq)
    {
        m_pRecvCq->AddRef();
    }

    m_deviceContext->RegisterQueuePair(&m_deviceLink);
}

NdQueuePair::~NdQueuePair()
{
    m_deviceContext->UnregisterQueuePair(&m_deviceLink);

    // A binding that was never ended still owns its OVERLAPPED.
    if (m_bindFile)
    {
        CancelIoEx(
            m_bindFile.get(),
            &m_bindOverlapped);
        DWORD bytesReturned = 0;
        GetOverlappedResult(
            m_bindFile.get(),
            &m_bindOverlapped,
            &bytesReturned,
            TRUE);
        m_bindFile.reset();
    }

    AcquireSRWLockExclusive(&m_lock);
    const HRESULT flushStatus = FlushLocked();
    if (FAILED(flushStatus))
    {
        NdTrace(
            "QP destructor flush failed: 0x%08X",
            flushStatus);
    }

    CloseQueuePairFile();
    ReleaseSRWLockExclusive(&m_lock);

    if (m_pSendCq)
    {
        m_pSendCq->Release();
    }
    if (m_pRecvCq)
    {
        m_pRecvCq->Release();
    }
}

// How long Disconnect waits for the peer to release the tunnel. The driver
// stops waiting for the peer after 5 seconds and completes the release, so
// this only expires if the driver itself doesn't answer.
static constexpr DWORD c_queuePairReleaseTimeoutMilliseconds = 10000;

// The driver completes binding requests with an NTSTATUS, which reaches the
// caller as a Win32 error; ND reports NTSTATUS values.
static
HRESULT
MapBindError(
    DWORD error)
{
    switch (error)
    {
        case ERROR_OPERATION_ABORTED:
            return ND_CANCELED;
        case ERROR_SEM_TIMEOUT:
            return ND_IO_TIMEOUT;
        case ERROR_CONNECTION_ABORTED:
            return ND_CONNECTION_ABORTED;
        case ERROR_NO_SYSTEM_RESOURCES:
            return ND_INSUFFICIENT_RESOURCES;
        // The peer has no binding with the connection's token.
        case ERROR_NOT_FOUND:
            return ND_CONNECTION_REFUSED;
        case ERROR_DEVICE_REMOVED:
        case ERROR_DEVICE_NOT_CONNECTED:
            return ND_DEVICE_REMOVED;
        default:
            return HRESULT_FROM_WIN32(error);
    }
}

HRESULT
NdQueuePair::BeginBind(
    const RDMA_CONNECTION_TOKEN &connectionToken,
    RDMA_CONNECTION_ROLE role)
{
    const auto lock = wil::AcquireSRWLockExclusive(&m_lock);

    if (m_disconnected)
    {
        return ND_CONNECTION_INVALID;
    }

    if (m_removed)
    {
        return HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED);
    }

    if (m_queuePairFile || m_bindFile)
    {
        return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
    }

    if (!m_bindEvent && !m_bindEvent.try_create(
                            wil::EventOptions::ManualReset,
                            nullptr))
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    if (!m_releaseEvent && !m_releaseEvent.try_create(
                               wil::EventOptions::ManualReset,
                               nullptr))
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    wil::unique_hfile queuePairFile{ CreateFileW(
        m_deviceContext->GetQueuePairPath(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED,
        nullptr) };
    if (!queuePairFile)
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    RDMA_CREATE_QUEUE_PAIR_INPUT input = {};
    input.QueuePairContext = reinterpret_cast<UINT64>(m_context);
    input.ConnectionToken = connectionToken;
    input.SendCompletionQueueId = m_sendCqId;
    input.ReceiveCompletionQueueId = m_recvCqId;
    input.Role = role;

    m_bindOutput = {};
    m_bindOverlapped = {};
    m_bindOverlapped.hEvent = m_bindEvent.get();
    m_bindEvent.ResetEvent();

    DWORD error = ERROR_SUCCESS;
    {
        // Keeps the RDMA handle open while the driver references it, which it
        // does before the request returns.
        NdDeviceUsageGuard deviceUsage{ m_deviceContext.Get() };
        if (!deviceUsage.IsAvailable())
        {
            error = ERROR_DEVICE_NOT_CONNECTED;
        }
        else
        {
            input.RdmaHandle = static_cast<UINT64>(reinterpret_cast<ULONG_PTR>(deviceUsage.GetHandle()));
            if (!DeviceIoControl(
                    queuePairFile.get(),
                    IOCTL_RDMA_CREATE_QUEUE_PAIR,
                    &input,
                    sizeof(input),
                    &m_bindOutput,
                    sizeof(m_bindOutput),
                    nullptr,
                    &m_bindOverlapped))
            {
                error = GetLastError();
            }
        }
    }

    if (error != ERROR_SUCCESS && error != ERROR_IO_PENDING)
    {
        NdTrace("QP::BeginBind CreateQueuePair IOCTL failed: %lu", error);
        return MapBindError(error);
    }

    m_bindFile = std::move(queuePairFile);
    return S_OK;
}

HRESULT
NdQueuePair::EndBind(
    bool cancel)
{
    // Only CloseForRemoval uses the handle concurrently, to cancel the
    // binding.
    const HANDLE bindFile = m_bindFile.get();
    if (bindFile == nullptr)
    {
        return E_UNEXPECTED;
    }

    if (cancel)
    {
        CancelIoEx(
            bindFile,
            &m_bindOverlapped);
    }

    HRESULT hr = S_OK;
    DWORD bytesReturned = 0;
    if (!GetOverlappedResult(
            bindFile,
            &m_bindOverlapped,
            &bytesReturned,
            TRUE))
    {
        hr = MapBindError(GetLastError());
    }

    const auto lock = wil::AcquireSRWLockExclusive(&m_lock);

    wil::unique_hfile queuePairFile = std::move(m_bindFile);

    // CloseForRemoval cancels the request, which then fails with
    // ND_CANCELED.
    if (m_removed)
    {
        hr = ND_DEVICE_REMOVED;
    }

    if (FAILED(hr))
    {
        NdTrace(
            "QP bind failed: 0x%08X",
            hr);
        return hr;
    }

    m_queuePairFile = std::move(queuePairFile);
    NdTrace(
        "QP bound, id=%llu, tunnel=%u",
        m_bindOutput.QueuePairId,
        m_bindOutput.TunnelIndex);

    for (ULONG index = 0; index < m_pendingReceiveCount; ++index)
    {
        const PendingReceive &pendingReceive = m_pendingReceives[index];
        hr = PostReceive(
            pendingReceive.RequestContext,
            pendingReceive.Sge);
        if (FAILED(hr))
        {
            for (ULONG pendingIndex = index; pendingIndex < m_pendingReceiveCount; ++pendingIndex)
            {
                m_pendingReceives[pendingIndex - index] = m_pendingReceives[pendingIndex];
            }
            m_pendingReceiveCount -= index;

            const HRESULT flushStatus = FlushLocked();
            if (FAILED(flushStatus))
            {
                NdTrace(
                    "QP bind failure flush failed: 0x%08X",
                    flushStatus);
            }

            CloseQueuePairFile();
            return hr;
        }
    }

    m_pendingReceiveCount = 0;
    StartReleaseNotificationLocked();

    NdTrace(
        "QP bound to tunnel %u, id=%I64u",
        m_bindOutput.TunnelIndex,
        m_bindOutput.QueuePairId);

    return S_OK;
}

void
NdQueuePair::StartReleaseNotificationLocked()
{
    m_releaseOutput = {};
    m_releaseOverlapped = {};
    m_releaseOverlapped.hEvent = m_releaseEvent.get();
    m_releaseEvent.ResetEvent();
    m_releaseStatus = ND_PENDING;

    if (DeviceIoControl(
            m_queuePairFile.get(),
            IOCTL_RDMA_NOTIFY_QUEUE_PAIR_RELEASE,
            nullptr,
            0,
            &m_releaseOutput,
            sizeof(m_releaseOutput),
            nullptr,
            &m_releaseOverlapped) ||
        GetLastError() == ERROR_IO_PENDING)
    {
        m_releasePending = true;
        return;
    }

    // Without the notification, the connection can't tell when it ends.
    NdTrace(
        "QP release notification failed: %lu",
        GetLastError());
    m_releaseStatus = ND_CONNECTION_ABORTED;
    m_releaseEvent.SetEvent();
}

void
NdQueuePair::UpdateReleaseStatusLocked(
    BOOL wait)
{
    if (!m_releasePending)
    {
        return;
    }

    DWORD bytesReturned = 0;
    if (GetOverlappedResult(
            m_queuePairFile.get(),
            &m_releaseOverlapped,
            &bytesReturned,
            wait))
    {
        m_releaseStatus = bytesReturned >= sizeof(m_releaseOutput) && m_releaseOutput.Reason == RdmaReleaseReasonDisconnect
                              ? S_OK
                              : ND_CONNECTION_ABORTED;
    }
    else if (GetLastError() == ERROR_IO_INCOMPLETE)
    {
        return;
    }
    else
    {
        // Closing the handle cancels the notification.
        m_releaseStatus = ND_CONNECTION_ABORTED;
    }

    m_releasePending = false;
}

HRESULT
NdQueuePair::GetReleaseStatus()
{
    const auto lock = wil::AcquireSRWLockExclusive(&m_lock);
    UpdateReleaseStatusLocked(FALSE);
    return m_releaseStatus == ND_PENDING ? ND_CONNECTION_ABORTED : m_releaseStatus;
}

void
NdQueuePair::AbortBinding()
{
    const auto lock = wil::AcquireSRWLockExclusive(&m_lock);
    const HRESULT status = FlushLocked();
    if (FAILED(status))
    {
        NdTrace(
            "QP abort flush failed: 0x%08X",
            status);
    }

    CloseQueuePairFile();
}

void
NdQueuePair::CloseQueuePairFile()
{
    if (m_queuePairFile)
    {
        // The notification completes before its OVERLAPPED can go away.
        if (m_releasePending)
        {
            CancelIoEx(
                m_queuePairFile.get(),
                &m_releaseOverlapped);
            UpdateReleaseStatusLocked(TRUE);
        }

        m_queuePairFile.reset();
    }
}

void
NdQueuePair::CloseForRemoval()
{
    const auto lock = wil::AcquireSRWLockExclusive(&m_lock);
    m_removed = true;

    // EndBind closes the handle of a binding in progress.
    if (m_bindFile)
    {
        CancelIoEx(
            m_bindFile.get(),
            &m_bindOverlapped);
    }

    CloseQueuePairFile();
}

NdQueuePair *
NdQueuePair::FromDeviceLink(
    _In_ LIST_ENTRY *entry)
{
    return CONTAINING_RECORD(
        entry,
        NdQueuePair,
        m_deviceLink);
}

HRESULT NdQueuePair::QueryInterface(REFIID riid, LPVOID* ppvObj)
{
    if (InlineIsEqualGUID(riid, IID_IND2QueuePair))
    {
        *ppvObj = static_cast<IND2QueuePair*>(this);
        AddRef();
        return S_OK;
    }
    else if (InlineIsEqualGUID(riid, IID_IUnknown))
    {
        *ppvObj = static_cast<IUnknown*>(this);
        AddRef();
        return S_OK;
    }

    *ppvObj = nullptr;
    return E_NOINTERFACE;
}

HRESULT NdQueuePair::Flush()
{
    AcquireSRWLockExclusive(&m_lock);
    const HRESULT status = FlushLocked();
    ReleaseSRWLockExclusive(&m_lock);
    return status;
}

HRESULT
NdQueuePair::FlushLocked(
    UINT32 flags)
{
    if (m_queuePairFile)
    {
        RDMA_FLUSH_QUEUE_PAIR_INPUT input = {};
        input.Flags = flags;
        const HRESULT status = ::SendIoctl(
            m_queuePairFile.get(),
            IOCTL_RDMA_FLUSH_QUEUE_PAIR,
            flags != 0 ? &input : nullptr,
            flags != 0 ? static_cast<DWORD>(sizeof(input)) : 0,
            nullptr,
            0);
        if (FAILED(status))
        {
            return status;
        }
    }

    if (m_pendingReceiveCount != 0 && m_pRecvCq != nullptr)
    {
        constexpr size_t headerSize = offsetof(
            RDMA_CANCEL_UNPOSTED_REQUESTS_INPUT,
            RequestContexts);
        alignas(RDMA_CANCEL_UNPOSTED_REQUESTS_INPUT) BYTE buffer[headerSize + c_maxPendingReceives * sizeof(UINT64)]{};
        RDMA_CANCEL_UNPOSTED_REQUESTS_INPUT *const input = reinterpret_cast<RDMA_CANCEL_UNPOSTED_REQUESTS_INPUT *>(buffer);
        input->CompletionQueueId = m_recvCqId;
        input->Event = RdmaCompletionEventTypeReceiveFlushError;
        input->QueuePairContext = reinterpret_cast<UINT64>(m_context);
        input->RequestCount = m_pendingReceiveCount;
        for (ULONG index = 0; index < m_pendingReceiveCount; ++index)
        {
            input->RequestContexts[index] = reinterpret_cast<UINT64>(m_pendingReceives[index].RequestContext);
        }

        const DWORD inputSize = static_cast<DWORD>(headerSize + m_pendingReceiveCount * sizeof(UINT64));
        const HRESULT status = m_deviceContext->SendIoctl(
            IOCTL_RDMA_CANCEL_UNPOSTED_REQUESTS,
            input,
            inputSize,
            nullptr,
            0);
        if (FAILED(status))
        {
            return status;
        }
    }

    m_pendingReceiveCount = 0;
    return S_OK;
}

HRESULT
NdQueuePair::Disconnect()
{
    HANDLE releaseEvent = nullptr;
    {
        const auto lock = wil::AcquireSRWLockExclusive(&m_lock);
        if (m_disconnected)
        {
            return S_OK;
        }

        // The driver releases the tunnel, which retires the peer's queue pair
        // and reports a disconnection to its NotifyDisconnect.
        const HRESULT flushStatus = FlushLocked(RDMA_FLUSH_FLAG_DISCONNECT);
        if (FAILED(flushStatus))
        {
            return flushStatus;
        }

        if (m_releasePending)
        {
            releaseEvent = m_releaseEvent.get();
        }
    }

    // Waits for the peer's release, so the tunnel is free again when
    // Disconnect completes.
    HRESULT status = S_OK;
    if (releaseEvent != nullptr && WaitForSingleObject(
                                       releaseEvent,
                                       c_queuePairReleaseTimeoutMilliseconds) != WAIT_OBJECT_0)
    {
        status = ND_IO_TIMEOUT;
    }

    const auto lock = wil::AcquireSRWLockExclusive(&m_lock);

    // The driver completed the release without the peer's answer.
    UpdateReleaseStatusLocked(FALSE);
    if (!m_releasePending && (m_releaseOutput.Flags & RDMA_RELEASE_FLAG_UNANSWERED) != 0)
    {
        status = ND_IO_TIMEOUT;
    }

    CloseQueuePairFile();
    m_disconnected = true;
    return status;
}

// The driver fails Sends and Receives on a queue pair whose completion queue
// failed with STATUS_CONNECTION_INVALID, which reaches the caller as a Win32
// error.
static
HRESULT
MapDataPathError(
    HRESULT status)
{
    return status == HRESULT_FROM_WIN32(ERROR_CONNECTION_INVALID) ? ND_CONNECTION_INVALID : status;
}

HRESULT
NdQueuePair::GetDataPathStatus() const
{
    HRESULT hr = S_OK;

    // A failed completion queue fails the queue pair, which can't report
    // completions anymore.
    if ((m_pSendCq != nullptr && m_pSendCq->IsFailed()) || (m_pRecvCq != nullptr && m_pRecvCq->IsFailed()) || m_disconnected)
    {
        hr = ND_CONNECTION_INVALID;
    }
    else if (m_removed)
    {
        hr = HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED);
    }

    return hr;
}

HRESULT NdQueuePair::Send(VOID* requestContext, const ND2_SGE sge[], ULONG nSge, ULONG flags)
{
    if (nSge > m_maxInitiatorRequestSge)
    {
        return ND_DATA_OVERRUN;
    }

    if (nSge > 0 && !IsPageAligned(sge[0].Buffer))
    {
        return ND_INVALID_PARAMETER_2;
    }

    const auto lock = wil::AcquireSRWLockShared(&m_lock);
    HRESULT hr = GetDataPathStatus();
    if (SUCCEEDED(hr) && !m_queuePairFile)
    {
        hr = HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
    }

    if (SUCCEEDED(hr))
    {

        RDMA_SEND_INPUT input = {};
        input.RequestContext = reinterpret_cast<UINT64>(requestContext);
        if (WI_IsFlagSet(
                flags,
                ND_OP_FLAG_SEND_AND_SOLICIT_EVENT))
        {
            WI_SetFlag(
                input.Flags,
                RDMA_SEND_FLAG_SOLICITED);
        }

        if (WI_IsFlagSet(
                flags,
                ND_OP_FLAG_SILENT_SUCCESS))
        {
            WI_SetFlag(
                input.Flags,
                RDMA_SEND_FLAG_SILENT_SUCCESS);
        }
        if (nSge > 0)
        {
            input.MemoryRegionKey = sge[0].MemoryRegionToken;
            input.Address = reinterpret_cast<UINT64>(sge[0].Buffer);
            input.Length = sge[0].BufferLength;
        }
        else
        {
            input.MemoryRegionKey = RDMA_INVALID_ID;
        }

        hr = MapDataPathError(
            ::SendDataPathIoctl(
                m_queuePairFile.get(),
                IOCTL_RDMA_SEND,
                &input,
                sizeof(input)));
    }

    return hr;
}

HRESULT NdQueuePair::Receive(VOID* requestContext, const ND2_SGE sge[], ULONG nSge)
{
    if (nSge == 0)
    {
        return E_INVALIDARG;
    }

    if (nSge > m_maxReceiveRequestSge)
    {
        return ND_DATA_OVERRUN;
    }

    if (!IsPageAligned(sge[0].Buffer))
    {
        return ND_INVALID_PARAMETER_2;
    }

    // A connected queue pair posts receives concurrently, like Sends.
    {
        const auto lock = wil::AcquireSRWLockShared(&m_lock);
        if (m_queuePairFile)
        {
            HRESULT hr = GetDataPathStatus();
            if (SUCCEEDED(hr))
            {
                hr = PostReceive(
                    requestContext,
                    sge[0]);
            }

            return hr;
        }
    }

    // Before the connection, receives wait in the provider and are posted
    // when it completes.
    const auto lock = wil::AcquireSRWLockExclusive(&m_lock);
    HRESULT hr = GetDataPathStatus();
    if (SUCCEEDED(hr))
    {
        if (m_queuePairFile)
        {
            hr = PostReceive(
                requestContext,
                sge[0]);
        }
        else if (m_pendingReceiveCount >= m_receiveQueueDepth || m_pendingReceiveCount >= c_maxPendingReceives)
        {
            hr = E_OUTOFMEMORY;
        }
        else
        {
            PendingReceive &pendingReceive = m_pendingReceives[m_pendingReceiveCount];
            pendingReceive.RequestContext = requestContext;
            pendingReceive.Sge = sge[0];
            m_pendingReceiveCount++;
        }
    }

    return hr;
}
HRESULT
NdQueuePair::PostReceive(
    VOID *requestContext,
    const ND2_SGE &sge)
{

    RDMA_RECEIVE_INPUT input = {};
    input.MemoryRegionKey = sge.MemoryRegionToken;
    input.RequestContext = reinterpret_cast<UINT64>(requestContext);
    input.Address = reinterpret_cast<UINT64>(sge.Buffer);
    input.Length = sge.BufferLength;

    return MapDataPathError(
        ::SendDataPathIoctl(
            m_queuePairFile.get(),
            IOCTL_RDMA_RECEIVE,
            &input,
            sizeof(input)));
}

HRESULT NdQueuePair::Bind(VOID* requestContext, IUnknown* pMemoryRegion, IUnknown* pMemoryWindow,
    const VOID* pBuffer, SIZE_T cbBuffer, ULONG flags)
{
    return ND_NOT_SUPPORTED;
}

HRESULT NdQueuePair::Invalidate(VOID* requestContext, IUnknown* pMemoryWindow, ULONG flags)
{
    return ND_NOT_SUPPORTED;
}

HRESULT NdQueuePair::Read(VOID* requestContext, const ND2_SGE sge[], ULONG nSge,
    UINT64 remoteAddress, UINT32 remoteToken, ULONG flags)
{
    return ND_NOT_SUPPORTED;
}

HRESULT NdQueuePair::Write(VOID* requestContext, const ND2_SGE sge[], ULONG nSge,
    UINT64 remoteAddress, UINT32 remoteToken, ULONG flags)
{
    return ND_NOT_SUPPORTED;
}

// ============================================================================
// NdOverlappedRequests
// ============================================================================

HRESULT
NdOverlappedRequests::Initialize(
    HANDLE overlappedFile)
{
    if (!DuplicateHandle(
            GetCurrentProcess(),
            overlappedFile,
            GetCurrentProcess(),
            m_overlappedFile.put(),
            0,
            FALSE,
            DUPLICATE_SAME_ACCESS))
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    return S_OK;
}

// Without the low bit, which only tells the I/O manager not to queue a packet.
static
HANDLE
GetOverlappedEvent(
    _In_ const OVERLAPPED *overlapped)
{
    return reinterpret_cast<HANDLE>(reinterpret_cast<ULONG_PTR>(overlapped->hEvent) & ~static_cast<ULONG_PTR>(1));
}

void
NdOverlappedRequests::Start(
    OVERLAPPED *overlapped)
{
    overlapped->InternalHigh = 0;
    WriteULongPtrRelease(
        &overlapped->Internal,
        STATUS_PENDING);
    const HANDLE event = GetOverlappedEvent(overlapped);
    if (event != nullptr)
    {
        ResetEvent(event);
    }
}

HRESULT
NdOverlappedRequests::Complete(
    OVERLAPPED *overlapped,
    HRESULT status) const
{
    // ND statuses are NTSTATUS values. The driver can't complete a request
    // with STATUS_PENDING or STATUS_MORE_PROCESSING_REQUIRED, which a peer's
    // status could carry.
    constexpr HRESULT c_statusMoreProcessingRequired = static_cast<HRESULT>(0xC0000016L);
    RDMA_COMPLETE_OVERLAPPED_INPUT input{};
    // ND results are NTSTATUS values; other HRESULTs pass through unchanged.
    input.Status =
        static_cast<NTSTATUS>(status == ND_PENDING || status == c_statusMoreProcessingRequired ? ND_INTERNAL_ERROR : status);

    // KMDF marks the request pending before the driver completes it, so the
    // I/O manager reports it like any pended request, also with
    // FILE_SKIP_COMPLETION_PORT_ON_SUCCESS: it stores the status, signals the
    // event, and queues a packet unless the event has its low bit set.
    if (DeviceIoControl(
            m_overlappedFile.get(),
            IOCTL_RDMA_COMPLETE_OVERLAPPED,
            &input,
            sizeof(input),
            nullptr,
            0,
            nullptr,
            overlapped))
    {
        return input.Status;
    }

    const DWORD error = GetLastError();
    if (error == ERROR_IO_PENDING)
    {
        return ND_PENDING;
    }

    // The request never reached the driver. Report the result through the
    // OVERLAPPED as far as possible; the caller returns it too.
    NdTrace(
        "Completing an overlapped request with 0x%08X failed: %lu",
        input.Status,
        error);
    WriteULongPtrRelease(
        &overlapped->Internal,
        static_cast<ULONG_PTR>(static_cast<ULONG>(input.Status)));
    const HANDLE event = GetOverlappedEvent(overlapped);
    if (event != nullptr)
    {
        SetEvent(event);
    }

    return input.Status;
}

HRESULT
NdOverlappedRequests::GetResult(
    OVERLAPPED *overlapped,
    BOOL wait)
{
    if (overlapped == nullptr)
    {
        return E_INVALIDARG;
    }

    // The status decides, not the event: the application may have consumed
    // the event's signal, or share the event between requests.
    while (ReadOverlappedStatus(overlapped) == STATUS_PENDING)
    {
        if (!wait)
        {
            return ND_PENDING;
        }

        const HANDLE event = GetOverlappedEvent(overlapped);
        if (event == nullptr)
        {
            WaitForOverlappedWithoutEvent(overlapped);
        }
        else if (WaitForSingleObject(
                     event,
                     INFINITE) == WAIT_FAILED)
        {
            return HRESULT_FROM_WIN32(GetLastError());
        }
    }

    return static_cast<HRESULT>(ReadOverlappedStatus(overlapped));
}

// ============================================================================
// NdWork
// ============================================================================

void
NdWork::Submit(
    wistd::unique_ptr<NdWork> work)
{
    HMODULE module = nullptr;
    if (GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(&ThreadPoolCallback),
            &module))
    {
        work->m_module = module;
        if (TrySubmitThreadpoolCallback(
                ThreadPoolCallback,
                work.get(),
                nullptr))
        {
            work.release();
            return;
        }

        FreeLibrary(module);
    }

    work->Run();
}

void CALLBACK
NdWork::ThreadPoolCallback(
    PTP_CALLBACK_INSTANCE instance,
    void *context)
{
    // Connection management blocks on the network.
    CallbackMayRunLong(instance);

    wistd::unique_ptr<NdWork> work{ static_cast<NdWork *>(context) };
    const HMODULE module = work->m_module;
    work->Run();

    // Deleting the work can release the last reference to a connector or
    // listener, so the module is released only after the callback returns.
    work.reset();
    FreeLibraryWhenCallbackReturns(
        instance,
        module);
}


// ============================================================================

// ============================================================================
// NdConnector — TCP-based connection management
// ============================================================================

static volatile LONG64 s_connectionTokenSequence = 0;
static constexpr DWORD c_cmNegotiationTimeoutMilliseconds{ 10000 };

static
HRESULT
GetCmAddress(
    const struct sockaddr_in &serviceAddress,
    struct sockaddr_in &cmAddress)
{
    const DWORD servicePort = ntohs(serviceAddress.sin_port);
    const DWORD cmPortOffset = GetProviderConfiguration().cmPortOffset;
    if (cmPortOffset > c_maxTcpPort - servicePort)
    {
        return E_INVALIDARG;
    }

    cmAddress = serviceAddress;
    cmAddress.sin_port = htons(static_cast<USHORT>(servicePort + cmPortOffset));
    return S_OK;
}

static
void
ApplyZeroPortFallback(
    _In_z_ const char *operation,
    struct sockaddr_in &serviceAddress)
{
    const DWORD fallbackServicePort = GetProviderConfiguration().zeroPortFallbackServicePort;
    if (serviceAddress.sin_port != 0 || fallbackServicePort == 0)
    {
        return;
    }

    serviceAddress.sin_port = htons(static_cast<USHORT>(fallbackServicePort));
    NdTrace(
        "%s substituted zero service port: servicePort=%lu",
        operation,
        fallbackServicePort);
}

static
void
LogCmSocketFailure(
    _In_z_ const char *operation,
    const struct sockaddr_in &serviceAddress,
    const struct sockaddr_in &cmAddress,
    int error)
{
    const ULONG address = ntohl(serviceAddress.sin_addr.s_addr);
    NdTrace(
        "%s failed: address=%u.%u.%u.%u, servicePort=%u, "
        "cmPort=%u, wsaError=%d",
        operation,
        static_cast<unsigned int>((address >> 24) & 0xff),
        static_cast<unsigned int>((address >> 16) & 0xff),
        static_cast<unsigned int>((address >> 8) & 0xff),
        static_cast<unsigned int>(address & 0xff),
        static_cast<unsigned int>(ntohs(serviceAddress.sin_port)),
        static_cast<unsigned int>(ntohs(cmAddress.sin_port)),
        error);
}

static
void
LogSocketAddress(
    _In_z_ const char *operation,
    _In_reads_bytes_opt_(addressLength) const struct sockaddr *address,
    ULONG addressLength)
{
    if (address == nullptr)
    {
        NdTrace(
            "%s: address=null, addressLength=%lu",
            operation,
            addressLength);
        return;
    }

    const ADDRESS_FAMILY family = addressLength >= sizeof(address->sa_family) ? address->sa_family : AF_UNSPEC;
    if (addressLength < sizeof(struct sockaddr_in) || family != AF_INET)
    {
        NdTrace(
            "%s: addressLength=%lu, family=%u",
            operation,
            addressLength,
            static_cast<unsigned int>(family));
        return;
    }

    const struct sockaddr_in *const ipv4Address = reinterpret_cast<const struct sockaddr_in *>(address);
    const ULONG addressValue = ntohl(ipv4Address->sin_addr.s_addr);
    NdTrace(
        "%s: address=%u.%u.%u.%u, port=%u, "
        "addressLength=%lu, family=%u",
        operation,
        static_cast<unsigned int>((addressValue >> 24) & 0xff),
        static_cast<unsigned int>((addressValue >> 16) & 0xff),
        static_cast<unsigned int>((addressValue >> 8) & 0xff),
        static_cast<unsigned int>(addressValue & 0xff),
        static_cast<unsigned int>(ntohs(ipv4Address->sin_port)),
        addressLength,
        static_cast<unsigned int>(ipv4Address->sin_family));
}

static
void
LogSocketAddressFailure(
    _In_z_ const char *operation,
    const struct sockaddr_in &address,
    int error)
{
    const ULONG ipv4Address = ntohl(address.sin_addr.s_addr);
    NdTrace(
        "%s failed: address=%u.%u.%u.%u, port=%u, "
        "wsaError=%d",
        operation,
        static_cast<unsigned int>((ipv4Address >> 24) & 0xff),
        static_cast<unsigned int>((ipv4Address >> 16) & 0xff),
        static_cast<unsigned int>((ipv4Address >> 8) & 0xff),
        static_cast<unsigned int>(ipv4Address & 0xff),
        static_cast<unsigned int>(ntohs(address.sin_port)),
        error);
}

static
HRESULT
SetCmSocketTimeouts(
    SOCKET socket,
    DWORD receiveTimeoutMilliseconds,
    DWORD sendTimeoutMilliseconds)
{
    if (setsockopt(
            socket,
            SOL_SOCKET,
            SO_RCVTIMEO,
            reinterpret_cast<const char *>(&receiveTimeoutMilliseconds),
            sizeof(receiveTimeoutMilliseconds)) == SOCKET_ERROR)
    {
        const int error = WSAGetLastError();
        NdTrace(
            "setsockopt(SO_RCVTIMEO) failed: socket=%llu, "
            "timeoutMs=%lu, wsaError=%d",
            static_cast<unsigned long long>(socket),
            receiveTimeoutMilliseconds,
            error);
        return HRESULT_FROM_WIN32(error);
    }

    if (setsockopt(
            socket,
            SOL_SOCKET,
            SO_SNDTIMEO,
            reinterpret_cast<const char *>(&sendTimeoutMilliseconds),
            sizeof(sendTimeoutMilliseconds)) == SOCKET_ERROR)
    {
        const int error = WSAGetLastError();
        NdTrace(
            "setsockopt(SO_SNDTIMEO) failed: socket=%llu, "
            "timeoutMs=%lu, wsaError=%d",
            static_cast<unsigned long long>(socket),
            sendTimeoutMilliseconds,
            error);
        return HRESULT_FROM_WIN32(error);
    }

    return S_OK;
}

// Ends waits on a connection whose peer machine stops responding. After 10 s
// without traffic, TCP sends up to 10 probes 1 s apart, then aborts the
// connection.
static
HRESULT
EnableCmKeepAlive(
    SOCKET socket)
{
    tcp_keepalive settings{};
    settings.onoff = 1;
    settings.keepalivetime = 10000;
    settings.keepaliveinterval = 1000;
    DWORD bytesReturned = 0;
    if (WSAIoctl(
            socket,
            SIO_KEEPALIVE_VALS,
            &settings,
            sizeof(settings),
            nullptr,
            0,
            &bytesReturned,
            nullptr,
            nullptr) == SOCKET_ERROR)
    {
        const int error = WSAGetLastError();
        NdTrace(
            "WSAIoctl(SIO_KEEPALIVE_VALS) failed: socket=%llu, "
            "wsaError=%d",
            static_cast<unsigned long long>(socket),
            error);
        return HRESULT_FROM_WIN32(error);
    }

    return S_OK;
}

static
bool
AreConnectionTokensEqual(
    const RDMA_CONNECTION_TOKEN &first,
    const RDMA_CONNECTION_TOKEN &second)
{
    return first.High == second.High && first.Low == second.Low;
}

NdConnector::NdConnector(
    _In_ NdDeviceContext *deviceContext)
    : m_deviceContext{ deviceContext }
    , m_cancelEvent{ CreateEventW(
          nullptr,
          TRUE,
          FALSE,
          nullptr) }
{
}

// Reliable TCP send: loops until all bytes are sent.
bool
NdConnector::SendMsg(
    SOCKET s,
    const void *buf,
    int len,
    _Out_opt_ int *socketError)
{
    if (socketError != nullptr)
    {
        *socketError = 0;
    }
    const char* p = static_cast<const char*>(buf);
    int remaining = len;
    while (remaining > 0)
    {
        int sent = send(s, p, remaining, 0);
        if (sent == SOCKET_ERROR || sent == 0)
        {
            if (socketError != nullptr && sent == SOCKET_ERROR)
            {
                *socketError = WSAGetLastError();
            }
            return false;
        }
        p += sent;
        remaining -= sent;
    }
    return true;
}

// Reliable TCP recv: loops until all bytes are received.
bool
NdConnector::RecvMsg(
    SOCKET s,
    void *buf,
    int len,
    _Out_opt_ int *socketError)
{
    if (socketError != nullptr)
    {
        *socketError = 0;
    }
    char* p = static_cast<char*>(buf);
    int remaining = len;
    while (remaining > 0)
    {
        int received = recv(s, p, remaining, 0);
        if (received == SOCKET_ERROR || received == 0)
        {
            if (socketError != nullptr && received == SOCKET_ERROR)
            {
                *socketError = WSAGetLastError();
            }
            return false;
        }
        p += received;
        remaining -= received;
    }
    return true;
}

bool
NdConnector::SendCmMessage(
    const void *buffer,
    int length)
{
    int socketError = 0;
    if (SendMsg(
            m_socket.get(),
            buffer,
            length,
            &socketError))
    {
        return true;
    }

    m_cmTimedOut = m_cmTimedOut || socketError == WSAETIMEDOUT;
    return false;
}

bool
NdConnector::ReceiveCmMessage(
    void *buffer,
    int length)
{
    int socketError = 0;
    if (RecvMsg(
            m_socket.get(),
            buffer,
            length,
            &socketError))
    {
        return true;
    }

    m_cmTimedOut = m_cmTimedOut || socketError == WSAETIMEDOUT;
    return false;
}

// Waits without a time limit, because the peer answers only once its
// application calls Accept or Reject; until then the peer provider doesn't
// read the socket, so shutting it down doesn't end a blocked receive. The wait
// ends when the answer arrives, when the connection fails (keepalive covers a
// peer machine that stops responding), or when CancelOverlappedRequests sets
// m_cancelEvent.
HRESULT
NdConnector::WaitForPeerApplication()
{
    const wil::unique_handle socketEvent{ CreateEventW(
        nullptr,
        TRUE,
        FALSE,
        nullptr) };
    if (!socketEvent)
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    if (WSAEventSelect(
            m_socket.get(),
            socketEvent.get(),
            FD_READ | FD_CLOSE) == SOCKET_ERROR)
    {
        return HRESULT_FROM_WIN32(WSAGetLastError());
    }

    const HANDLE events[] = { socketEvent.get(),
                              m_cancelEvent.get() };
    const DWORD waitResult = WaitForMultipleObjects(
        ARRAYSIZE(events),
        events,
        FALSE,
        INFINITE);
    const DWORD waitError = GetLastError();

    // WSAEventSelect made the socket nonblocking; the receives that follow
    // block, with the negotiation timeout.
    u_long nonblocking = 0;
    if (WSAEventSelect(
            m_socket.get(),
            nullptr,
            0) == SOCKET_ERROR ||
        ioctlsocket(
            m_socket.get(),
            FIONBIO,
            &nonblocking) == SOCKET_ERROR)
    {
        return HRESULT_FROM_WIN32(WSAGetLastError());
    }

    switch (waitResult)
    {
        case WAIT_OBJECT_0:
            return S_OK;
        case WAIT_OBJECT_0 + 1:
            return ND_CANCELED;
        default:
            return HRESULT_FROM_WIN32(waitError);
    }
}

RDMA_CONNECTION_TOKEN
NdConnector::GenerateConnectionToken()
{
    LARGE_INTEGER performanceCounter = {};
    QueryPerformanceCounter(&performanceCounter);

    const UINT64 processId = static_cast<UINT64>(GetCurrentProcessId());
    const UINT64 threadId = static_cast<UINT64>(GetCurrentThreadId());
    const UINT64 sequence = static_cast<UINT64>(InterlockedIncrement64(&s_connectionTokenSequence));

    RDMA_CONNECTION_TOKEN connectionToken = {};
    connectionToken.High = (processId << 32) | threadId;
    connectionToken.Low = static_cast<UINT64>(performanceCounter.QuadPart) ^ (GetTickCount64() << 16) ^ sequence;

    if (connectionToken.High == 0 && connectionToken.Low == 0)
    {
        connectionToken.Low = 1;
    }

    return connectionToken;
}

bool
NdConnector::ReceiveRejectData()
{
    struct
    {
        UINT32 PrivateDataLength;
    } rejectFields = {};

    if (!ReceiveCmMessage(
            &rejectFields,
            sizeof(rejectFields)) ||
        rejectFields.PrivateDataLength > NDPROV_MAX_PRIVATE_DATA)
    {
        return false;
    }

    m_peerPrivateDataLength = rejectFields.PrivateDataLength;
    return m_peerPrivateDataLength == 0 || ReceiveCmMessage(
                                               m_peerPrivateData,
                                               m_peerPrivateDataLength);
}

NdConnector::~NdConnector()
{
    const HRESULT status = DisconnectWorker();
    if (FAILED(status) && status != ND_CONNECTION_INVALID)
    {
        NdTrace(
            "Connector destructor disconnect failed: 0x%08X",
            status);
    }

    StopReleaseWait();
    if (m_releaseWait != nullptr)
    {
        CloseThreadpoolWait(m_releaseWait);
        m_releaseWait = nullptr;
    }

    CloseSocket();
}

void
NdConnector::CloseSocket()
{
    wil::unique_socket closingSocket;
    AcquireSRWLockExclusive(&m_stateLock);
    closingSocket = std::move(m_socket);
    ReleaseSRWLockExclusive(&m_stateLock);
}

HRESULT
NdConnector::DisconnectAssociatedQueuePair()
{
    Microsoft::WRL::ComPtr<IND2QueuePair> queuePair;
    AcquireSRWLockShared(&m_stateLock);
    queuePair = m_queuePair;
    ReleaseSRWLockShared(&m_stateLock);
    return queuePair ? static_cast<NdQueuePair *>(queuePair.Get())->Disconnect() : S_OK;
}

HRESULT
NdConnector::WaitForBind(
    _In_ NdQueuePair *queuePair)
{
    const HANDLE events[] = { queuePair->GetBindEvent(),
                              m_cancelEvent.get() };
    const DWORD waitResult = WaitForMultipleObjects(
        ARRAYSIZE(events),
        events,
        FALSE,
        c_cmNegotiationTimeoutMilliseconds);
    if (waitResult == WAIT_OBJECT_0)
    {
        return queuePair->EndBind(false);
    }

    const DWORD waitError = GetLastError();

    // A binding that completed anyway is undone, which retires the peer's
    // queue pair too.
    if (SUCCEEDED(queuePair->EndBind(true)))
    {
        queuePair->AbortBinding();
    }

    switch (waitResult)
    {
        case WAIT_OBJECT_0 + 1:
            return ND_CANCELED;
        case WAIT_TIMEOUT:
            return ND_IO_TIMEOUT;
        default:
            return HRESULT_FROM_WIN32(waitError);
    }
}

HRESULT
NdConnector::EstablishConnection(
    _In_ IND2QueuePair *queuePair)
{
    if (m_releaseWait == nullptr)
    {
        m_releaseWait = CreateThreadpoolWait(
            ReleaseWaitCallback,
            this,
            nullptr);
        if (m_releaseWait == nullptr)
        {
            return HRESULT_FROM_WIN32(GetLastError());
        }
    }

    AcquireSRWLockExclusive(&m_stateLock);
    m_connected = true;
    m_everConnected = true;
    m_queuePair = queuePair;
    m_releaseHandled = false;
    m_disconnectStatus = ND_PENDING;
    ReleaseSRWLockExclusive(&m_stateLock);

    SetThreadpoolWait(
        m_releaseWait,
        static_cast<NdQueuePair *>(queuePair)->GetReleaseEvent(),
        nullptr);
    return S_OK;
}

void CALLBACK
NdConnector::ReleaseWaitCallback(
    PTP_CALLBACK_INSTANCE instance,
    void *context,
    PTP_WAIT wait,
    TP_WAIT_RESULT waitResult)
{
    UNREFERENCED_PARAMETER(instance);
    UNREFERENCED_PARAMETER(wait);
    UNREFERENCED_PARAMETER(waitResult);

    static_cast<NdConnector *>(context)->OnQueuePairReleased();
}

void
NdConnector::OnQueuePairReleased()
{
    Microsoft::WRL::ComPtr<IND2QueuePair> queuePair;
    AcquireSRWLockShared(&m_stateLock);
    const bool handled = m_releaseHandled;
    queuePair = m_queuePair;
    ReleaseSRWLockShared(&m_stateLock);
    if (handled || !queuePair)
    {
        return;
    }

    const HRESULT status = static_cast<NdQueuePair *>(queuePair.Get())->GetReleaseStatus();

    AcquireSRWLockExclusive(&m_stateLock);
    if (m_releaseHandled)
    {
        ReleaseSRWLockExclusive(&m_stateLock);
        return;
    }

    m_releaseHandled = true;
    m_connected = false;
    m_disconnectStatus = status;
    OVERLAPPED *const disconnectRequest = m_disconnectRequest;
    m_disconnectRequest = nullptr;
    ReleaseSRWLockExclusive(&m_stateLock);

    NdTrace(
        "Connection released: 0x%08X",
        status);

    if (disconnectRequest != nullptr)
    {
        m_requests.Complete(
            disconnectRequest,
            status);
    }
}

void
NdConnector::StopReleaseWait()
{
    if (m_releaseWait != nullptr)
    {
        SetThreadpoolWait(
            m_releaseWait,
            nullptr,
            nullptr);
        WaitForThreadpoolWaitCallbacks(
            m_releaseWait,
            FALSE);
    }
}

HRESULT
NdConnector::AbortAccept(
    HRESULT status)
{
    NdCmConnectAbort abort = {};
    abort.Header.Type = ND_CM_CONNECT_ABORT;
    abort.Header.Version = NDPROV_CM_VERSION;
    abort.Header.ConnectionToken = m_connectionToken;

    // The status becomes the active side's Connect result, and the active
    // application didn't cancel anything.
    abort.Status = status == ND_CANCELED ? ND_CONNECTION_REFUSED : status;
    if (!SendCmMessage(
            &abort,
            static_cast<int>(sizeof(abort))))
    {
        NdTrace(
            "Conn::Accept failed to send the abort: 0x%08X",
            status);
    }

    CloseSocket();
    return status;
}

HRESULT NdConnector::QueryInterface(REFIID riid, LPVOID* ppvObj)
{
    if (InlineIsEqualGUID(riid, IID_IUnknown) || InlineIsEqualGUID(riid, IID_IND2Connector))
    {
        *ppvObj = static_cast<IND2Connector*>(this);
        AddRef();
        return S_OK;
    }
    *ppvObj = nullptr;
    return E_NOINTERFACE;
}

HRESULT NdConnector::CancelOverlappedRequests()
{
    // The cancel event ends a running Connect or Accept that waits for the
    // peer application or for its binding, and shutting the socket down ends
    // one blocked on the socket. The operation then completes with
    // ND_CANCELED if it fails.
    AcquireSRWLockExclusive(&m_stateLock);
    m_operationCanceled = m_operationInProgress;
    if (m_operationCanceled)
    {
        SetEvent(m_cancelEvent.get());
    }
    OVERLAPPED *const disconnectRequest = m_disconnectRequest;
    m_disconnectRequest = nullptr;
    if (m_socket)
    {
        shutdown(
            m_socket.get(),
            SD_BOTH);
    }
    ReleaseSRWLockExclusive(&m_stateLock);

    if (disconnectRequest != nullptr)
    {
        m_requests.Complete(
            disconnectRequest,
            ND_CANCELED);
    }
    return S_OK;
}

HRESULT NdConnector::GetOverlappedResult(OVERLAPPED* pOverlapped, BOOL wait)
{
    return NdOverlappedRequests::GetResult(
        pOverlapped,
        wait);
}

HRESULT NdConnector::Bind(const struct sockaddr* pAddress, ULONG cbAddress)
{
    LogSocketAddress(
        "Conn::Bind requested",
        pAddress,
        cbAddress);
    if (pAddress == nullptr || cbAddress > sizeof(m_localAddr))
    {
        NdTrace(
            "Conn::Bind rejected: addressLength=%lu, "
            "maximumLength=%zu",
            cbAddress,
            sizeof(m_localAddr));
        return E_INVALIDARG;
    }
    memcpy(&m_localAddr, pAddress, cbAddress);
    m_localAddrLen = cbAddress;
    return S_OK;
}

HRESULT
NdConnector::Initialize(
    HANDLE overlappedFile)
{
    return m_requests.Initialize(overlappedFile);
}

enum class NdConnectorOperation
{
    Connect,
    Accept,
    Disconnect,
};

// A Connect, Accept or Disconnect running on the thread pool, with copies of
// its parameters. It keeps the connector and the queue pair alive.
class NdConnectorWork : public NdWork
{
public:

    NdConnectorWork(
        _In_ NdConnector *connector,
        NdConnectorOperation operation,
        _In_ OVERLAPPED *overlapped)
        : m_connector{ connector }
        , m_operation{ operation }
        , m_overlapped{ overlapped }
    {
    }

    Microsoft::WRL::ComPtr<IUnknown> QueuePair{};
    struct sockaddr_in Destination{};
    ULONG DestinationLength{ 0 };
    ULONG InboundReadLimit{ 0 };
    ULONG OutboundReadLimit{ 0 };
    BYTE PrivateData[NDPROV_MAX_PRIVATE_DATA]{};
    ULONG PrivateDataLength{ 0 };

private:

    void
    Run() override
    {
        HRESULT result = E_UNEXPECTED;
        switch (m_operation)
        {
            case NdConnectorOperation::Connect:
                result = m_connector->ConnectWorker(
                    QueuePair.Get(),
                    reinterpret_cast<const struct sockaddr *>(&Destination),
                    DestinationLength,
                    InboundReadLimit,
                    OutboundReadLimit,
                    PrivateData,
                    PrivateDataLength);
                break;
            case NdConnectorOperation::Accept:
                result = m_connector->AcceptWorker(
                    QueuePair.Get(),
                    InboundReadLimit,
                    OutboundReadLimit,
                    PrivateData,
                    PrivateDataLength);
                break;
            case NdConnectorOperation::Disconnect:
                result = m_connector->DisconnectWorker();
                break;
        }

        m_connector->EndOperation(
            m_overlapped,
            result);
    }

    Microsoft::WRL::ComPtr<NdConnector> m_connector;
    NdConnectorOperation m_operation;
    OVERLAPPED *m_overlapped;
};

// Validates the private data of Connect, Accept or Reject and copies it into
// work.
static
HRESULT
CopyPrivateData(
    _In_reads_bytes_opt_(cbPrivateData) const void *pPrivateData,
    ULONG cbPrivateData,
    _Inout_ NdConnectorWork &work)
{
    if (cbPrivateData > NDPROV_MAX_PRIVATE_DATA)
    {
        return ND_INVALID_BUFFER_SIZE;
    }

    if (cbPrivateData != 0)
    {
        if (pPrivateData == nullptr)
        {
            return E_INVALIDARG;
        }

        memcpy(
            work.PrivateData,
            pPrivateData,
            cbPrivateData);
    }

    work.PrivateDataLength = cbPrivateData;
    return S_OK;
}

HRESULT
NdConnector::StartOperation(
    OVERLAPPED *overlapped,
    wistd::unique_ptr<NdWork> work)
{
    if (!m_cancelEvent)
    {
        return E_OUTOFMEMORY;
    }

    AcquireSRWLockExclusive(&m_stateLock);
    if (m_operationInProgress)
    {
        ReleaseSRWLockExclusive(&m_stateLock);
        return ND_DEVICE_BUSY;
    }

    m_operationInProgress = true;
    m_operationCanceled = false;
    ResetEvent(m_cancelEvent.get());
    ReleaseSRWLockExclusive(&m_stateLock);

    NdOverlappedRequests::Start(overlapped);
    NdWork::Submit(wistd::move(work));
    return ND_PENDING;
}

void
NdConnector::EndOperation(
    OVERLAPPED *overlapped,
    HRESULT result)
{
    // Ended before the request completes, so the application can start the
    // next operation once it sees the completion.
    AcquireSRWLockExclusive(&m_stateLock);
    if (m_operationCanceled && FAILED(result))
    {
        result = ND_CANCELED;
    }
    m_operationInProgress = false;
    m_operationCanceled = false;
    ReleaseSRWLockExclusive(&m_stateLock);

    m_requests.Complete(
        overlapped,
        result);
}

HRESULT NdConnector::Connect(IUnknown* pQueuePair, const struct sockaddr* pDestAddress, ULONG cbDestAddress,
    ULONG inboundReadLimit, ULONG outboundReadLimit,
    const VOID* pPrivateData, ULONG cbPrivateData, OVERLAPPED* pOverlapped)
{
    if (pOverlapped == nullptr || pQueuePair == nullptr || pDestAddress == nullptr || cbDestAddress > sizeof(struct sockaddr_in))
    {
        return E_INVALIDARG;
    }

    wistd::unique_ptr<NdConnectorWork> work = wil::make_unique_nothrow<NdConnectorWork>(
        this,
        NdConnectorOperation::Connect,
        pOverlapped);
    if (!work)
    {
        return E_OUTOFMEMORY;
    }

    const HRESULT result = CopyPrivateData(
        pPrivateData,
        cbPrivateData,
        *work);
    if (FAILED(result))
    {
        return result;
    }

    work->QueuePair = pQueuePair;
    memcpy(
        &work->Destination,
        pDestAddress,
        cbDestAddress);
    work->DestinationLength = cbDestAddress;
    work->InboundReadLimit = inboundReadLimit;
    work->OutboundReadLimit = outboundReadLimit;
    return StartOperation(
        pOverlapped,
        wistd::move(work));
}

HRESULT
NdConnector::ConnectWorker(
    IUnknown *pQueuePair,
    const struct sockaddr *pDestAddress,
    ULONG cbDestAddress,
    ULONG inboundReadLimit,
    ULONG outboundReadLimit,
    const void *pPrivateData,
    ULONG cbPrivateData)
{
    UNREFERENCED_PARAMETER(inboundReadLimit);
    UNREFERENCED_PARAMETER(outboundReadLimit);

    m_cmTimedOut = false;
    LogSocketAddress(
        "Conn::Connect destination requested",
        pDestAddress,
        cbDestAddress);

    if (pQueuePair == nullptr || pDestAddress == nullptr || cbDestAddress > sizeof(m_peerAddr) || m_socket)
    {
        NdTrace(
            "Conn::Connect rejected: queuePair=%p, "
            "destination=%p, destinationLength=%lu, maximumLength=%zu, "
            "socketActive=%u",
            static_cast<const void *>(pQueuePair),
            static_cast<const void *>(pDestAddress),
            cbDestAddress,
            sizeof(m_peerAddr),
            m_socket ? 1U : 0U);
        return E_INVALIDARG;
    }

    if (cbPrivateData > NDPROV_MAX_PRIVATE_DATA)
    {
        return ND_INVALID_BUFFER_SIZE;
    }

    // TCP closes after setup, so the socket doesn't show a connection; the
    // queue pair stays associated until Disconnect.
    AcquireSRWLockShared(&m_stateLock);
    const bool associated = m_queuePair != nullptr;
    ReleaseSRWLockShared(&m_stateLock);
    if (associated)
    {
        return ND_CONNECTION_ACTIVE;
    }

    Microsoft::WRL::ComPtr<IND2QueuePair> queuePairInterface;
    HRESULT result = pQueuePair->QueryInterface(
        IID_IND2QueuePair,
        reinterpret_cast<void **>(queuePairInterface.GetAddressOf()));
    if (FAILED(result))
    {
        return result;
    }

    NdQueuePair *const queuePair = static_cast<NdQueuePair *>(queuePairInterface.Get());

    // The driver registers the binding before the peer can see the
    // connection request, and assigns the tunnel when the peer accepts.
    m_connectionToken = GenerateConnectionToken();
    result = queuePair->BeginBind(
        m_connectionToken,
        RdmaConnectionRoleActive);
    if (FAILED(result))
    {
        return result;
    }

    bool bindPending = true;
    bool connectionEstablished = false;

    do
    {
        memcpy(&m_peerAddr, pDestAddress, cbDestAddress);
        m_peerAddrLen = cbDestAddress;
        ApplyZeroPortFallback(
            "Conn::Connect",
            m_peerAddr);

        if (m_peerAddr.sin_family != AF_INET || m_peerAddr.sin_port == 0)
        {
            NdTrace(
                "Conn::Connect invalid destination: "
                "destinationLength=%lu, family=%u, servicePort=%u",
                m_peerAddrLen,
                static_cast<unsigned int>(m_peerAddr.sin_family),
                static_cast<unsigned int>(ntohs(m_peerAddr.sin_port)));
            result = E_INVALIDARG;
            break;
        }

        struct sockaddr_in cmPeerAddress{};
        result = GetCmAddress(
            m_peerAddr,
            cmPeerAddress);
        if (FAILED(result))
        {
            NdTrace(
                "Conn::Connect invalid CM port mapping: "
                "servicePort=%u, offset=%lu",
                static_cast<unsigned int>(ntohs(m_peerAddr.sin_port)),
                GetProviderConfiguration().cmPortOffset);
            break;
        }

        NdTrace(
            "Conn::Connect attempting: servicePort=%u, "
            "cmPort=%u, offset=%lu",
            static_cast<unsigned int>(ntohs(m_peerAddr.sin_port)),
            static_cast<unsigned int>(ntohs(cmPeerAddress.sin_port)),
            GetProviderConfiguration().cmPortOffset);

        wil::unique_socket connectSocket{ socket(
            AF_INET,
            SOCK_STREAM,
            IPPROTO_TCP) };
        if (!connectSocket)
        {
            const int error = WSAGetLastError();
            LogCmSocketFailure(
                "Conn::Connect socket",
                m_peerAddr,
                cmPeerAddress,
                error);
            result = HRESULT_FROM_WIN32(error);
            break;
        }

        // Published under the lock, so that CancelOverlappedRequests either
        // shuts it down or prevents the connection.
        AcquireSRWLockExclusive(&m_stateLock);
        const bool canceled = m_operationCanceled;
        if (!canceled)
        {
            m_socket = std::move(connectSocket);
        }
        ReleaseSRWLockExclusive(&m_stateLock);
        if (canceled)
        {
            result = ND_CANCELED;
            break;
        }

        result = SetCmSocketTimeouts(
            m_socket.get(),
            c_cmNegotiationTimeoutMilliseconds,
            c_cmNegotiationTimeoutMilliseconds);
        if (FAILED(result))
        {
            break;
        }

        if (m_localAddrLen > 0)
        {
            struct sockaddr_in bindAddress = m_localAddr;
            bindAddress.sin_port = 0;
            LogSocketAddress(
                "Conn::Connect local bind attempting",
                reinterpret_cast<const struct sockaddr *>(&bindAddress),
                sizeof(bindAddress));
            if (bind(
                    m_socket.get(),
                    reinterpret_cast<const struct sockaddr *>(&bindAddress),
                    sizeof(bindAddress)) == SOCKET_ERROR)
            {
                const int error = WSAGetLastError();
                LogSocketAddressFailure(
                    "Conn::Connect bind",
                    bindAddress,
                    error);
                result = HRESULT_FROM_WIN32(error);
                break;
            }
        }

        if (connect(
                m_socket.get(),
                reinterpret_cast<const struct sockaddr *>(&cmPeerAddress),
                sizeof(cmPeerAddress)) == SOCKET_ERROR)
        {
            const int error = WSAGetLastError();
            LogCmSocketFailure(
                "Conn::Connect connect",
                m_peerAddr,
                cmPeerAddress,
                error);
            result = HRESULT_FROM_WIN32(error);
            break;
        }

        NdTrace(
            "Conn::Connect connected: servicePort=%u, cmPort=%u",
            static_cast<unsigned int>(ntohs(m_peerAddr.sin_port)),
            static_cast<unsigned int>(ntohs(cmPeerAddress.sin_port)));

        result = EnableCmKeepAlive(m_socket.get());
        if (FAILED(result))
        {
            break;
        }

        int localAddressLength = sizeof(m_localAddr);
        if (getsockname(
                m_socket.get(),
                reinterpret_cast<struct sockaddr *>(&m_localAddr),
                &localAddressLength) == SOCKET_ERROR)
        {
            const int error = WSAGetLastError();
            LogCmSocketFailure(
                "Conn::Connect getsockname",
                m_peerAddr,
                cmPeerAddress,
                error);
            result = HRESULT_FROM_WIN32(error);
            break;
        }
        m_localAddrLen = static_cast<ULONG>(localAddressLength);

        NdCmConnectRequest request = {};
        request.Header.Type = ND_CM_CONNECT_REQUEST;
        request.Header.Version = NDPROV_CM_VERSION;
        request.Header.ConnectionToken = m_connectionToken;
        request.InboundReadLimit = 0;
        request.OutboundReadLimit = 0;
        request.PrivateDataLength = cbPrivateData;
        if (pPrivateData != nullptr && request.PrivateDataLength > 0)
        {
            memcpy(
                request.PrivateData,
                pPrivateData,
                request.PrivateDataLength);
        }

        const int sendSize = static_cast<int>(
            offsetof(
                NdCmConnectRequest,
                PrivateData) +
            request.PrivateDataLength);
        if (!SendCmMessage(
                &request,
                sendSize))
        {
            result = ND_CONNECTION_REFUSED;
            break;
        }

        result = WaitForPeerApplication();
        if (FAILED(result))
        {
            break;
        }

        NdCmHeader responseHeader = {};
        if (!ReceiveCmMessage(
                &responseHeader,
                sizeof(responseHeader)) ||
            responseHeader.Version != NDPROV_CM_VERSION ||
            !AreConnectionTokensEqual(
                responseHeader.ConnectionToken,
                m_connectionToken))
        {
            result = ND_CONNECTION_REFUSED;
            break;
        }

        if (responseHeader.Type == ND_CM_CONNECT_REJECT)
        {
            ReceiveRejectData();
            result = ND_CONNECTION_REFUSED;
            break;
        }

        if (responseHeader.Type == ND_CM_CONNECT_ABORT)
        {
            HRESULT abortStatus = ND_CONNECTION_REFUSED;
            if (!ReceiveCmMessage(
                    &abortStatus,
                    sizeof(abortStatus)) ||
                SUCCEEDED(abortStatus))
            {
                abortStatus = ND_CONNECTION_REFUSED;
            }

            result = abortStatus;
            break;
        }

        if (responseHeader.Type != ND_CM_CONNECT_ACCEPT)
        {
            result = ND_CONNECTION_REFUSED;
            break;
        }
        struct {
            UINT32 InboundReadLimit;
            UINT32 OutboundReadLimit;
            UINT32 PrivateDataLength;
        } acceptFields = {};

        if (!ReceiveCmMessage(
                &acceptFields,
                sizeof(acceptFields)) ||
            acceptFields.PrivateDataLength > NDPROV_MAX_PRIVATE_DATA)
        {
            result = ND_CONNECTION_REFUSED;
            break;
        }

        m_peerPrivateDataLength = acceptFields.PrivateDataLength;
        if (m_peerPrivateDataLength > 0 && !ReceiveCmMessage(
                                               m_peerPrivateData,
                                               m_peerPrivateDataLength))
        {
            result = ND_CONNECTION_REFUSED;
            break;
        }

        // The passive side accepts once the rings of both sides run, so this
        // side's binding completes at the same time.
        bindPending = false;
        result = WaitForBind(queuePair);
        if (FAILED(result))
        {
            break;
        }

        result = EstablishConnection(queuePairInterface.Get());
        if (FAILED(result))
        {
            queuePair->AbortBinding();
            break;
        }

        connectionEstablished = true;
    } while (false);

    if (bindPending && SUCCEEDED(queuePair->EndBind(true)))
    {
        queuePair->AbortBinding();
    }

    // A peer provider that stops answering isn't a rejection.
    if (!connectionEstablished && result == ND_CONNECTION_REFUSED && m_cmTimedOut)
    {
        result = ND_IO_TIMEOUT;
    }

    // TCP carries only the connection setup.
    CloseSocket();
    return result;
}

HRESULT NdConnector::CompleteConnect(OVERLAPPED* pOverlapped)
{
    if (pOverlapped == nullptr)
    {
        return E_INVALIDARG;
    }

    // A connection that the peer already ended was still established; its
    // NotifyDisconnect reports how it ended.
    NdOverlappedRequests::Start(pOverlapped);
    return m_requests.Complete(
        pOverlapped,
        m_everConnected ? S_OK : ND_CONNECTION_REFUSED);
}

HRESULT NdConnector::Accept(IUnknown* pQueuePair, ULONG inboundReadLimit, ULONG outboundReadLimit,
    const VOID* pPrivateData, ULONG cbPrivateData, OVERLAPPED* pOverlapped)
{
    if (pOverlapped == nullptr)
    {
        return E_INVALIDARG;
    }

    wistd::unique_ptr<NdConnectorWork> work = wil::make_unique_nothrow<NdConnectorWork>(
        this,
        NdConnectorOperation::Accept,
        pOverlapped);
    if (!work)
    {
        return E_OUTOFMEMORY;
    }

    // The connection request stays pending, so the application can accept
    // again with less private data, or reject it.
    const HRESULT result = CopyPrivateData(
        pPrivateData,
        cbPrivateData,
        *work);
    if (FAILED(result))
    {
        return result;
    }

    work->QueuePair = pQueuePair;
    work->InboundReadLimit = inboundReadLimit;
    work->OutboundReadLimit = outboundReadLimit;
    return StartOperation(
        pOverlapped,
        wistd::move(work));
}

HRESULT
NdConnector::AcceptWorker(
    IUnknown *pQueuePair,
    ULONG inboundReadLimit,
    ULONG outboundReadLimit,
    const void *pPrivateData,
    ULONG cbPrivateData)
{
    UNREFERENCED_PARAMETER(inboundReadLimit);
    UNREFERENCED_PARAMETER(outboundReadLimit);

    if (!m_socket)
    {
        return E_FAIL;
    }

    m_cmTimedOut = false;

    // The connection request stays pending, so the application can accept
    // again with less private data, or reject it.
    if (cbPrivateData > NDPROV_MAX_PRIVATE_DATA)
    {
        return ND_INVALID_BUFFER_SIZE;
    }

    if (pQueuePair == nullptr)
    {
        return AbortAccept(E_INVALIDARG);
    }

    Microsoft::WRL::ComPtr<IND2QueuePair> queuePairInterface;
    HRESULT result = pQueuePair->QueryInterface(
        IID_IND2QueuePair,
        reinterpret_cast<void **>(queuePairInterface.GetAddressOf()));
    if (FAILED(result))
    {
        return AbortAccept(result);
    }

    NdQueuePair *const queuePair = static_cast<NdQueuePair *>(queuePairInterface.Get());

    // The passive side's binding agrees on the tunnel with the active side's,
    // and completes once the rings of both sides run.
    result = queuePair->BeginBind(
        m_connectionToken,
        RdmaConnectionRolePassive);
    if (SUCCEEDED(result))
    {
        result = WaitForBind(queuePair);
    }

    if (FAILED(result))
    {
        NdTrace(
            "Conn::Accept binding failed: 0x%08X",
            result);
        return AbortAccept(result);
    }

    NdCmConnectAccept accept = {};
    accept.Header.Type = ND_CM_CONNECT_ACCEPT;
    accept.Header.Version = NDPROV_CM_VERSION;
    accept.Header.ConnectionToken = m_connectionToken;
    accept.InboundReadLimit = 0;
    accept.OutboundReadLimit = 0;
    accept.PrivateDataLength = cbPrivateData;
    if (pPrivateData != nullptr && accept.PrivateDataLength > 0)
    {
        memcpy(accept.PrivateData, pPrivateData, accept.PrivateDataLength);
    }

    const int sendSize = static_cast<int>(
        offsetof(
            NdCmConnectAccept,
            PrivateData) +
        accept.PrivateDataLength);
    if (!SendCmMessage(
            &accept,
            sendSize))
    {
        // Releasing the tunnel retires the active side's queue pair.
        queuePair->AbortBinding();
        CloseSocket();
        return m_cmTimedOut ? ND_IO_TIMEOUT : ND_CONNECTION_REFUSED;
    }

    result = EstablishConnection(queuePairInterface.Get());
    if (FAILED(result))
    {
        queuePair->AbortBinding();
    }

    // TCP carries only the connection setup.
    CloseSocket();
    return result;
}

HRESULT NdConnector::Reject(const VOID* pPrivateData, ULONG cbPrivateData)
{
    if (!m_socket)
    {
        return E_FAIL;
    }

    if (cbPrivateData > NDPROV_MAX_PRIVATE_DATA)
    {
        return ND_INVALID_BUFFER_SIZE;
    }

    NdCmConnectReject reject = {};
    reject.Header.Type = ND_CM_CONNECT_REJECT;
    reject.Header.Version = NDPROV_CM_VERSION;
    reject.Header.ConnectionToken = m_connectionToken;
    reject.PrivateDataLength = cbPrivateData;
    if (pPrivateData && reject.PrivateDataLength > 0)
    {
        memcpy(reject.PrivateData, pPrivateData, reject.PrivateDataLength);
    }

    int sendSize = static_cast<int>(offsetof(NdCmConnectReject, PrivateData) + reject.PrivateDataLength);
    SendMsg(
        m_socket.get(),
        &reject,
        sendSize);
    CloseSocket();
    return S_OK;
}

HRESULT NdConnector::GetReadLimits(ULONG* pInboundReadLimit, ULONG* pOutboundReadLimit)
{
    if (pInboundReadLimit)
    {
        *pInboundReadLimit = 0;
    }
    if (pOutboundReadLimit)
    {
        *pOutboundReadLimit = 0;
    }
    return S_OK;
}

HRESULT NdConnector::GetPrivateData(VOID* pPrivateData, ULONG* pcbPrivateData)
{
    if (!pcbPrivateData)
    {
        return E_INVALIDARG;
    }
    ULONG copyLen = min(*pcbPrivateData, m_peerPrivateDataLength);
    if (pPrivateData && copyLen > 0)
    {
        memcpy(pPrivateData, m_peerPrivateData, copyLen);
    }
    *pcbPrivateData = m_peerPrivateDataLength;
    return S_OK;
}

HRESULT NdConnector::GetLocalAddress(struct sockaddr* pAddress, ULONG* pcbAddress)
{
    if (m_localAddrLen == 0)
    {
        return E_NOTIMPL;
    }
    if (*pcbAddress < m_localAddrLen) { *pcbAddress = m_localAddrLen; return ND_BUFFER_OVERFLOW; }
    memcpy(pAddress, &m_localAddr, m_localAddrLen);
    *pcbAddress = m_localAddrLen;
    return S_OK;
}

HRESULT NdConnector::GetPeerAddress(struct sockaddr* pAddress, ULONG* pcbAddress)
{
    if (m_peerAddrLen == 0)
    {
        return E_NOTIMPL;
    }
    if (*pcbAddress < m_peerAddrLen) { *pcbAddress = m_peerAddrLen; return ND_BUFFER_OVERFLOW; }
    memcpy(pAddress, &m_peerAddr, m_peerAddrLen);
    *pcbAddress = m_peerAddrLen;
    return S_OK;
}

HRESULT NdConnector::NotifyDisconnect(OVERLAPPED* pOverlapped)
{
    if (pOverlapped == nullptr)
    {
        return E_INVALIDARG;
    }

    AcquireSRWLockExclusive(&m_stateLock);
    if (!m_connected && !m_everConnected)
    {
        ReleaseSRWLockExclusive(&m_stateLock);
        return ND_CONNECTION_INVALID;
    }

    if (m_disconnectRequest != nullptr)
    {
        ReleaseSRWLockExclusive(&m_stateLock);
        return ND_DEVICE_BUSY;
    }

    // Started under the lock, so that the release callback and
    // CancelOverlappedRequests, which complete the request, see it started.
    NdOverlappedRequests::Start(pOverlapped);
    if (m_connected)
    {
        m_disconnectRequest = pOverlapped;
        ReleaseSRWLockExclusive(&m_stateLock);
        return ND_PENDING;
    }

    const HRESULT status = m_disconnectStatus;
    ReleaseSRWLockExclusive(&m_stateLock);

    return m_requests.Complete(
        pOverlapped,
        status);
}

HRESULT NdConnector::Disconnect(OVERLAPPED* pOverlapped)
{
    if (pOverlapped == nullptr)
    {
        return E_INVALIDARG;
    }

    wistd::unique_ptr<NdConnectorWork> work = wil::make_unique_nothrow<NdConnectorWork>(
        this,
        NdConnectorOperation::Disconnect,
        pOverlapped);
    if (!work)
    {
        return E_OUTOFMEMORY;
    }

    return StartOperation(
        pOverlapped,
        wistd::move(work));
}

HRESULT
NdConnector::DisconnectWorker()
{
    AcquireSRWLockShared(&m_stateLock);
    const bool wasConnected = m_everConnected;
    ReleaseSRWLockShared(&m_stateLock);

    if (!wasConnected)
    {
        return ND_CONNECTION_INVALID;
    }

    // Flushes the queue pair as a disconnection and waits for the peer to
    // release the tunnel, which ends the connection.
    const HRESULT status = DisconnectAssociatedQueuePair();

    // Ends the connection here if the release callback didn't.
    StopReleaseWait();
    OnQueuePairReleased();

    Microsoft::WRL::ComPtr<IND2QueuePair> queuePair;
    AcquireSRWLockExclusive(&m_stateLock);
    queuePair = std::move(m_queuePair);
    ReleaseSRWLockExclusive(&m_stateLock);

    return status;
}

bool
NdConnector::SetAcceptedSocket(
    SOCKET sock,
    const struct sockaddr_in &localAddr,
    const struct sockaddr_in &peerAddr,
    const NdCmConnectRequest &request)
{
    if (FAILED(SetCmSocketTimeouts(
            sock,
            c_cmNegotiationTimeoutMilliseconds,
            c_cmNegotiationTimeoutMilliseconds)))
    {
        return false;
    }

    AcquireSRWLockExclusive(&m_stateLock);
    m_socket.reset(sock);
    ReleaseSRWLockExclusive(&m_stateLock);
    m_localAddr = localAddr;
    m_localAddrLen = sizeof(localAddr);
    m_peerAddr = peerAddr;
    m_peerAddrLen = sizeof(peerAddr);
    m_peerPrivateDataLength = min(request.PrivateDataLength, NDPROV_MAX_PRIVATE_DATA);
    m_connectionToken = request.Header.ConnectionToken;
    if (m_peerPrivateDataLength > 0)
    {
        memcpy(m_peerPrivateData, request.PrivateData, m_peerPrivateDataLength);
    }

    return true;
}


// ============================================================================
// NdListener — TCP-based listener
// ============================================================================

NdListener::NdListener(
    _In_ NdDeviceContext *deviceContext)
    : m_deviceContext{ deviceContext }
{
}

NdListener::~NdListener() = default;

HRESULT
NdListener::Initialize(
    HANDLE overlappedFile)
{
    return m_requests.Initialize(overlappedFile);
}

HRESULT NdListener::QueryInterface(REFIID riid, LPVOID* ppvObj)
{
    if (InlineIsEqualGUID(riid, IID_IUnknown) || InlineIsEqualGUID(riid, IID_IND2Listener))
    {
        *ppvObj = static_cast<IND2Listener*>(this);
        AddRef();
        return S_OK;
    }
    *ppvObj = nullptr;
    return E_NOINTERFACE;
}

HRESULT NdListener::CancelOverlappedRequests()
{
    // Closing the socket ends running GetConnectionRequest work with
    // ND_CANCELED. It is closed after the lock is released.
    wil::unique_socket listenSocket;
    AcquireSRWLockExclusive(&m_socketLock);
    m_canceled = true;
    listenSocket = std::move(m_socket);
    ReleaseSRWLockExclusive(&m_socketLock);
    return S_OK;
}

HRESULT NdListener::GetOverlappedResult(OVERLAPPED* pOverlapped, BOOL wait)
{
    return NdOverlappedRequests::GetResult(
        pOverlapped,
        wait);
}

HRESULT NdListener::Bind(const struct sockaddr* pAddress, ULONG cbAddress)
{
    LogSocketAddress(
        "Listener::Bind requested",
        pAddress,
        cbAddress);
    if (pAddress == nullptr || cbAddress > sizeof(m_listenAddr))
    {
        NdTrace(
            "Listener::Bind rejected: addressLength=%lu, "
            "maximumLength=%zu",
            cbAddress,
            sizeof(m_listenAddr));
        return E_INVALIDARG;
    }
    memcpy(&m_listenAddr, pAddress, cbAddress);
    m_listenAddrLen = cbAddress;
    return S_OK;
}

HRESULT NdListener::Listen(ULONG backlog)
{
    LogSocketAddress(
        "Listener::Listen service endpoint",
        reinterpret_cast<const struct sockaddr *>(&m_listenAddr),
        m_listenAddrLen);
    ApplyZeroPortFallback(
        "Listener::Listen",
        m_listenAddr);

    if (m_listenAddrLen != sizeof(m_listenAddr) || m_listenAddr.sin_family != AF_INET || m_listenAddr.sin_port == 0)
    {
        NdTrace(
            "Listener::Listen invalid service endpoint: "
            "addressLength=%lu, expectedLength=%zu, family=%u, "
            "servicePort=%u",
            m_listenAddrLen,
            sizeof(m_listenAddr),
            static_cast<unsigned int>(m_listenAddr.sin_family),
            static_cast<unsigned int>(ntohs(m_listenAddr.sin_port)));
        return E_INVALIDARG;
    }

    struct sockaddr_in cmListenAddress{
    };
    const HRESULT addressStatus = GetCmAddress(
        m_listenAddr,
        cmListenAddress);
    if (FAILED(addressStatus))
    {
        NdTrace(
            "Listener::Listen invalid CM port mapping: "
            "servicePort=%u, offset=%lu",
            static_cast<unsigned int>(ntohs(m_listenAddr.sin_port)),
            GetProviderConfiguration().cmPortOffset);
        return addressStatus;
    }

    NdTrace(
        "Listener::Listen attempting: servicePort=%u, "
        "cmPort=%u, offset=%lu, requestedBacklog=%lu",
        static_cast<unsigned int>(ntohs(m_listenAddr.sin_port)),
        static_cast<unsigned int>(ntohs(cmListenAddress.sin_port)),
        GetProviderConfiguration().cmPortOffset,
        backlog);

    m_socket.reset(socket(
        AF_INET,
        SOCK_STREAM,
        IPPROTO_TCP));
    if (!m_socket)
    {
        const int error = WSAGetLastError();
        LogCmSocketFailure(
            "Listener::Listen socket",
            m_listenAddr,
            cmListenAddress,
            error);
        return HRESULT_FROM_WIN32(error);
    }

    const BOOL exclusiveAddressUse = TRUE;
    if (setsockopt(
            m_socket.get(),
            SOL_SOCKET,
            SO_EXCLUSIVEADDRUSE,
            reinterpret_cast<const char *>(&exclusiveAddressUse),
            sizeof(exclusiveAddressUse)) == SOCKET_ERROR)
    {
        const int error = WSAGetLastError();
        LogCmSocketFailure(
            "Listener::Listen setsockopt(SO_EXCLUSIVEADDRUSE)",
            m_listenAddr,
            cmListenAddress,
            error);
        m_socket.reset();
        return HRESULT_FROM_WIN32(error);
    }

    if (bind(
            m_socket.get(),
            reinterpret_cast<const struct sockaddr *>(&cmListenAddress),
            sizeof(cmListenAddress)) == SOCKET_ERROR)
    {
        const int error = WSAGetLastError();
        LogCmSocketFailure(
            "Listener::Listen bind",
            m_listenAddr,
            cmListenAddress,
            error);
        m_socket.reset();
        return HRESULT_FROM_WIN32(error);
    }

    const int listenBacklog = backlog > 0 && backlog <= static_cast<ULONG>(SOMAXCONN) ? static_cast<int>(backlog) : SOMAXCONN;
    if (listen(
            m_socket.get(),
            listenBacklog) == SOCKET_ERROR)
    {
        const int error = WSAGetLastError();
        LogCmSocketFailure(
            "Listener::Listen listen",
            m_listenAddr,
            cmListenAddress,
            error);
        m_socket.reset();
        return HRESULT_FROM_WIN32(error);
    }

    NdTrace(
        "Listener::Listen active: servicePort=%u, cmPort=%u, "
        "backlog=%lu",
        static_cast<unsigned int>(ntohs(m_listenAddr.sin_port)),
        static_cast<unsigned int>(ntohs(cmListenAddress.sin_port)),
        static_cast<ULONG>(listenBacklog));

    return S_OK;
}

HRESULT NdListener::GetLocalAddress(struct sockaddr* pAddress, ULONG* pcbAddress)
{
    if (m_listenAddrLen == 0)
    {
        return E_NOTIMPL;
    }
    if (*pcbAddress < m_listenAddrLen) { *pcbAddress = m_listenAddrLen; return ND_BUFFER_OVERFLOW; }
    memcpy(pAddress, &m_listenAddr, m_listenAddrLen);
    *pcbAddress = m_listenAddrLen;
    return S_OK;
}

// A GetConnectionRequest running on the thread pool. It keeps the listener and
// the connector alive.
class NdListenerWork : public NdWork
{
public:

    NdListenerWork(
        _In_ NdListener *listener,
        _In_ IUnknown *connector,
        _In_ OVERLAPPED *overlapped)
        : m_listener{ listener }
        , m_connector{ connector }
        , m_overlapped{ overlapped }
    {
    }

private:

    void
    Run() override
    {
        m_listener->m_requests.Complete(
            m_overlapped,
            m_listener->GetConnectionRequestWorker(m_connector.Get()));
    }

    Microsoft::WRL::ComPtr<NdListener> m_listener;
    Microsoft::WRL::ComPtr<IUnknown> m_connector;
    OVERLAPPED *m_overlapped;
};

HRESULT NdListener::GetConnectionRequest(IUnknown* pConnector, OVERLAPPED* pOverlapped)
{
    if (pConnector == nullptr || pOverlapped == nullptr)
    {
        return E_INVALIDARG;
    }

    wistd::unique_ptr<NdListenerWork> work = wil::make_unique_nothrow<NdListenerWork>(
        this,
        pConnector,
        pOverlapped);
    if (!work)
    {
        return E_OUTOFMEMORY;
    }

    NdOverlappedRequests::Start(pOverlapped);
    NdWork::Submit(wistd::move(work));
    return ND_PENDING;
}

HRESULT
NdListener::GetConnectionRequestWorker(
    IUnknown *pConnector)
{
    AcquireSRWLockShared(&m_socketLock);
    const SOCKET listenSocket = m_socket.get();
    const bool canceled = m_canceled;
    ReleaseSRWLockShared(&m_socketLock);
    if (listenSocket == INVALID_SOCKET)
    {
        return canceled ? ND_CANCELED : E_FAIL;
    }

    // Blocks until a client connects, or CancelOverlappedRequests closes the
    // socket.
    struct sockaddr_in peerAddr = {};
    int peerAddrLen = sizeof(peerAddr);
    wil::unique_socket acceptedSocket{ accept(
        listenSocket,
        reinterpret_cast<struct sockaddr *>(&peerAddr),
        &peerAddrLen) };
    if (!acceptedSocket)
    {
        int err = WSAGetLastError();
        if (err == WSAEINTR || err == WSAENOTSOCK)
        {
            return ND_CANCELED;
        }
        return HRESULT_FROM_WIN32(err);
    }

    const HRESULT socketStatus = SetCmSocketTimeouts(
        acceptedSocket.get(),
        c_cmNegotiationTimeoutMilliseconds,
        c_cmNegotiationTimeoutMilliseconds);
    if (FAILED(socketStatus))
    {
        return socketStatus;
    }

    const HRESULT keepAliveStatus = EnableCmKeepAlive(acceptedSocket.get());
    if (FAILED(keepAliveStatus))
    {
        return keepAliveStatus;
    }

    // Read the connect request header + fixed fields from the TCP stream
    NdCmConnectRequest request = {};
    int fixedSize = static_cast<int>(offsetof(NdCmConnectRequest, PrivateData));
    if (!NdConnector::RecvMsg(
            acceptedSocket.get(),
            &request,
            fixedSize))
    {
        return E_FAIL;
    }

    if (request.Header.Type != ND_CM_CONNECT_REQUEST || request.Header.Version != NDPROV_CM_VERSION ||
        request.PrivateDataLength > NDPROV_MAX_PRIVATE_DATA ||
        (request.Header.ConnectionToken.High == 0 && request.Header.ConnectionToken.Low == 0))
    {
        return E_FAIL;
    }

    // Read private data if present
    const UINT32 privLen = request.PrivateDataLength;
    if (privLen > 0)
    {
        if (!NdConnector::RecvMsg(
                acceptedSocket.get(),
                request.PrivateData,
                privLen))
        {
            return E_FAIL;
        }
    }

    // Hand the accepted socket and request data to the connector
    IND2Connector* iConn = nullptr;
    HRESULT hr = pConnector->QueryInterface(IID_IND2Connector, reinterpret_cast<void**>(&iConn));
    if (SUCCEEDED(hr) && iConn)
    {
        NdConnector* pConn = static_cast<NdConnector*>(iConn);
        const bool socketAccepted = pConn->SetAcceptedSocket(acceptedSocket.get(), m_listenAddr, peerAddr, request);
        iConn->Release();
        if (!socketAccepted)
        {
            return E_FAIL;
        }
        acceptedSocket.release();
    }
    else
    {
        return E_FAIL;
    }
    return S_OK;
}

// NdAdapter
// ============================================================================

HRESULT
NdAdapter::Create(
    UINT64 adapterId,
    _In_z_ const WCHAR *symbolicLink,
    NET_LUID luid,
    _Outptr_ NdAdapter **adapter)
{
    if (symbolicLink == nullptr || adapter == nullptr)
    {
        return E_INVALIDARG;
    }

    *adapter = nullptr;

    wistd::unique_ptr<NdAdapter> adapterInstance = wil::make_unique_nothrow<NdAdapter>();
    if (adapterInstance == nullptr)
    {
        return E_OUTOFMEMORY;
    }

    const HRESULT status = adapterInstance->Open(
        adapterId,
        symbolicLink,
        luid);
    if (FAILED(status))
    {
        return status;
    }

    *adapter = adapterInstance.release();
    return S_OK;
}

NdAdapter::~NdAdapter()
{
}

HRESULT
NdAdapter::Open(
    UINT64 adapterId,
    _In_z_ const WCHAR *symbolicLink,
    NET_LUID luid)
{
    m_adapterId = adapterId;
    m_luid = luid;

    NdDeviceContext *deviceContext{ nullptr };
    const HRESULT status = NdDeviceContext::Create(
        symbolicLink,
        &deviceContext);
    if (FAILED(status))
    {
        NdTrace(
            "Adapter::Open device context failed: 0x%08X",
            status);
        return status;
    }
    m_deviceContext.Attach(deviceContext);

    RefreshAddresses();

    NdTrace(
        "Adapter::Open OK, LUID=0x%llX, addrs=%u",
        m_luid.Value,
        m_addrCount);
    return S_OK;
}

HRESULT NdAdapter::RefreshAddresses()
{
    m_addrCount = 0;

    unique_unicast_ip_address_table table;
    const DWORD err = GetUnicastIpAddressTable(
        AF_INET,
        table.addressof());
    if (err != NO_ERROR)
    {
        return HRESULT_FROM_WIN32(err);
    }

    for (ULONG t = 0; t < table.get()->NumEntries && m_addrCount < MAX_ADDRS; t++)
    {
        const MIB_UNICASTIPADDRESS_ROW &row = table.get()->Table[t];
        if (row.InterfaceLuid.Value == m_luid.Value)
        {
            memset(&m_addrs[m_addrCount], 0, sizeof(struct sockaddr_in));
            m_addrs[m_addrCount].sin_family = AF_INET;
            m_addrs[m_addrCount].sin_addr = row.Address.Ipv4.sin_addr;
            m_addrCount++;
        }
    }
    return S_OK;
}

HRESULT NdAdapter::QueryInterface(REFIID riid, LPVOID* ppvObj)
{
    if (InlineIsEqualGUID(riid, IID_IUnknown) || InlineIsEqualGUID(riid, IID_IND2Adapter))
    {
        *ppvObj = static_cast<IND2Adapter*>(this);
        AddRef();
        return S_OK;
    }
    *ppvObj = nullptr;
    return E_NOINTERFACE;
}

HRESULT NdAdapter::CreateOverlappedFile(HANDLE* phOverlappedFile)
{
    if (phOverlappedFile == nullptr)
    {
        return E_INVALIDARG;
    }
    *phOverlappedFile = nullptr;

    wil::unique_hfile overlappedFile{ CreateFileW(
        m_deviceContext->GetOverlappedPath(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED,
        nullptr) };
    if (!overlappedFile)
    {
        const HRESULT openStatus = HRESULT_FROM_WIN32(GetLastError());
        NdTrace(
            "Adapter::CreateOverlappedFile open failed: 0x%08X",
            openStatus);
        return openStatus;
    }

    HRESULT bindStatus = S_OK;
    {
        NdDeviceUsageGuard deviceUsage{ m_deviceContext.Get() };
        if (!deviceUsage.IsAvailable())
        {
            return ND_DEVICE_REMOVED;
        }

        RDMA_BIND_OVERLAPPED_FILE_INPUT input{};
        input.RdmaHandle = static_cast<UINT64>(reinterpret_cast<ULONG_PTR>(deviceUsage.GetHandle()));
        bindStatus = ::SendIoctl(
            overlappedFile.get(),
            IOCTL_RDMA_BIND_OVERLAPPED_FILE,
            &input,
            sizeof(input),
            nullptr,
            0);
    }

    if (FAILED(bindStatus))
    {
        NdTrace(
            "Adapter::CreateOverlappedFile bind failed: 0x%08X",
            bindStatus);
        return bindStatus;
    }

    if (!SetFileCompletionNotificationModes(
            overlappedFile.get(),
            FILE_SKIP_COMPLETION_PORT_ON_SUCCESS | FILE_SKIP_SET_EVENT_ON_HANDLE))
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    *phOverlappedFile = overlappedFile.release();
    return S_OK;
}

HRESULT NdAdapter::Query(ND2_ADAPTER_INFO* pInfo, ULONG* pcbInfo)
{
    if (*pcbInfo < sizeof(ND2_ADAPTER_INFO))
    {
        *pcbInfo = sizeof(ND2_ADAPTER_INFO);
        return ND_BUFFER_OVERFLOW;
    }

    memset(pInfo, 0, sizeof(*pInfo));
    pInfo->InfoVersion = ND_VERSION_2;
    // USB4 vendor and device IDs of the USB4 P2P device.
    pInfo->VendorId = 0x045E;
    pInfo->DeviceId = 0x083F;
    pInfo->AdapterId = m_adapterId;
    // Registrations must also be page aligned and a whole number of pages.
    pInfo->MaxRegistrationSize = static_cast<SIZE_T>(RDMA_MAX_MEMORY_REGION_SIZE);
    pInfo->MaxWindowSize = 0;
    pInfo->MaxInitiatorSge = 1;
    pInfo->MaxReceiveSge = 1;
    pInfo->MaxReadSge = 0;
    // With one SGE, a request lies within one registration.
    pInfo->MaxTransferLength = static_cast<ULONG>(RDMA_MAX_MEMORY_REGION_SIZE);
    // Sends always read registered memory; ND_OP_FLAG_INLINE has no effect.
    pInfo->MaxInlineDataSize = 0;
    pInfo->MaxInboundReadLimit = 0;
    pInfo->MaxOutboundReadLimit = 0;
    // Receives posted before the QP connects are held in a fixed table.
    pInfo->MaxReceiveQueueDepth = NDPROV_MAX_QUEUE_DEPTH;
    pInfo->MaxInitiatorQueueDepth = RDMA_MAX_QUEUE_PAIR_REQUESTS;
    pInfo->MaxSharedReceiveQueueDepth = 0;
    pInfo->MaxCompletionQueueDepth = RDMA_MAX_COMPLETION_QUEUE_CAPACITY;
    pInfo->InlineRequestThreshold = 0;
    // Read and Write aren't supported.
    pInfo->LargeRequestThreshold = ULONG_MAX;
    pInfo->MaxCallerData = NDPROV_MAX_PRIVATE_DATA;
    pInfo->MaxCalleeData = NDPROV_MAX_PRIVATE_DATA;
    pInfo->AdapterFlags = ND_ADAPTER_FLAG_IN_ORDER_DMA_SUPPORTED;

    return S_OK;
}

HRESULT NdAdapter::QueryAddressList(SOCKET_ADDRESS_LIST* pAddressList, ULONG* pcbAddressList)
{
    RefreshAddresses();

    if (m_addrCount == 0)
    {
        ULONG needed = sizeof(SOCKET_ADDRESS_LIST);
        if (pAddressList == nullptr || *pcbAddressList < needed)
        {
            *pcbAddressList = needed;
            return ND_BUFFER_OVERFLOW;
        }
        *pcbAddressList = needed;
        pAddressList->iAddressCount = 0;
        return S_OK;
    }

    ULONG needed = sizeof(SOCKET_ADDRESS_LIST)
        + (m_addrCount - 1) * sizeof(SOCKET_ADDRESS)
        + m_addrCount * sizeof(struct sockaddr_in);

    if (pAddressList == nullptr || *pcbAddressList < needed)
    {
        *pcbAddressList = needed;
        return ND_BUFFER_OVERFLOW;
    }

    *pcbAddressList = needed;
    pAddressList->iAddressCount = static_cast<INT>(m_addrCount);

    struct sockaddr_in* pAddrs = reinterpret_cast<struct sockaddr_in*>(
        reinterpret_cast<BYTE*>(pAddressList) + sizeof(SOCKET_ADDRESS_LIST)
        + (m_addrCount - 1) * sizeof(SOCKET_ADDRESS));

    for (ULONG i = 0; i < m_addrCount; i++)
    {
        pAddrs[i] = m_addrs[i];
        pAddressList->Address[i].lpSockaddr = reinterpret_cast<struct sockaddr*>(&pAddrs[i]);
        pAddressList->Address[i].iSockaddrLength = sizeof(struct sockaddr_in);
    }

    return S_OK;
}

HRESULT NdAdapter::CreateCompletionQueue(REFIID iid, HANDLE hOverlappedFile, ULONG queueDepth,
    USHORT group, KAFFINITY affinity, VOID** ppCompletionQueue)
{
    if (ppCompletionQueue == nullptr)
    {
        return E_INVALIDARG;
    }
    *ppCompletionQueue = nullptr;

    if (queueDepth > RDMA_MAX_COMPLETION_QUEUE_CAPACITY)
    {
        return ND_INVALID_PARAMETER_3;
    }

    UINT32 capacity = 1;
    while (capacity < queueDepth)
    {
        capacity <<= 1;
    }

    const SIZE_T ringSize = RDMA_CQ_RING_SIZE(capacity);
    wil::unique_virtualalloc_ptr<RDMA_CQ_RING_HEADER> ring{ static_cast<RDMA_CQ_RING_HEADER *>(VirtualAlloc(
        nullptr,
        ringSize,
        MEM_RESERVE | MEM_COMMIT,
        PAGE_READWRITE)) };
    if (!ring)
    {
        return ND_INSUFFICIENT_RESOURCES;
    }

    RDMA_CQ_ENTRY *const entries =
        reinterpret_cast<RDMA_CQ_ENTRY *>(reinterpret_cast<BYTE *>(ring.get()) + RDMA_CQ_RING_ENTRIES_OFFSET);
    for (UINT32 index = 0; index < capacity; ++index)
    {
        entries[index].Sequence = index;
    }

    RDMA_CQ_RING_HEADER *const ringAddress = ring.get();
    wistd::unique_ptr<NdCompletionQueue> completionQueue = wil::make_unique_nothrow<NdCompletionQueue>(
        m_deviceContext.Get(),
        hOverlappedFile,
        std::move(ring),
        capacity);
    if (!completionQueue)
    {
        return E_OUTOFMEMORY;
    }

    const HRESULT createStatus = completionQueue->Initialize();
    if (FAILED(createStatus))
    {
        NdTrace(
            "Adapter::CreateCQ IOCTL failed: 0x%08X",
            createStatus);
        return createStatus;
    }

    NdTrace(
        "Adapter::CreateCQ OK, id=%u, ring=%p, cap=%u",
        completionQueue->GetId(),
        static_cast<void *>(ringAddress),
        capacity);
    *ppCompletionQueue = static_cast<IND2CompletionQueue *>(completionQueue.release());
    return S_OK;
}

HRESULT NdAdapter::CreateMemoryRegion(REFIID iid, HANDLE hOverlappedFile, VOID** ppMemoryRegion)
{
    if (ppMemoryRegion == nullptr)
    {
        return E_INVALIDARG;
    }
    *ppMemoryRegion = nullptr;

    wistd::unique_ptr<NdMemoryRegion> memoryRegion = wil::make_unique_nothrow<NdMemoryRegion>(m_deviceContext.Get());
    if (memoryRegion == nullptr)
    {
        return E_OUTOFMEMORY;
    }

    *ppMemoryRegion = static_cast<IND2MemoryRegion *>(memoryRegion.release());
    return S_OK;
}

HRESULT NdAdapter::CreateMemoryWindow(REFIID iid, VOID** ppMemoryWindow)
{
    if (ppMemoryWindow != nullptr)
    {
        *ppMemoryWindow = nullptr;
    }

    return ND_NOT_SUPPORTED;
}

HRESULT NdAdapter::CreateSharedReceiveQueue(REFIID iid, HANDLE hOverlappedFile, ULONG queueDepth,
    ULONG maxRequestSge, ULONG notifyThreshold, USHORT group, KAFFINITY affinity, VOID** ppSharedReceiveQueue)
{
    return E_NOTIMPL;
}

HRESULT NdAdapter::CreateQueuePair(REFIID iid, IUnknown* pReceiveCompletionQueue,
    IUnknown* pInitiatorCompletionQueue, VOID* context, ULONG receiveQueueDepth, ULONG initiatorQueueDepth,
    ULONG maxReceiveRequestSge, ULONG maxInitiatorRequestSge, ULONG inlineDataSize, VOID** ppQueuePair)
{
    if (ppQueuePair == nullptr || receiveQueueDepth > NDPROV_MAX_QUEUE_DEPTH ||
        initiatorQueueDepth > RDMA_MAX_QUEUE_PAIR_REQUESTS)
    {
        return E_INVALIDARG;
    }
    *ppQueuePair = nullptr;

    // Query reports MaxReceiveSge and MaxInitiatorSge of 1.
    if (maxReceiveRequestSge > 1)
    {
        return ND_INVALID_PARAMETER_7;
    }

    if (maxInitiatorRequestSge > 1)
    {
        return ND_INVALID_PARAMETER_8;
    }

    NdCompletionQueue* pSendCqObj = nullptr;
    NdCompletionQueue* pRecvCqObj = nullptr;

    if (pInitiatorCompletionQueue)
    {
        IND2CompletionQueue* pInitCq = nullptr;
        if (SUCCEEDED(pInitiatorCompletionQueue->QueryInterface(IID_IND2CompletionQueue, reinterpret_cast<void**>(&pInitCq))))
        {
            pSendCqObj = static_cast<NdCompletionQueue*>(pInitCq);
        }
    }

    if (pReceiveCompletionQueue)
    {
        IND2CompletionQueue* pRecvCq = nullptr;
        if (SUCCEEDED(pReceiveCompletionQueue->QueryInterface(IID_IND2CompletionQueue, reinterpret_cast<void**>(&pRecvCq))))
        {
            pRecvCqObj = static_cast<NdCompletionQueue*>(pRecvCq);
        }
    }

    wistd::unique_ptr<NdQueuePair> queuePair = wil::make_unique_nothrow<NdQueuePair>(
        m_deviceContext.Get(),
        context,
        pSendCqObj,
        pRecvCqObj,
        receiveQueueDepth,
        maxReceiveRequestSge,
        maxInitiatorRequestSge);

    if (pSendCqObj)
    {
        pSendCqObj->Release();
    }
    if (pRecvCqObj)
    {
        pRecvCqObj->Release();
    }

    if (queuePair == nullptr)
    {
        return E_OUTOFMEMORY;
    }

    NdTrace("Adapter::CreateQP created unbound logical QP");
    *ppQueuePair = static_cast<IND2QueuePair *>(queuePair.release());
    return S_OK;
}

HRESULT NdAdapter::CreateQueuePairWithSrq(REFIID iid, IUnknown* pReceiveCompletionQueue,
    IUnknown* pInitiatorCompletionQueue, IUnknown* pSharedReceiveQueue, VOID* context,
    ULONG initiatorQueueDepth, ULONG maxInitiatorRequestSge, ULONG inlineDataSize, VOID** ppQueuePair)
{
    return E_NOTIMPL;
}

HRESULT NdAdapter::CreateConnector(REFIID iid, HANDLE hOverlappedFile, VOID** ppConnector)
{
    if (ppConnector == nullptr)
    {
        return E_INVALIDARG;
    }
    *ppConnector = nullptr;

    wistd::unique_ptr<NdConnector> connector = wil::make_unique_nothrow<NdConnector>(m_deviceContext.Get());
    if (connector == nullptr)
    {
        return E_OUTOFMEMORY;
    }

    const HRESULT result = connector->Initialize(hOverlappedFile);
    if (FAILED(result))
    {
        return result;
    }

    *ppConnector = static_cast<IND2Connector *>(connector.release());
    return S_OK;
}

HRESULT NdAdapter::CreateListener(REFIID iid, HANDLE hOverlappedFile, VOID** ppListener)
{
    if (ppListener == nullptr)
    {
        return E_INVALIDARG;
    }
    *ppListener = nullptr;

    wistd::unique_ptr<NdListener> listener = wil::make_unique_nothrow<NdListener>(m_deviceContext.Get());
    if (listener == nullptr)
    {
        return E_OUTOFMEMORY;
    }

    const HRESULT result = listener->Initialize(hOverlappedFile);
    if (FAILED(result))
    {
        return result;
    }

    *ppListener = static_cast<IND2Listener *>(listener.release());
    return S_OK;
}


// ============================================================================
// NdProvider
// ============================================================================

HRESULT
NdProvider::Create(
    _Outptr_ NdProvider **provider)
{
    if (provider == nullptr)
    {
        return E_INVALIDARG;
    }

    *provider = nullptr;

    wistd::unique_ptr<NdProvider> providerInstance = wil::make_unique_nothrow<NdProvider>();
    if (providerInstance == nullptr)
    {
        return E_OUTOFMEMORY;
    }

    const HRESULT status = providerInstance->EnumerateInterfaces();
    if (FAILED(status))
    {
        return status;
    }

    *provider = providerInstance.release();
    return S_OK;
}

HRESULT NdProvider::EnumerateInterfaces()
{
    ULONG bufferLen = 0;
    CONFIGRET cr = CM_Get_Device_Interface_List_SizeW(
        &bufferLen, const_cast<GUID*>(&GUID_DEVINTERFACE_RDMA),
        nullptr, CM_GET_DEVICE_INTERFACE_LIST_PRESENT);

    if (cr != CR_SUCCESS || bufferLen <= 1)
    {
        NdTrace("No RDMA device interfaces found (cr=%u, len=%u)", cr, bufferLen);
        m_interfaceCount = 0;
        return S_OK;
    }

    wistd::unique_ptr<WCHAR[]> buffer = wil::make_unique_nothrow<WCHAR[]>(bufferLen);
    if (buffer == nullptr)
    {
        return E_OUTOFMEMORY;
    }

    cr = CM_Get_Device_Interface_ListW(
        const_cast<GUID *>(&GUID_DEVINTERFACE_RDMA),
        nullptr,
        buffer.get(),
        bufferLen,
        CM_GET_DEVICE_INTERFACE_LIST_PRESENT);

    if (cr != CR_SUCCESS)
    {
        NdTrace("CM_Get_Device_Interface_List failed: %u", cr);
        return HRESULT_FROM_WIN32(CM_MapCrToWin32Err(cr, ERROR_GEN_FAILURE));
    }

    // Parse the multi-sz buffer
    m_interfaceCount = 0;
    WCHAR *current = buffer.get();
    while (*current != L'\0' && m_interfaceCount < c_maxInterfaces)
    {
        const size_t length = wcslen(current);
        m_symbolicLinks[m_interfaceCount] = wil::make_unique_nothrow<WCHAR[]>(length + 1);
        if (m_symbolicLinks[m_interfaceCount] == nullptr)
        {
            return E_OUTOFMEMORY;
        }
        wcscpy_s(
            m_symbolicLinks[m_interfaceCount].get(),
            length + 1,
            current);

        NET_LUID luid{};
        DEVPROPTYPE propertyType{ 0 };
        ULONG propertySize{ sizeof(luid) };
        const CONFIGRET propertyResult = CM_Get_Device_Interface_PropertyW(
            current,
            &DEVPKEY_RdmaAdapter_Luid,
            &propertyType,
            reinterpret_cast<PBYTE>(&luid),
            &propertySize,
            0);

        if (propertyResult == CR_SUCCESS && propertyType == DEVPROP_TYPE_UINT64)
        {
            m_luids[m_interfaceCount] = luid;
            NdTrace("Interface[%u] LUID=0x%llX",
                m_interfaceCount, luid.Value);
        }
        else
        {
            NdTrace(
                "Interface[%u] LUID query failed (cr=%u)",
                m_interfaceCount,
                propertyResult);
        }

            NdTrace("Found interface[%u]: %ls", m_interfaceCount, current);
        m_interfaceCount++;
        current += length + 1;
    }
    NdTrace("Enumerated %u RDMA interfaces", m_interfaceCount);
    return S_OK;
}

HRESULT NdProvider::QueryInterface(REFIID riid, LPVOID* ppvObj)
{
    if (InlineIsEqualGUID(riid, IID_IUnknown) || InlineIsEqualGUID(riid, IID_IND2Provider))
    {
        *ppvObj = static_cast<IND2Provider*>(this);
        AddRef();
        return S_OK;
    }
    *ppvObj = nullptr;
    return E_NOINTERFACE;
}

HRESULT NdProvider::QueryAddressList(SOCKET_ADDRESS_LIST* pAddressList, ULONG* pcbAddressList)
{
    // Gather all IPv4 unicast addresses across all our adapters
    unique_unicast_ip_address_table table;
    const DWORD err = GetUnicastIpAddressTable(
        AF_INET,
        table.addressof());
    if (err != NO_ERROR)
    {
        return HRESULT_FROM_WIN32(err);
    }

    // Count addresses that match our adapter LUIDs
    struct MatchedAddr
    {
        struct sockaddr_in addr;
        ULONG interfaceIndex;
    };
    MatchedAddr matched[64] = {};
    ULONG matchCount = 0;

    for (ULONG t = 0; t < table.get()->NumEntries && matchCount < 64; t++)
    {
        const MIB_UNICASTIPADDRESS_ROW &row = table.get()->Table[t];
        for (ULONG i = 0; i < m_interfaceCount; i++)
        {
            if (row.InterfaceLuid.Value == m_luids[i].Value)
            {
                memset(&matched[matchCount].addr, 0, sizeof(struct sockaddr_in));
                matched[matchCount].addr.sin_family = AF_INET;
                matched[matchCount].addr.sin_addr = row.Address.Ipv4.sin_addr;
                matched[matchCount].interfaceIndex = i;
                matchCount++;
                break;
            }
        }
    }

    if (matchCount == 0)
    {
        // No addresses found — return empty list
        ULONG needed = sizeof(SOCKET_ADDRESS_LIST);
        if (pAddressList == nullptr || *pcbAddressList < needed)
        {
            *pcbAddressList = needed;
            return ND_BUFFER_OVERFLOW;
        }
        *pcbAddressList = needed;
        pAddressList->iAddressCount = 0;
        return S_OK;
    }

    ULONG needed = sizeof(SOCKET_ADDRESS_LIST)
        + (matchCount - 1) * sizeof(SOCKET_ADDRESS)
        + matchCount * sizeof(struct sockaddr_in);

    if (pAddressList == nullptr || *pcbAddressList < needed)
    {
        *pcbAddressList = needed;
        return ND_BUFFER_OVERFLOW;
    }

    *pcbAddressList = needed;
    pAddressList->iAddressCount = static_cast<INT>(matchCount);

    struct sockaddr_in* pAddrs = reinterpret_cast<struct sockaddr_in*>(
        reinterpret_cast<BYTE*>(pAddressList) + sizeof(SOCKET_ADDRESS_LIST)
        + (matchCount - 1) * sizeof(SOCKET_ADDRESS));

    for (ULONG i = 0; i < matchCount; i++)
    {
        pAddrs[i] = matched[i].addr;
        pAddressList->Address[i].lpSockaddr = reinterpret_cast<struct sockaddr*>(&pAddrs[i]);
        pAddressList->Address[i].iSockaddrLength = sizeof(struct sockaddr_in);
    }

    return S_OK;
}

HRESULT NdProvider::ResolveAddress(const struct sockaddr* pAddress, ULONG cbAddress, UINT64* pAdapterId)
{
    if (cbAddress < sizeof(struct sockaddr_in))
    {
        return ND_INVALID_ADDRESS;
    }

    const struct sockaddr_in* pAddr = reinterpret_cast<const struct sockaddr_in*>(pAddress);
    if (pAddr->sin_family != AF_INET)
    {
        return ND_INVALID_ADDRESS;
    }

    // Look up which adapter LUID owns this address
    unique_unicast_ip_address_table table;
    const DWORD err = GetUnicastIpAddressTable(
        AF_INET,
        table.addressof());
    if (err != NO_ERROR)
    {
        return HRESULT_FROM_WIN32(err);
    }

    HRESULT hr = ND_INVALID_ADDRESS;
    for (ULONG t = 0; t < table.get()->NumEntries; t++)
    {
        const MIB_UNICASTIPADDRESS_ROW &row = table.get()->Table[t];
        if (row.Address.Ipv4.sin_addr.s_addr != pAddr->sin_addr.s_addr)
        {
            continue;
        }

        for (ULONG i = 0; i < m_interfaceCount; i++)
        {
            if (row.InterfaceLuid.Value == m_luids[i].Value)
            {
                *pAdapterId = static_cast<UINT64>(i + 1);
                NdTrace("Provider::ResolveAddress %08X -> adapterId=%llu",
                    ntohl(pAddr->sin_addr.s_addr), *pAdapterId);
                hr = S_OK;
                goto done;
            }
        }
    }

done:
    return hr;
}

HRESULT NdProvider::OpenAdapter(REFIID iid, UINT64 adapterId, VOID** ppAdapter)
{
    if (ppAdapter == nullptr)
    {
        return E_INVALIDARG;
    }
    *ppAdapter = nullptr;

    if (adapterId == 0 || adapterId > m_interfaceCount)
    {
        NdTrace("Provider::OpenAdapter - invalid adapterId %llu", adapterId);
        return ND_INVALID_ADDRESS;
    }

    ULONG index = static_cast<ULONG>(adapterId - 1);

    NdAdapter *adapter{ nullptr };
    const HRESULT status = NdAdapter::Create(
        adapterId,
        m_symbolicLinks[index].get(),
        m_luids[index],
        &adapter);
    if (FAILED(status))
    {
        return status;
    }

    *ppAdapter = static_cast<IND2Adapter *>(adapter);
    NdTrace("Provider::OpenAdapter OK for interface[%u]", index);
    return S_OK;
}
