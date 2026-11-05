// Copyright (c) Microsoft Corporation. All rights reserved.

//
// ndprov.h - NetworkDirect v2 Provider backed by RDMA IOCTLs
//
// Implements IND2 interfaces by forwarding operations to the
// USB4 P2P RDMA kernel driver via IOCTLs defined in RdmaIoctl.h.
//

#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>
#include <TraceLoggingProvider.h>
#include <winmeta.h>
#include <cfgmgr32.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <unknwn.h>
#include <wil/resource.h>
#include <winioctl.h>
#include <winternl.h>
#include <wrl/client.h>

#include <array>
#include <memory>
#include <set>
#include <stdarg.h>
#include <stdio.h>
#include "ndspi.h"
#include "RdmaIoctl.h"

// {1F6A4848-5D04-4C77-B96C-4AB28A39449D}
DEFINE_GUID(CLSID_NdDummyProvider,
    0x1f6a4848, 0x5d04, 0x4c77, 0xb9, 0x6c, 0x4a, 0xb2, 0x8a, 0x39, 0x44, 0x9d);

#define NDPROV_MAX_QUEUE_DEPTH 1024

using unique_unicast_ip_address_table = wil::unique_any<PMIB_UNICASTIPADDRESS_TABLE, decltype(&::FreeMibTable), &::FreeMibTable>;

TRACELOGGING_DECLARE_PROVIDER(g_ndprovTraceProvider);

void
NdTrace(
    _In_z_ _Printf_format_string_ const char *format,
    ...);

//
// Helper base class for COM ref counting
//
class ComBase
{
protected:
    volatile LONG m_refCount = 1;

    virtual ~ComBase() {}

public:
    ULONG STDMETHODCALLTYPE AddRef() { return InterlockedIncrement(&m_refCount); }
    ULONG STDMETHODCALLTYPE Release()
    {
        LONG ref = InterlockedDecrement(&m_refCount);
        if (ref == 0)
        {
            delete this;
        }
        return ref;
    }
};


//
// Helper: Send a synchronous DeviceIoControl and return HRESULT.
//
inline HRESULT SendIoctl(
    HANDLE hDevice,
    DWORD ioctl,
    const void* pInBuf,
    DWORD cbInBuf,
    void* pOutBuf,
    DWORD cbOutBuf,
    DWORD* pcbReturned = nullptr)
{
    DWORD bytesReturned = 0;
    wil::unique_handle event{ CreateEventW(
        nullptr,
        TRUE,
        FALSE,
        nullptr) };
    if (!event)
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    OVERLAPPED ov{};
    ov.hEvent = event.get();

    BOOL ok = DeviceIoControl(hDevice, ioctl, const_cast<void*>(pInBuf), cbInBuf,
        pOutBuf, cbOutBuf, &bytesReturned, &ov);

    HRESULT hr;
    if (!ok && GetLastError() == ERROR_IO_PENDING)
    {
        WaitForSingleObject(ov.hEvent, INFINITE);
        ok = ::GetOverlappedResult(hDevice, &ov, &bytesReturned, FALSE);
        hr = ok ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    }
    else
    {
        hr = ok ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    }

    if (pcbReturned)
    {
        *pcbReturned = bytesReturned;
    }
    return hr;
}

// Sends a data-path IOCTL. The driver completes these through fast I/O, so
// they don't pend and need no event; one that pends is waited for through the
// handle.
inline
HRESULT
SendDataPathIoctl(
    HANDLE hDevice,
    DWORD ioctl,
    const void *pInBuf,
    DWORD cbInBuf)
{
    IO_STATUS_BLOCK ioStatus{};
    NTSTATUS status = NtDeviceIoControlFile(
        hDevice,
        nullptr,
        nullptr,
        nullptr,
        &ioStatus,
        ioctl,
        const_cast<void *>(pInBuf),
        cbInBuf,
        nullptr,
        0);
    if (status == STATUS_PENDING)
    {
        WaitForSingleObject(
            hDevice,
            INFINITE);
        status = ioStatus.Status;
    }

    HRESULT hr = S_OK;
    if (status < 0)
    {
        const ULONG error = RtlNtStatusToDosError(status);
        hr = error == ERROR_NOACCESS ? ND_ACCESS_VIOLATION : HRESULT_FROM_WIN32(error);
    }

    return hr;
}

inline
void
CloseDeviceNotification(
    HCMNOTIFICATION notification) noexcept
{
    if (notification != nullptr)
    {
        CM_Unregister_Notification(notification);
    }
}

using unique_device_notification = wil::unique_any<HCMNOTIFICATION, decltype(&CloseDeviceNotification), CloseDeviceNotification>;

class NdDeviceContext : public ComBase
{
public:

