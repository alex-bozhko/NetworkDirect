//
// ndprov.h - NetworkDirect v2 Provider backed by RDMA IOCTLs
//
// Implements IND2 interfaces by forwarding operations to the
// USB4 P2P RDMA kernel driver via IOCTLs defined in RdmaIoctl.h.
//

#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>
#include <unknwn.h>
#include <stdio.h>
#include <stdarg.h>
#include <cfgmgr32.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include "ndspi.h"
#include "RdmaIoctl.h"

// {1F6A4848-5D04-4C77-B96C-4AB28A39449D}
DEFINE_GUID(CLSID_NdDummyProvider,
    0x1f6a4848, 0x5d04, 0x4c77, 0xb9, 0x6c, 0x4a, 0xb2, 0x8a, 0x39, 0x44, 0x9d);

//
// File-based logging (visible even when loaded by system services)
//
inline void LogToFile(const char* fmt, ...)
{
    UNREFERENCED_PARAMETER(fmt);
    /*FILE* f = nullptr;
    fopen_s(&f, "C:\\ndprov_log.txt", "a");
    if (f)
    {
        va_list args;
        va_start(args, fmt);
        vfprintf(f, fmt, args);
        va_end(args);
        fflush(f);
        fclose(f);
    }*/
}

#define LOG_ENTRY(name) LogToFile("[ndprov] %s\n", name)

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
// Only for IOCTLs that complete quickly (create/destroy resources).
// For data-path IOCTLs (send/receive), use overlapped I/O instead.
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
    OVERLAPPED ov = {};
    ov.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    if (ov.hEvent == nullptr) return E_OUTOFMEMORY;

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

    CloseHandle(ov.hEvent);
    if (pcbReturned) *pcbReturned = bytesReturned;
    return hr;
}



//
// IND2CompletionQueue implementation
//
class NdCompletionQueue : public ComBase, public IND2CompletionQueue
{
public:
    NdCompletionQueue(HANDLE hDevice, UINT32 cqId, RDMA_SHARED_COMPLETION_QUEUE* pSharedCq);
    ~NdCompletionQueue() override;

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

private:
    HANDLE m_hDevice;
    UINT32 m_cqId;
    RDMA_SHARED_COMPLETION_QUEUE* m_sharedCq;
};


//
// IND2MemoryRegion implementation
//
class NdMemoryRegion : public ComBase, public IND2MemoryRegion
{
public:
    NdMemoryRegion(HANDLE hDevice) : m_hDevice(hDevice) {}
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
    HANDLE m_hDevice;
    const VOID* m_pBuffer = nullptr;
    SIZE_T m_cbBuffer = 0;
    UINT32 m_mrKey = RDMA_INVALID_ID;
    bool m_registered = false;
};


//
// IND2MemoryWindow implementation
//
class NdMemoryWindow : public ComBase, public IND2MemoryWindow
{
public:
    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, LPVOID* ppvObj) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return ComBase::AddRef(); }
    ULONG STDMETHODCALLTYPE Release() override { return ComBase::Release(); }

    // IND2MemoryWindow
    UINT32 STDMETHODCALLTYPE GetRemoteToken() override;
};


//
// IND2QueuePair implementation
//
class NdQueuePair : public ComBase, public IND2QueuePair
{
public:
    NdQueuePair(HANDLE hDevice, UINT32 qpIndex, VOID* context,
        NdCompletionQueue* pSendCq, NdCompletionQueue* pRecvCq);

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

private:
    HANDLE m_hDevice;
    UINT32 m_qpIndex;
    VOID* m_context;
    NdCompletionQueue* m_pSendCq;
    NdCompletionQueue* m_pRecvCq;
};


//
// ============================================================================
// UDP wire protocol for RDMA connection management
// ============================================================================
//

// Well-known UDP port for RDMA connection management on the P2P link.
#define NDPROV_CM_PORT 23517

// Maximum private data exchanged during connection setup.
#define NDPROV_MAX_PRIVATE_DATA 1024

enum NdCmMessageType : UINT32
{
    ND_CM_CONNECT_REQUEST  = 1,
    ND_CM_CONNECT_ACCEPT   = 2,
    ND_CM_CONNECT_REJECT   = 3,
    ND_CM_DISCONNECT       = 4,
};

// Header common to all CM messages.
struct NdCmHeader
{
    NdCmMessageType Type;
    UINT32 ConnectionId;
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

struct NdCmDisconnect
{
    NdCmHeader Header;
};


//
// IND2Connector implementation — uses TCP for connection management
//
class NdConnector : public ComBase, public IND2Connector
{
public:
    NdConnector(HANDLE hDevice) : m_hDevice(hDevice) {}
    ~NdConnector() override;

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
    void SetAcceptedSocket(
        SOCKET sock,
        const struct sockaddr_in& peerAddr,
        const NdCmConnectRequest& request);

    // Helpers for reliable TCP send/recv of CM messages.
    static bool SendMsg(SOCKET s, const void* buf, int len);
    static bool RecvMsg(SOCKET s, void* buf, int len);

private:

    HANDLE m_hDevice;

    // TCP socket for this connection
    SOCKET m_socket = INVALID_SOCKET;

    // Bound local address
    struct sockaddr_in m_localAddr = {};
    ULONG m_localAddrLen = 0;

    // Peer address
    struct sockaddr_in m_peerAddr = {};
    ULONG m_peerAddrLen = 0;

    // Peer negotiated parameters
    UINT32 m_peerInboundReadLimit = 0;
    UINT32 m_peerOutboundReadLimit = 0;
    UINT32 m_peerPrivateDataLength = 0;
    BYTE m_peerPrivateData[NDPROV_MAX_PRIVATE_DATA] = {};

    // State
    bool m_connected = false;
};


//
// IND2Listener implementation — listens on TCP for connection requests
//
class NdListener : public ComBase, public IND2Listener
{
public:
    NdListener(HANDLE hDevice) : m_hDevice(hDevice) {}
    ~NdListener() override;

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
    HANDLE m_hDevice;
    SOCKET m_socket = INVALID_SOCKET;

    struct sockaddr_in m_listenAddr = {};
    ULONG m_listenAddrLen = 0;
};


//
// IND2Adapter implementation
//
class NdAdapter : public ComBase, public IND2Adapter
{
public:
    NdAdapter() = default;
    ~NdAdapter() override;

    HRESULT Initialize(const WCHAR* pSymLink, NET_LUID luid);

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
    HANDLE m_hDevice = INVALID_HANDLE_VALUE;
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
    NdProvider();

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

    static const ULONG MAX_INTERFACES = 16;
    WCHAR* m_symLinks[MAX_INTERFACES] = {};
    NET_LUID m_luids[MAX_INTERFACES] = {};
    ULONG m_interfaceCount = 0;
};