    ~NdDeviceContext() override = default;

    static
    HRESULT
    Create(
        _In_z_ const WCHAR *symbolicLink,
        _Outptr_ NdDeviceContext **deviceContext);

    HRESULT
    SendIoctl(
        DWORD ioctl,
        _In_reads_bytes_opt_(inputSize) const void *input,
        DWORD inputSize,
        _Out_writes_bytes_opt_(outputSize) void *output,
        DWORD outputSize,
        _Out_opt_ DWORD *bytesReturned = nullptr);

    BOOL
    StartIoctl(
        DWORD ioctl,
        _In_reads_bytes_opt_(inputSize) const void *input,
        DWORD inputSize,
        _Out_writes_bytes_opt_(outputSize) void *output,
        DWORD outputSize,
        _Out_opt_ DWORD *bytesReturned,
        _Inout_opt_ OVERLAPPED *overlapped);

    BOOL
    CancelIo(
        _In_opt_ OVERLAPPED *overlapped);

    BOOL
    GetIoResult(
        _In_ OVERLAPPED *overlapped,
        _Out_ DWORD *bytesTransferred,
        BOOL wait);

    void
    AcquireUsage();
    void
    ReleaseUsage();
    HANDLE
    GetHandleLocked() const;

    const WCHAR *
    GetSymbolicLink() const
    {
        return m_symbolicLink.get();
    }

    // Path that opens a queue pair handle on the RDMA interface.
    const WCHAR *
    GetQueuePairPath() const
    {
        return m_queuePairPath.get();
    }

    // Path that opens an overlapped file handle on the RDMA interface.
    const WCHAR *
    GetOverlappedPath() const
    {
        return m_overlappedPath.get();
    }

    // Queue pair handles keep the device open, so they are closed with the
    // device handle when the device is removed.
    void
    RegisterQueuePair(
        _Inout_ LIST_ENTRY *queuePairLink);
    void
    UnregisterQueuePair(
        _Inout_ LIST_ENTRY *queuePairLink);

private:

    HRESULT
    Open(
        _In_z_ const WCHAR *symbolicLink);
    void
    CloseForRemoval();

    static
    DWORD CALLBACK
    DeviceNotificationCallback(
        HCMNOTIFICATION notification,
        _In_opt_ void *context,
        CM_NOTIFY_ACTION action,
        _In_reads_bytes_(eventDataSize) PCM_NOTIFY_EVENT_DATA eventData,
        DWORD eventDataSize);

    wil::unique_hfile m_deviceHandle{};
    wistd::unique_ptr<WCHAR[]> m_symbolicLink{};
    wistd::unique_ptr<WCHAR[]> m_queuePairPath{};
    wistd::unique_ptr<WCHAR[]> m_overlappedPath{};
    SRWLOCK m_deviceLock{};

    // Taken before any queue pair's lock.
    SRWLOCK m_queuePairLock{};
    LIST_ENTRY m_queuePairs{ &m_queuePairs,
                             &m_queuePairs };

    // Declared last, so they are unregistered first: CM_Unregister_Notification
    // waits for running callbacks, which use the members above.
    unique_device_notification m_deviceHandleNotification{};
    unique_device_notification m_deviceInterfaceNotification{};
};

class NdDeviceUsageGuard
{
public:

    explicit NdDeviceUsageGuard(
        _In_ NdDeviceContext *deviceContext)
        : m_deviceContext{ deviceContext }
    {
        m_deviceContext->AcquireUsage();
    }

    ~NdDeviceUsageGuard()
    {
        m_deviceContext->ReleaseUsage();
    }

    NdDeviceUsageGuard(
        const NdDeviceUsageGuard &) = delete;
    NdDeviceUsageGuard &
    operator=(
        const NdDeviceUsageGuard &) = delete;

    bool
    IsAvailable() const
    {
        return GetHandle() != INVALID_HANDLE_VALUE;
    }

    HANDLE
    GetHandle() const
    {
        return m_deviceContext->GetHandleLocked();
    }

private:

    NdDeviceContext *m_deviceContext;
};



//
// IND2CompletionQueue implementation
//
class NdCompletionQueue : public ComBase, public IND2CompletionQueue
{
public:

    // overlappedFile is owned by the application; nullptr selects the RDMA
    // device handle for notifications.
    NdCompletionQueue(
        _In_ NdDeviceContext *deviceContext,
        _In_opt_ HANDLE overlappedFile,
        wil::unique_virtualalloc_ptr<RDMA_CQ_RING_HEADER> &&ring,
        UINT32 capacity);
    ~NdCompletionQueue() override;

    // Hands the ring to the driver. On failure the queue owns no driver state.
    [[nodiscard]]
    HRESULT
    Initialize();

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, LPVOID* ppvObj) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return ComBase::AddRef(); }
    ULONG STDMETHODCALLTYPE Release() override { return ComBase::Release(); }

    // IND2Overlapped
    HRESULT STDMETHODCALLTYPE CancelOverlappedRequests() override;
    HRESULT STDMETHODCALLTYPE GetOverlappedResult(OVERLAPPED* pOverlapped, BOOL wait) override;

    // IND2CompletionQueue
    HRESULT STDMETHODCALLTYPE GetNotifyAffinity(USHORT* pGroup, KAFFINITY* pAffinity) override;
    HRESULT STDMETHODCALLTYPE Resize(ULONG queueDepth) override;
    HRESULT STDMETHODCALLTYPE Notify(ULONG type, OVERLAPPED* pOverlapped) override;
    ULONG STDMETHODCALLTYPE GetResults(ND2_RESULT results[], ULONG nResults) override;

    UINT32 GetId() const { return m_cqId; }
    bool
    IsFailed() const;

private:

    // Runs operation with the handle that notification requests are issued on.
    template <typename Operation>
    [[nodiscard]]
    HRESULT
    RunWithNotifyHandle(
        Operation &&operation);

    RDMA_CQ_ENTRY &
    GetEntry(
        ULONG position) const;
    LONG
    GetState() const;
    bool
    IsReady(
        ULONG type) const;
    bool
    SpinUntilReady(
        ULONG type) const;
    [[nodiscard]]
    HRESULT
    MapNotifyError(
        DWORD error) const;

    Microsoft::WRL::ComPtr<NdDeviceContext> m_deviceContext{};
    HANDLE m_overlappedFile = nullptr;
    wil::unique_virtualalloc_ptr<RDMA_CQ_RING_HEADER> m_ring{};
    RDMA_CQ_ENTRY *m_entries = nullptr;
    UINT32 m_capacity = 0;
    UINT32 m_positionMask = 0;
    UINT32 m_cqId = RDMA_INVALID_ID;
    LONGLONG m_notifySpinTicks = 0;
    SRWLOCK m_notifyLock{};
    std::set<OVERLAPPED *> m_notifyRequests{};
};


//
// IND2MemoryRegion implementation
//
class NdMemoryRegion : public ComBase, public IND2MemoryRegion
{
public:

    explicit NdMemoryRegion(
        _In_ NdDeviceContext *deviceContext);
    ~NdMemoryRegion() override;

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, LPVOID* ppvObj) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return ComBase::AddRef(); }
    ULONG STDMETHODCALLTYPE Release() override { return ComBase::Release(); }

    // IND2Overlapped
    HRESULT STDMETHODCALLTYPE CancelOverlappedRequests() override;
    HRESULT STDMETHODCALLTYPE GetOverlappedResult(OVERLAPPED* pOverlapped, BOOL wait) override;

    // IND2MemoryRegion
    HRESULT STDMETHODCALLTYPE Register(const VOID* pBuffer, SIZE_T cbBuffer, ULONG flags, OVERLAPPED* pOverlapped) override;
    HRESULT STDMETHODCALLTYPE Deregister(OVERLAPPED* pOverlapped) override;
    UINT32 STDMETHODCALLTYPE GetLocalToken() override;
    UINT32 STDMETHODCALLTYPE GetRemoteToken() override;

private:

    Microsoft::WRL::ComPtr<NdDeviceContext> m_deviceContext{};
    const VOID* m_pBuffer = nullptr;
    SIZE_T m_cbBuffer = 0;
    UINT32 m_mrKey = RDMA_INVALID_ID;
    bool m_registered = false;
};


//
// IND2QueuePair implementation
//
class NdQueuePair : public ComBase, public IND2QueuePair
{
public:

    NdQueuePair(
        _In_ NdDeviceContext *deviceContext,
        VOID *context,
        NdCompletionQueue *pSendCq,
        NdCompletionQueue *pRecvCq,
        ULONG receiveQueueDepth,
        ULONG maxReceiveRequestSge,
        ULONG maxInitiatorRequestSge);

    ~NdQueuePair() override;

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, LPVOID* ppvObj) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return ComBase::AddRef(); }
    ULONG STDMETHODCALLTYPE Release() override { return ComBase::Release(); }

    // IND2QueuePair
    HRESULT STDMETHODCALLTYPE Flush() override;
    HRESULT STDMETHODCALLTYPE Send(VOID* requestContext, const ND2_SGE sge[], ULONG nSge, ULONG flags) override;
    HRESULT STDMETHODCALLTYPE Receive(VOID* requestContext, const ND2_SGE sge[], ULONG nSge) override;
    HRESULT STDMETHODCALLTYPE Bind(VOID* requestContext, IUnknown* pMemoryRegion, IUnknown* pMemoryWindow,
        const VOID* pBuffer, SIZE_T cbBuffer, ULONG flags) override;
    HRESULT STDMETHODCALLTYPE Invalidate(VOID* requestContext, IUnknown* pMemoryWindow, ULONG flags) override;
    HRESULT STDMETHODCALLTYPE Read(VOID* requestContext, const ND2_SGE sge[], ULONG nSge,
        UINT64 remoteAddress, UINT32 remoteToken, ULONG flags) override;
    HRESULT STDMETHODCALLTYPE Write(VOID* requestContext, const ND2_SGE sge[], ULONG nSge,
        UINT64 remoteAddress, UINT32 remoteToken, ULONG flags) override;

    VOID* GetContext() const { return m_context; }

    // Starts binding the queue pair to a tunnel for a connection. The binding
    // completes asynchronously, which signals the bind event; EndBind then
    // ends it.
    HRESULT
    BeginBind(
        const RDMA_CONNECTION_TOKEN &connectionToken,
        RDMA_CONNECTION_ROLE role);

    HANDLE
    GetBindEvent() const
    {
        return m_bindEvent.get();
    }

    // Waits for the binding to complete, canceling it first if cancel. Once
    // bound, the queue pair posts the receives it held and watches for the
    // release of its tunnel. A failed binding closes the queue pair handle.
    HRESULT
    EndBind(
        bool cancel);

    // Signaled once both sides released the bound queue pair's tunnel, or its
    // handle closed.
    HANDLE
    GetReleaseEvent() const
    {
        return m_releaseEvent.get();
    }

    // After the release event: ND_SUCCESS for a disconnection, otherwise
    // ND_CONNECTION_ABORTED.
    HRESULT
    GetReleaseStatus();

    void
    AbortBinding();

    // Flushes the queue pair as a disconnection, waits up to 5 seconds for the
    // peer to release the tunnel, then closes the queue pair handle.
    HRESULT
    Disconnect();

    // Closes the queue pair handle because the device is being removed.
    void
    CloseForRemoval();

    static
    NdQueuePair *
    FromDeviceLink(
        _In_ LIST_ENTRY *entry);

private:

    struct PendingReceive
    {
        VOID *RequestContext{ nullptr };
        ND2_SGE Sge{};
    };

    HRESULT
    PostReceive(
        VOID *requestContext,
        const ND2_SGE &sge);

    // Returns why Send and Receive must fail, or S_OK. Requires m_lock.
    HRESULT
    GetDataPathStatus() const;

    // Closing the queue pair handle destroys the driver's queue pair.
    void
    CloseQueuePairFile();
    HRESULT
    FlushLocked(
        UINT32 flags = 0);

    // Watches for the release of the bound queue pair's tunnel.
    void
    StartReleaseNotificationLocked();
    // Records the result of the release notification once it completed.
    void
    UpdateReleaseStatusLocked(
        BOOL wait);

    static const ULONG c_maxPendingReceives = NDPROV_MAX_QUEUE_DEPTH;

    Microsoft::WRL::ComPtr<NdDeviceContext> m_deviceContext{};
    // Queue pair handle, valid once the queue pair is bound to a tunnel.
    // Guarded by m_lock.
    wil::unique_hfile m_queuePairFile{};
    // Queue pair handle while its binding is in progress, owned by the thread
    // running BeginBind and EndBind.
    wil::unique_hfile m_bindFile{};
    OVERLAPPED m_bindOverlapped{};
    wil::unique_event_nothrow m_bindEvent{};
    RDMA_CREATE_QUEUE_PAIR_OUTPUT m_bindOutput{};
    // The pending IOCTL_RDMA_NOTIFY_QUEUE_PAIR_RELEASE and its result.
    // Guarded by m_lock.
    OVERLAPPED m_releaseOverlapped{};
    wil::unique_event_nothrow m_releaseEvent{};
    RDMA_QUEUE_PAIR_RELEASE_OUTPUT m_releaseOutput{};
    bool m_releasePending{ false };
    HRESULT m_releaseStatus{ ND_PENDING };
    VOID *m_context{ nullptr };
    NdCompletionQueue *m_pSendCq{ nullptr };
    NdCompletionQueue *m_pRecvCq{ nullptr };
    UINT32 m_sendCqId{ RDMA_INVALID_ID };
    UINT32 m_recvCqId{ RDMA_INVALID_ID };
    ULONG m_receiveQueueDepth{ 0 };
    PendingReceive m_pendingReceives[c_maxPendingReceives]{};
    ULONG m_pendingReceiveCount{ 0 };
    SRWLOCK m_lock{};
    bool m_disconnected{ false };
    bool m_removed{ false };
    // Entry in the device context's queue pair list. Guarded by its lock.
    LIST_ENTRY m_deviceLink{ &m_deviceLink,
                             &m_deviceLink };
    // SGE limits given at creation, each at most 1.
    ULONG m_maxReceiveRequestSge{ 0 };
    ULONG m_maxInitiatorRequestSge{ 0 };
};


//
// ============================================================================
// TCP wire protocol for RDMA connection management
// ============================================================================
//

// Maximum private data exchanged during connection setup.
#define NDPROV_MAX_PRIVATE_DATA 1024
// Version 5: TCP carries only the connection request and its answer; the
// drivers agree on the tunnel and report disconnections.
#define NDPROV_CM_VERSION 5

enum NdCmMessageType : UINT32
{
    ND_CM_CONNECT_REQUEST = 1,
    ND_CM_CONNECT_ACCEPT = 2,
    ND_CM_CONNECT_REJECT = 3,
    // The passive side's binding failed after its application accepted.
    ND_CM_CONNECT_ABORT = 4,
};

#pragma pack(                                                                                                                    \
    push,                                                                                                                        \
    1)

// Header common to all CM messages.
struct NdCmHeader
{
    NdCmMessageType Type;
    UINT32 Version;
    RDMA_CONNECTION_TOKEN ConnectionToken;
};

struct NdCmConnectRequest
{
    NdCmHeader Header;
    UINT32 InboundReadLimit;
    UINT32 OutboundReadLimit;
    UINT32 PrivateDataLength;
    BYTE PrivateData[NDPROV_MAX_PRIVATE_DATA];
};

struct NdCmConnectAccept
{
    NdCmHeader Header;
    UINT32 InboundReadLimit;
    UINT32 OutboundReadLimit;
    UINT32 PrivateDataLength;
    BYTE PrivateData[NDPROV_MAX_PRIVATE_DATA];
};

struct NdCmConnectReject
{
    NdCmHeader Header;
    UINT32 PrivateDataLength;
    BYTE PrivateData[NDPROV_MAX_PRIVATE_DATA];
};

struct NdCmConnectAbort
{
    NdCmHeader Header;
    HRESULT Status;
};

#pragma pack(pop)

//
// Completes connector and listener requests, which the provider runs itself,
// like any other overlapped I/O, the way Winsock's WPUCompleteOverlappedRequest
// does. Once a request's result is known, the provider sends
// IOCTL_RDMA_COMPLETE_OVERLAPPED on the application's overlapped file, with the
// application's OVERLAPPED and the result. The driver completes it with that
// status, so the I/O manager stores the status in the OVERLAPPED, signals its
// event and queues a packet to the file's completion port.
//
class NdOverlappedRequests
{
public:

    // Duplicates the handle of the application's overlapped file, which stays
    // open as long as requests can complete through it.
    HRESULT
    Initialize(
        HANDLE overlappedFile);

    // Marks a request in progress, as the I/O manager does when it starts one:
    // its status is pending and its event reset.
    static
    void
    Start(
        _Inout_ OVERLAPPED *overlapped);

    // Completes a started request with its result. Returns the value for the
    // method that started it to return: ND_PENDING once the request completes
    // through the OVERLAPPED, or the result itself if it couldn't.
    HRESULT
    Complete(
        _Inout_ OVERLAPPED *overlapped,
        HRESULT status) const;

    static
    HRESULT
    GetResult(
        _In_ OVERLAPPED *overlapped,
        BOOL wait);

private:

    wil::unique_handle m_overlappedFile{};
};

//
// Work that runs on the thread pool, such as the body of a connector or
// listener request, which then completes the request through
// NdOverlappedRequests.
//
class NdWork
{
public:

    virtual ~NdWork() = default;

    // Runs the work on the thread pool, keeping ndprov.dll loaded until it
    // returns, then deletes it. Runs it on the calling thread instead if the
    // thread pool can't take it.
    static
    void
    Submit(
        wistd::unique_ptr<NdWork> work);

private:

    virtual
    void
    Run() = 0;

    static
    void CALLBACK
    ThreadPoolCallback(
        _Inout_ PTP_CALLBACK_INSTANCE instance,
        _Inout_opt_ void *context);

    HMODULE m_module{ nullptr };
};


//
// IND2Connector implementation — uses TCP for connection management
//
class NdConnector : public ComBase, public IND2Connector
{
public:

    explicit NdConnector(
        _In_ NdDeviceContext *deviceContext);
    ~NdConnector() override;

    HRESULT
    Initialize(
        HANDLE overlappedFile);

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, LPVOID* ppvObj) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return ComBase::AddRef(); }
    ULONG STDMETHODCALLTYPE Release() override { return ComBase::Release(); }

    // IND2Overlapped
    HRESULT STDMETHODCALLTYPE CancelOverlappedRequests() override;
    HRESULT STDMETHODCALLTYPE GetOverlappedResult(OVERLAPPED* pOverlapped, BOOL wait) override;

    // IND2Connector
    HRESULT STDMETHODCALLTYPE Bind(const struct sockaddr* pAddress, ULONG cbAddress) override;
    HRESULT STDMETHODCALLTYPE Connect(IUnknown* pQueuePair, const struct sockaddr* pDestAddress, ULONG cbDestAddress,
        ULONG inboundReadLimit, ULONG outboundReadLimit,
        const VOID* pPrivateData, ULONG cbPrivateData, OVERLAPPED* pOverlapped) override;
    HRESULT STDMETHODCALLTYPE CompleteConnect(OVERLAPPED* pOverlapped) override;
    HRESULT STDMETHODCALLTYPE Accept(IUnknown* pQueuePair, ULONG inboundReadLimit, ULONG outboundReadLimit,
        const VOID* pPrivateData, ULONG cbPrivateData, OVERLAPPED* pOverlapped) override;
    HRESULT STDMETHODCALLTYPE Reject(const VOID* pPrivateData, ULONG cbPrivateData) override;
    HRESULT STDMETHODCALLTYPE GetReadLimits(ULONG* pInboundReadLimit, ULONG* pOutboundReadLimit) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(VOID* pPrivateData, ULONG* pcbPrivateData) override;
    HRESULT STDMETHODCALLTYPE GetLocalAddress(struct sockaddr* pAddress, ULONG* pcbAddress) override;
    HRESULT STDMETHODCALLTYPE GetPeerAddress(struct sockaddr* pAddress, ULONG* pcbAddress) override;
    HRESULT STDMETHODCALLTYPE NotifyDisconnect(OVERLAPPED* pOverlapped) override;
    HRESULT STDMETHODCALLTYPE Disconnect(OVERLAPPED* pOverlapped) override;

    // Called by NdListener to hand off an accepted TCP socket with the
    // peer's connect request already received.
    bool
    SetAcceptedSocket(
        SOCKET sock,
        const struct sockaddr_in &localAddr,
        const struct sockaddr_in &peerAddr,
        const NdCmConnectRequest &request);

    // Helpers for reliable TCP send/recv of CM messages. On failure,
    // socketError receives the Winsock error, or 0 if the connection closed.
    static
    bool
    SendMsg(
        SOCKET s,
        const void *buf,
        int len,
        _Out_opt_ int *socketError = nullptr);
    static
    bool
    RecvMsg(
        SOCKET s,
        void *buf,
        int len,
        _Out_opt_ int *socketError = nullptr);

private:

    friend class NdConnectorWork;

    static
    RDMA_CONNECTION_TOKEN
    GenerateConnectionToken();

    HRESULT
    DisconnectAssociatedQueuePair();
    // Closes the TCP socket. Takes it under the state lock, where
    // CancelOverlappedRequests uses it.
    void
    CloseSocket();

    // The synchronous bodies of the overlapped methods.
    HRESULT
    ConnectWorker(
        IUnknown *pQueuePair,
        const struct sockaddr *pDestAddress,
        ULONG cbDestAddress,
        ULONG inboundReadLimit,
        ULONG outboundReadLimit,
        const void *pPrivateData,
        ULONG cbPrivateData);
    HRESULT
    AcceptWorker(
        IUnknown *pQueuePair,
        ULONG inboundReadLimit,
        ULONG outboundReadLimit,
        const void *pPrivateData,
        ULONG cbPrivateData);
    HRESULT
    DisconnectWorker();

    // Starts the request of a Connect, Accept or Disconnect, and runs its work
    // on the thread pool. Only one runs at a time.
    HRESULT
    StartOperation(
        _In_ OVERLAPPED *overlapped,
        wistd::unique_ptr<NdWork> work);

    // Ends the running operation and completes its request with its result.
    void
    EndOperation(
        _Inout_ OVERLAPPED *overlapped,
        HRESULT result);

    // Waits for a queue pair's binding, which CancelOverlappedRequests cancels,
    // and ends it.
    HRESULT
    WaitForBind(
        _In_ NdQueuePair *queuePair);

    // Marks the connection established and watches for its release, which
    // completes NotifyDisconnect.
    HRESULT
    EstablishConnection(
        _In_ IND2QueuePair *queuePair);

    static
    void CALLBACK
    ReleaseWaitCallback(
        _Inout_ PTP_CALLBACK_INSTANCE instance,
        _Inout_opt_ void *context,
        _Inout_ PTP_WAIT wait,
        TP_WAIT_RESULT waitResult);

    // Ends the connection once both sides released its tunnel. Runs once.
    void
    OnQueuePairReleased();

    // Stops watching for the release, waiting for a running callback.
    void
    StopReleaseWait();

    bool
    ReceiveRejectData();

    // Fails Accept after its application accepted: tells the active side,
    // closes the connection setup socket and returns status.
    HRESULT
    AbortAccept(
        HRESULT status);

    // SendMsg and RecvMsg on the connection socket for Connect and Accept,
    // which record a timeout in m_cmTimedOut.
    bool
    SendCmMessage(
        const void *buffer,
        int length);
    bool
    ReceiveCmMessage(
        void *buffer,
        int length);

    // Waits for the peer's first answer to a connection request, which its
    // application sends by calling Accept or Reject.
    HRESULT
    WaitForPeerApplication();

    Microsoft::WRL::ComPtr<NdDeviceContext> m_deviceContext{};

    // TCP socket for this connection, open only while it is set up.
    wil::unique_socket m_socket{};

    // Bound local address
    struct sockaddr_in m_localAddr = {};
    ULONG m_localAddrLen = 0;

    // Peer address
    struct sockaddr_in m_peerAddr = {};
    ULONG m_peerAddrLen = 0;

    // Peer negotiated parameters
    UINT32 m_peerPrivateDataLength = 0;
    BYTE m_peerPrivateData[NDPROV_MAX_PRIVATE_DATA] = {};
    RDMA_CONNECTION_TOKEN m_connectionToken{};

    // State
    bool m_connected = false;
    bool m_everConnected{ false };
    Microsoft::WRL::ComPtr<IND2QueuePair> m_queuePair{};
    // The pending NotifyDisconnect request. Guarded by m_stateLock.
    OVERLAPPED *m_disconnectRequest{ nullptr };
    // A Connect, Accept or Disconnect is running, and whether
    // CancelOverlappedRequests canceled it. Guarded by m_stateLock.
    bool m_operationInProgress{ false };
    bool m_operationCanceled{ false };
    // Set when CancelOverlappedRequests cancels the running operation, and
    // reset when the next one starts. Guarded by m_stateLock.
    wil::unique_handle m_cancelEvent{};
    // Whether a send or receive of the running Connect or Accept timed out.
    // Used only by that operation's work.
    bool m_cmTimedOut{ false };
    SRWLOCK m_stateLock{};
    // Waits for the queue pair's release event once connected.
    PTP_WAIT m_releaseWait{ nullptr };
    // Whether the release ended the connection, and its NotifyDisconnect
    // status. Guarded by m_stateLock.
    bool m_releaseHandled{ false };
    HRESULT m_disconnectStatus{ ND_PENDING };
    NdOverlappedRequests m_requests{};
};


//
// IND2Listener implementation — listens on TCP for connection requests
//
class NdListener : public ComBase, public IND2Listener
{
public:

    explicit NdListener(
        _In_ NdDeviceContext *deviceContext);
    ~NdListener() override;

    HRESULT
    Initialize(
        HANDLE overlappedFile);

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, LPVOID* ppvObj) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return ComBase::AddRef(); }
    ULONG STDMETHODCALLTYPE Release() override { return ComBase::Release(); }

    // IND2Overlapped
    HRESULT STDMETHODCALLTYPE CancelOverlappedRequests() override;
    HRESULT STDMETHODCALLTYPE GetOverlappedResult(OVERLAPPED* pOverlapped, BOOL wait) override;

    // IND2Listener
    HRESULT STDMETHODCALLTYPE Bind(const struct sockaddr* pAddress, ULONG cbAddress) override;
    HRESULT STDMETHODCALLTYPE Listen(ULONG backlog) override;
    HRESULT STDMETHODCALLTYPE GetLocalAddress(struct sockaddr* pAddress, ULONG* pcbAddress) override;
    HRESULT STDMETHODCALLTYPE GetConnectionRequest(IUnknown* pConnector, OVERLAPPED* pOverlapped) override;

private:

    friend class NdListenerWork;

    // The synchronous body of GetConnectionRequest.
    HRESULT
    GetConnectionRequestWorker(
        IUnknown *pConnector);

    Microsoft::WRL::ComPtr<NdDeviceContext> m_deviceContext{};
    // Closed by CancelOverlappedRequests while workers accept on it. Guarded
    // by m_socketLock, as is m_canceled.
    SRWLOCK m_socketLock{};
    wil::unique_socket m_socket{};
    bool m_canceled{ false };

    struct sockaddr_in m_listenAddr = {};
    ULONG m_listenAddrLen = 0;
    NdOverlappedRequests m_requests{};
};


//
// IND2Adapter implementation
//
class NdAdapter : public ComBase, public IND2Adapter
{
public:
    NdAdapter() = default;
    ~NdAdapter() override;

    static
    HRESULT
    Create(
        UINT64 adapterId,
        _In_z_ const WCHAR *symbolicLink,
        NET_LUID luid,
        _Outptr_ NdAdapter **adapter);

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, LPVOID* ppvObj) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return ComBase::AddRef(); }
    ULONG STDMETHODCALLTYPE Release() override { return ComBase::Release(); }

    // IND2Adapter
    HRESULT STDMETHODCALLTYPE CreateOverlappedFile(HANDLE* phOverlappedFile) override;
    HRESULT STDMETHODCALLTYPE Query(ND2_ADAPTER_INFO* pInfo, ULONG* pcbInfo) override;
    HRESULT STDMETHODCALLTYPE QueryAddressList(SOCKET_ADDRESS_LIST* pAddressList, ULONG* pcbAddressList) override;
    HRESULT STDMETHODCALLTYPE CreateCompletionQueue(REFIID iid, HANDLE hOverlappedFile, ULONG queueDepth,
        USHORT group, KAFFINITY affinity, VOID** ppCompletionQueue) override;
    HRESULT STDMETHODCALLTYPE CreateMemoryRegion(REFIID iid, HANDLE hOverlappedFile, VOID** ppMemoryRegion) override;
    HRESULT STDMETHODCALLTYPE CreateMemoryWindow(REFIID iid, VOID** ppMemoryWindow) override;
    HRESULT STDMETHODCALLTYPE CreateSharedReceiveQueue(REFIID iid, HANDLE hOverlappedFile, ULONG queueDepth,
        ULONG maxRequestSge, ULONG notifyThreshold, USHORT group, KAFFINITY affinity, VOID** ppSharedReceiveQueue) override;
    HRESULT STDMETHODCALLTYPE CreateQueuePair(REFIID iid, IUnknown* pReceiveCompletionQueue,
        IUnknown* pInitiatorCompletionQueue, VOID* context, ULONG receiveQueueDepth, ULONG initiatorQueueDepth,
        ULONG maxReceiveRequestSge, ULONG maxInitiatorRequestSge, ULONG inlineDataSize, VOID** ppQueuePair) override;
    HRESULT STDMETHODCALLTYPE CreateQueuePairWithSrq(REFIID iid, IUnknown* pReceiveCompletionQueue,
        IUnknown* pInitiatorCompletionQueue, IUnknown* pSharedReceiveQueue, VOID* context,
        ULONG initiatorQueueDepth, ULONG maxInitiatorRequestSge, ULONG inlineDataSize, VOID** ppQueuePair) override;
    HRESULT STDMETHODCALLTYPE CreateConnector(REFIID iid, HANDLE hOverlappedFile, VOID** ppConnector) override;
    HRESULT STDMETHODCALLTYPE CreateListener(REFIID iid, HANDLE hOverlappedFile, VOID** ppListener) override;

private:

    HRESULT
    Open(
        UINT64 adapterId,
        _In_z_ const WCHAR *symbolicLink,
        NET_LUID luid);

    Microsoft::WRL::ComPtr<NdDeviceContext> m_deviceContext{};
    // The ID that IND2Provider::ResolveAddress returns for this adapter.
    UINT64 m_adapterId = 0;
    NET_LUID m_luid = {};

    // Cached IP addresses for this adapter (queried from the LUID)
    static const ULONG MAX_ADDRS = 16;
    struct sockaddr_in m_addrs[MAX_ADDRS] = {};
    ULONG m_addrCount = 0;

    HRESULT RefreshAddresses();
};


//
// IND2Provider implementation
//
class NdProvider : public ComBase, public IND2Provider
{
public:

    NdProvider() = default;
    static
    HRESULT
    Create(
        _Outptr_ NdProvider **provider);

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, LPVOID* ppvObj) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return ComBase::AddRef(); }
    ULONG STDMETHODCALLTYPE Release() override { return ComBase::Release(); }

    // IND2Provider
    HRESULT STDMETHODCALLTYPE QueryAddressList(SOCKET_ADDRESS_LIST* pAddressList, ULONG* pcbAddressList) override;
    HRESULT STDMETHODCALLTYPE ResolveAddress(const struct sockaddr* pAddress, ULONG cbAddress, UINT64* pAdapterId) override;
    HRESULT STDMETHODCALLTYPE OpenAdapter(REFIID iid, UINT64 adapterId, VOID** ppAdapter) override;

private:
    HRESULT EnumerateInterfaces();

    static constexpr ULONG c_maxInterfaces{ 16 };
    std::array<wistd::unique_ptr<WCHAR[]>, c_maxInterfaces> m_symbolicLinks{};
    std::array<NET_LUID, c_maxInterfaces> m_luids{};
    ULONG m_interfaceCount{ 0 };
};
