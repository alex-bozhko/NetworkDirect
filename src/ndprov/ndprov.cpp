//
// ndprov.cpp - NetworkDirect v2 Provider backed by RDMA IOCTLs
//
// Routes IND2 interface calls to the USB4 P2P RDMA kernel driver
// via IOCTLs defined in RdmaIoctl.h.
//

#include <initguid.h>
#include "ndprov.h"
#include <new>


// ============================================================================
// NdCompletionQueue
// ============================================================================

NdCompletionQueue::NdCompletionQueue(HANDLE hDevice, UINT32 cqId, RDMA_SHARED_COMPLETION_QUEUE* pSharedCq)
    : m_hDevice(hDevice), m_cqId(cqId), m_sharedCq(pSharedCq)
{
}

NdCompletionQueue::~NdCompletionQueue()
{
    if (m_hDevice != INVALID_HANDLE_VALUE && m_cqId != RDMA_INVALID_ID)
    {
        RDMA_DESTROY_COMPLETION_QUEUE_INPUT input = { m_cqId };
        SendIoctl(m_hDevice, IOCTL_RDMA_DESTROY_COMPLETION_QUEUE,
            &input, sizeof(input), nullptr, 0);
        LOG_ENTRY("CQ::~NdCompletionQueue (destroyed)");
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

HRESULT NdCompletionQueue::CancelOverlappedRequests()
{
    LOG_ENTRY("CQ::CancelOverlappedRequests");
    return S_OK;
}

HRESULT NdCompletionQueue::GetOverlappedResult(OVERLAPPED* pOverlapped, BOOL wait)
{
    LOG_ENTRY("CQ::GetOverlappedResult");

    if (pOverlapped == nullptr)
    {
        return E_INVALIDARG;
    }

    DWORD bytesTransferred = 0;
    BOOL ok = ::GetOverlappedResult(m_hDevice, pOverlapped, &bytesTransferred, wait);

    if (!ok)
    {
        DWORD err = GetLastError();
        if (err == ERROR_IO_INCOMPLETE)
        {
            return ND_PENDING;
        }

        return HRESULT_FROM_WIN32(err);
    }

    return ND_SUCCESS;
}

HRESULT NdCompletionQueue::GetNotifyAffinity(USHORT* pGroup, KAFFINITY* pAffinity)
{
    LOG_ENTRY("CQ::GetNotifyAffinity");
    if (pGroup) *pGroup = 0;
    if (pAffinity) *pAffinity = 1;
    return S_OK;
}

HRESULT NdCompletionQueue::Resize(ULONG queueDepth)
{
    LOG_ENTRY("CQ::Resize");
    return S_OK;
}

HRESULT NdCompletionQueue::Notify(ULONG type, OVERLAPPED* pOverlapped)
{
    LOG_ENTRY("CQ::Notify");

    if (pOverlapped == nullptr)
    {
        return E_INVALIDARG;
    }

    // Do a quick check to see if there are new CQEs
    ULONG drain = m_sharedCq->DrainIndex;
    ULONG post = m_sharedCq->PostIndex;
    MemoryBarrier();

    if (drain != post)
    {
        return ND_SUCCESS;
    }

    RDMA_ARM_CQ_INPUT input = { m_cqId };
    BOOL ok = DeviceIoControl(m_hDevice, IOCTL_RDMA_ARM_CQ,
        &input, sizeof(input), nullptr, 0, nullptr, pOverlapped);

    if (ok)
    {
        // Completed immediately — CQ already has entries
        return ND_SUCCESS;
    }

    DWORD err = GetLastError();
    if (err == ERROR_IO_PENDING)
    {
        return ND_PENDING;
    }

    return HRESULT_FROM_WIN32(err);
}

ULONG NdCompletionQueue::GetResults(ND2_RESULT results[], ULONG nResults)
{
    if (nResults == 0 || m_sharedCq == nullptr)
    {
        return 0;
    }

    ULONG count = 0;
    ULONG drain = m_sharedCq->DrainIndex;
    const ULONG post = ReadULongAcquire(&m_sharedCq->PostIndex);

    while (count < nResults && drain != post)
    {
        const RDMA_COMPLETION_ENTRY& entry = m_sharedCq->Entries[drain & m_sharedCq->CapacityMask];

        ND2_REQUEST_TYPE ndType;
        HRESULT ndStatus = S_OK;

        switch (entry.Event)
        {
        case RdmaCompletionEventTypeSend:
        case RdmaCompletionEventTypeSendWithInvalidate:
            ndType = Nd2RequestTypeSend;
            break;
        case RdmaCompletionEventTypeRdmaWrite:
        case RdmaCompletionEventTypeRdmaWriteWithImmediate:
            ndType = Nd2RequestTypeWrite;
            break;
        case RdmaCompletionEventTypeRdmaRead:
            ndType = Nd2RequestTypeRead;
            break;
        case RdmaCompletionEventTypeBindMemoryWindow:
            ndType = Nd2RequestTypeBind;
            break;
        case RdmaCompletionEventTypeLocalInvalidate:
        case RdmaCompletionEventTypeFastRegistration:
            ndType = Nd2RequestTypeInvalidate;
            break;
        case RdmaCompletionEventTypeAtomicCompareAndSwap:
        case RdmaCompletionEventTypeAtomicFetchAndAdd:
            ndType = Nd2RequestTypeRead;
            break;
        case RdmaCompletionEventTypeReceive:
        case RdmaCompletionEventTypeReceiveWithImmediate:
        case RdmaCompletionEventTypeReceiveWithInvalidate:
            ndType = Nd2RequestTypeReceive;
            break;
        default:
            ndType = Nd2RequestTypeSend;
            ndStatus = ND_UNSUCCESSFUL;
            break;
        }

        results[count].Status = ndStatus;
        results[count].BytesTransferred = entry.BytesTransferred;
        results[count].QueuePairContext = reinterpret_cast<VOID*>(entry.QueuePairContext);
        results[count].RequestContext = reinterpret_cast<VOID*>(entry.RequestContext);
        results[count].RequestType = ndType;
        count++;

        drain = (drain + 1) & m_sharedCq->CapacityMask;
    }

    // Batch-update the drain index once after processing all entries.
    if (count > 0)
    {
        MemoryBarrier();
        m_sharedCq->DrainIndex = drain;
    }

    return count;
}



// ============================================================================
// NdMemoryRegion
// ============================================================================

NdMemoryRegion::~NdMemoryRegion()
{
    if (m_registered && m_hDevice != INVALID_HANDLE_VALUE)
    {
        RDMA_DESTROY_MEMORY_REGION_INPUT input = { m_mrKey };
        SendIoctl(m_hDevice, IOCTL_RDMA_DESTROY_MEMORY_REGION,
            &input, sizeof(input), nullptr, 0);
        LOG_ENTRY("MR::~NdMemoryRegion (destroyed)");
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
    LOG_ENTRY("MR::CancelOverlappedRequests");
    return S_OK;
}

HRESULT NdMemoryRegion::GetOverlappedResult(OVERLAPPED* pOverlapped, BOOL wait)
{
    LOG_ENTRY("MR::GetOverlappedResult");
    return S_OK;
}

HRESULT NdMemoryRegion::Register(const VOID* pBuffer, SIZE_T cbBuffer, ULONG flags, OVERLAPPED* pOverlapped)
{
    LOG_ENTRY("MR::Register");

    if (m_registered)
    {
        return ND_DEVICE_BUSY;
    }

    RDMA_CREATE_MEMORY_REGION_INPUT input = {};
    input.UserModeBuffer = reinterpret_cast<UINT64>(pBuffer);
    input.Size = static_cast<UINT64>(cbBuffer);

    RDMA_CREATE_MEMORY_REGION_OUTPUT output = {};
    HRESULT hr = SendIoctl(m_hDevice, IOCTL_RDMA_CREATE_MEMORY_REGION,
        &input, sizeof(input), &output, sizeof(output));
    if (FAILED(hr))
    {
        LogToFile("[ndprov] MR::Register failed: 0x%08X\n", hr);
        return hr;
    }

    m_pBuffer = pBuffer;
    m_cbBuffer = cbBuffer;
    m_mrKey = output.MemoryRegionKey;
    m_registered = true;

    LogToFile("[ndprov] MR::Register OK, key=%u\n", m_mrKey);
    return S_OK;
}

HRESULT NdMemoryRegion::Deregister(OVERLAPPED* pOverlapped)
{
    LOG_ENTRY("MR::Deregister");

    if (m_registered && m_hDevice != INVALID_HANDLE_VALUE)
    {
        RDMA_DESTROY_MEMORY_REGION_INPUT input = { m_mrKey };
        HRESULT hr = SendIoctl(m_hDevice, IOCTL_RDMA_DESTROY_MEMORY_REGION,
            &input, sizeof(input), nullptr, 0);
        if (FAILED(hr))
        {
            LogToFile("[ndprov] MR::Deregister failed: 0x%08X\n", hr);
            return hr;
        }
    }

    m_pBuffer = nullptr;
    m_cbBuffer = 0;
    m_mrKey = RDMA_INVALID_ID;
    m_registered = false;
    return S_OK;
}

UINT32 NdMemoryRegion::GetLocalToken() { return m_mrKey; }
UINT32 NdMemoryRegion::GetRemoteToken() { return m_mrKey; }


// ============================================================================
// NdMemoryWindow
// ============================================================================

HRESULT NdMemoryWindow::QueryInterface(REFIID riid, LPVOID* ppvObj)
{
    if (InlineIsEqualGUID(riid, IID_IUnknown) || InlineIsEqualGUID(riid, IID_IND2MemoryWindow))
    {
        *ppvObj = static_cast<IND2MemoryWindow*>(this);
        AddRef();
        return S_OK;
    }
    *ppvObj = nullptr;
    return E_NOINTERFACE;
}

UINT32 NdMemoryWindow::GetRemoteToken() { return 0; }


// ============================================================================
// NdQueuePair
// ============================================================================

NdQueuePair::NdQueuePair(HANDLE hDevice, UINT32 qpIndex, VOID* context,
    NdCompletionQueue* pSendCq, NdCompletionQueue* pRecvCq)
    : m_hDevice(hDevice), m_qpIndex(qpIndex), m_context(context),
      m_pSendCq(pSendCq), m_pRecvCq(pRecvCq)
{
    if (m_pSendCq) m_pSendCq->AddRef();
    if (m_pRecvCq) m_pRecvCq->AddRef();
}

NdQueuePair::~NdQueuePair()
{
    if (m_hDevice != INVALID_HANDLE_VALUE && m_qpIndex != RDMA_INVALID_ID)
    {
        RDMA_DESTROY_QUEUE_PAIR_INPUT input = { m_qpIndex };
        SendIoctl(m_hDevice, IOCTL_RDMA_DESTROY_QUEUE_PAIR,
            &input, sizeof(input), nullptr, 0);
        LOG_ENTRY("QP::~NdQueuePair (destroyed)");
    }

    if (m_pSendCq) m_pSendCq->Release();
    if (m_pRecvCq) m_pRecvCq->Release();
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
    LOG_ENTRY("QP::Flush");
    return S_OK;
}

HRESULT NdQueuePair::Send(VOID* requestContext, const ND2_SGE sge[], ULONG nSge, ULONG flags)
{
    LOG_ENTRY("QP::Send");

    RDMA_SEND_INPUT input = {};
    input.QueuePairIndex = m_qpIndex;
    input.QueuePairContext = reinterpret_cast<UINT64>(m_context);
    input.RequestContext = reinterpret_cast<UINT64>(requestContext);
    if (nSge > 0)
    {
        input.MemoryRegionKey = sge[0].MemoryRegionToken;
        input.Address = reinterpret_cast<UINT64>(sge[0].Buffer);
        input.Length = sge[0].BufferLength;
    }

    return SendIoctl(m_hDevice, IOCTL_RDMA_SEND,
        &input, sizeof(input), nullptr, 0);
}

HRESULT NdQueuePair::Receive(VOID* requestContext, const ND2_SGE sge[], ULONG nSge)
{
    LOG_ENTRY("QP::Receive");

    if (nSge == 0)
    {
        return E_INVALIDARG;
    }

    RDMA_RECEIVE_INPUT input = {};
    input.QueuePairIndex = m_qpIndex;
    input.MemoryRegionKey = sge[0].MemoryRegionToken;
    input.QueuePairContext = reinterpret_cast<UINT64>(m_context);
    input.RequestContext = reinterpret_cast<UINT64>(requestContext);
    input.Address = reinterpret_cast<UINT64>(sge[0].Buffer);
    input.Length = sge[0].BufferLength;

    return SendIoctl(m_hDevice, IOCTL_RDMA_RECEIVE,
        &input, sizeof(input), nullptr, 0);
}

HRESULT NdQueuePair::Bind(VOID* requestContext, IUnknown* pMemoryRegion, IUnknown* pMemoryWindow,
    const VOID* pBuffer, SIZE_T cbBuffer, ULONG flags)
{
    LOG_ENTRY("QP::Bind");
    return S_OK;
}

HRESULT NdQueuePair::Invalidate(VOID* requestContext, IUnknown* pMemoryWindow, ULONG flags)
{
    LOG_ENTRY("QP::Invalidate");
    return S_OK;
}

HRESULT NdQueuePair::Read(VOID* requestContext, const ND2_SGE sge[], ULONG nSge,
    UINT64 remoteAddress, UINT32 remoteToken, ULONG flags)
{
    LOG_ENTRY("QP::Read");
    return E_NOTIMPL;
}

HRESULT NdQueuePair::Write(VOID* requestContext, const ND2_SGE sge[], ULONG nSge,
    UINT64 remoteAddress, UINT32 remoteToken, ULONG flags)
{
    LOG_ENTRY("QP::Write");

    RDMA_WRITE_INPUT input = {};
    input.QueuePairIndex = m_qpIndex;
    input.QueuePairContext = reinterpret_cast<UINT64>(m_context);
    input.RequestContext = reinterpret_cast<UINT64>(requestContext);
    input.RemoteMemoryRegionKey = remoteToken;
    input.RemoteOffset = remoteAddress;
    if (nSge > 0)
    {
        input.MemoryRegionKey = sge[0].MemoryRegionToken;
        input.Address = reinterpret_cast<UINT64>(sge[0].Buffer);
        input.Length = sge[0].BufferLength;
    }

    return SendIoctl(m_hDevice, IOCTL_RDMA_WRITE,
        &input, sizeof(input), nullptr, 0);
}


// ============================================================================

// ============================================================================
// NdConnector — TCP-based connection management
// ============================================================================

// Reliable TCP send: loops until all bytes are sent.
bool NdConnector::SendMsg(SOCKET s, const void* buf, int len)
{
    const char* p = static_cast<const char*>(buf);
    int remaining = len;
    while (remaining > 0)
    {
        int sent = send(s, p, remaining, 0);
        if (sent == SOCKET_ERROR || sent == 0) return false;
        p += sent;
        remaining -= sent;
    }
    return true;
}

// Reliable TCP recv: loops until all bytes are received.
bool NdConnector::RecvMsg(SOCKET s, void* buf, int len)
{
    char* p = static_cast<char*>(buf);
    int remaining = len;
    while (remaining > 0)
    {
        int received = recv(s, p, remaining, 0);
        if (received == SOCKET_ERROR || received == 0) return false;
        p += received;
        remaining -= received;
    }
    return true;
}

NdConnector::~NdConnector()
{
    if (m_socket != INVALID_SOCKET)
    {
        closesocket(m_socket);
    }
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
    LOG_ENTRY("Conn::CancelOverlappedRequests");
    if (m_socket != INVALID_SOCKET)
    {
        shutdown(m_socket, SD_BOTH);
    }
    return S_OK;
}

HRESULT NdConnector::GetOverlappedResult(OVERLAPPED* pOverlapped, BOOL wait)
{
    LOG_ENTRY("Conn::GetOverlappedResult");
    if (pOverlapped && pOverlapped->hEvent)
    {
        DWORD result = WaitForSingleObject(pOverlapped->hEvent, wait ? INFINITE : 0);
        if (result == WAIT_TIMEOUT) return ND_PENDING;
    }
    return S_OK;
}

HRESULT NdConnector::Bind(const struct sockaddr* pAddress, ULONG cbAddress)
{
    LOG_ENTRY("Conn::Bind");
    if (cbAddress > sizeof(m_localAddr)) return E_INVALIDARG;
    memcpy(&m_localAddr, pAddress, cbAddress);
    m_localAddrLen = cbAddress;
    return S_OK;
}

HRESULT NdConnector::Connect(IUnknown* pQueuePair, const struct sockaddr* pDestAddress, ULONG cbDestAddress,
    ULONG inboundReadLimit, ULONG outboundReadLimit,
    const VOID* pPrivateData, ULONG cbPrivateData, OVERLAPPED* pOverlapped)
{
    LOG_ENTRY("Conn::Connect");

    if (cbDestAddress > sizeof(m_peerAddr)) return E_INVALIDARG;
    memcpy(&m_peerAddr, pDestAddress, cbDestAddress);
    m_peerAddrLen = cbDestAddress;

    // Create TCP socket and connect to peer's CM port
    m_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_socket == INVALID_SOCKET)
    {
        return HRESULT_FROM_WIN32(WSAGetLastError());
    }

    // Bind to local address if provided
    if (m_localAddrLen > 0)
    {
        struct sockaddr_in bindAddr = m_localAddr;
        bindAddr.sin_port = 0;
        bind(m_socket, reinterpret_cast<const struct sockaddr*>(&bindAddr), sizeof(bindAddr));
    }

    struct sockaddr_in destAddr = m_peerAddr;
    destAddr.sin_port = htons(NDPROV_CM_PORT);

    if (connect(m_socket, reinterpret_cast<const struct sockaddr*>(&destAddr), sizeof(destAddr)) == SOCKET_ERROR)
    {
        int err = WSAGetLastError();
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
        return HRESULT_FROM_WIN32(err);
    }

    // Send connect request over the TCP connection
    NdCmConnectRequest req = {};
    req.Header.Type = ND_CM_CONNECT_REQUEST;
    req.Header.ConnectionId = 0;
    req.InboundReadLimit = inboundReadLimit;
    req.OutboundReadLimit = outboundReadLimit;
    req.PrivateDataLength = min(cbPrivateData, NDPROV_MAX_PRIVATE_DATA);
    if (pPrivateData && req.PrivateDataLength > 0)
    {
        memcpy(req.PrivateData, pPrivateData, req.PrivateDataLength);
    }

    int sendSize = static_cast<int>(offsetof(NdCmConnectRequest, PrivateData) + req.PrivateDataLength);
    if (!SendMsg(m_socket, &req, sendSize))
    {
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
        return ND_CONNECTION_REFUSED;
    }

    // Wait for ACCEPT or REJECT response (blocking recv on the TCP socket)
    NdCmHeader respHeader = {};
    if (!RecvMsg(m_socket, &respHeader, sizeof(respHeader)))
    {
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
        return ND_CONNECTION_REFUSED;
    }

    if (respHeader.Type == ND_CM_CONNECT_ACCEPT)
    {
        // Read remaining accept fields
        struct {
            UINT32 InboundReadLimit;
            UINT32 OutboundReadLimit;
            UINT32 PrivateDataLength;
        } acceptFields = {};
        if (!RecvMsg(m_socket, &acceptFields, sizeof(acceptFields)))
        {
            closesocket(m_socket);
            m_socket = INVALID_SOCKET;
            return ND_CONNECTION_REFUSED;
        }

        m_peerInboundReadLimit = acceptFields.InboundReadLimit;
        m_peerOutboundReadLimit = acceptFields.OutboundReadLimit;
        m_peerPrivateDataLength = min(acceptFields.PrivateDataLength, NDPROV_MAX_PRIVATE_DATA);

        if (m_peerPrivateDataLength > 0)
        {
            if (!RecvMsg(m_socket, m_peerPrivateData, m_peerPrivateDataLength))
            {
                closesocket(m_socket);
                m_socket = INVALID_SOCKET;
                return ND_CONNECTION_REFUSED;
            }
        }

        m_connected = true;
    }
    else if (respHeader.Type == ND_CM_CONNECT_REJECT)
    {
        struct { UINT32 PrivateDataLength; } rejectFields = {};
        if (RecvMsg(m_socket, &rejectFields, sizeof(rejectFields)))
        {
            m_peerPrivateDataLength = min(rejectFields.PrivateDataLength, NDPROV_MAX_PRIVATE_DATA);
            if (m_peerPrivateDataLength > 0)
            {
                RecvMsg(m_socket, m_peerPrivateData, m_peerPrivateDataLength);
            }
        }
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
        return ND_CONNECTION_REFUSED;
    }
    else
    {
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
        return ND_CONNECTION_REFUSED;
    }

    // Connection established — signal completion
    if (pOverlapped && pOverlapped->hEvent)
    {
        SetEvent(pOverlapped->hEvent);
    }
    return ND_PENDING;
}

HRESULT NdConnector::CompleteConnect(OVERLAPPED* pOverlapped)
{
    LOG_ENTRY("Conn::CompleteConnect");
    if (!m_connected) return ND_CONNECTION_REFUSED;
    if (pOverlapped && pOverlapped->hEvent) SetEvent(pOverlapped->hEvent);
    return S_OK;
}

HRESULT NdConnector::Accept(IUnknown* pQueuePair, ULONG inboundReadLimit, ULONG outboundReadLimit,
    const VOID* pPrivateData, ULONG cbPrivateData, OVERLAPPED* pOverlapped)
{
    LOG_ENTRY("Conn::Accept");
    if (m_socket == INVALID_SOCKET) return E_FAIL;

    NdCmConnectAccept accept = {};
    accept.Header.Type = ND_CM_CONNECT_ACCEPT;
    accept.Header.ConnectionId = 0;
    accept.InboundReadLimit = inboundReadLimit;
    accept.OutboundReadLimit = outboundReadLimit;
    accept.PrivateDataLength = min(cbPrivateData, NDPROV_MAX_PRIVATE_DATA);
    if (pPrivateData && accept.PrivateDataLength > 0)
    {
        memcpy(accept.PrivateData, pPrivateData, accept.PrivateDataLength);
    }

    int sendSize = static_cast<int>(offsetof(NdCmConnectAccept, PrivateData) + accept.PrivateDataLength);
    if (!SendMsg(m_socket, &accept, sendSize))
    {
        return E_FAIL;
    }

    m_connected = true;
    if (pOverlapped && pOverlapped->hEvent) SetEvent(pOverlapped->hEvent);
    return S_OK;
}

HRESULT NdConnector::Reject(const VOID* pPrivateData, ULONG cbPrivateData)
{
    LOG_ENTRY("Conn::Reject");
    if (m_socket == INVALID_SOCKET) return E_FAIL;

    NdCmConnectReject reject = {};
    reject.Header.Type = ND_CM_CONNECT_REJECT;
    reject.Header.ConnectionId = 0;
    reject.PrivateDataLength = min(cbPrivateData, NDPROV_MAX_PRIVATE_DATA);
    if (pPrivateData && reject.PrivateDataLength > 0)
    {
        memcpy(reject.PrivateData, pPrivateData, reject.PrivateDataLength);
    }

    int sendSize = static_cast<int>(offsetof(NdCmConnectReject, PrivateData) + reject.PrivateDataLength);
    SendMsg(m_socket, &reject, sendSize);
    closesocket(m_socket);
    m_socket = INVALID_SOCKET;
    return S_OK;
}

HRESULT NdConnector::GetReadLimits(ULONG* pInboundReadLimit, ULONG* pOutboundReadLimit)
{
    LOG_ENTRY("Conn::GetReadLimits");
    if (pInboundReadLimit) *pInboundReadLimit = m_peerInboundReadLimit ? m_peerInboundReadLimit : 16;
    if (pOutboundReadLimit) *pOutboundReadLimit = m_peerOutboundReadLimit ? m_peerOutboundReadLimit : 16;
    return S_OK;
}

HRESULT NdConnector::GetPrivateData(VOID* pPrivateData, ULONG* pcbPrivateData)
{
    LOG_ENTRY("Conn::GetPrivateData");
    if (!pcbPrivateData) return E_INVALIDARG;
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
    LOG_ENTRY("Conn::GetLocalAddress");
    if (m_localAddrLen == 0) return E_NOTIMPL;
    if (*pcbAddress < m_localAddrLen) { *pcbAddress = m_localAddrLen; return ND_BUFFER_OVERFLOW; }
    memcpy(pAddress, &m_localAddr, m_localAddrLen);
    *pcbAddress = m_localAddrLen;
    return S_OK;
}

HRESULT NdConnector::GetPeerAddress(struct sockaddr* pAddress, ULONG* pcbAddress)
{
    LOG_ENTRY("Conn::GetPeerAddress");
    if (m_peerAddrLen == 0) return E_NOTIMPL;
    if (*pcbAddress < m_peerAddrLen) { *pcbAddress = m_peerAddrLen; return ND_BUFFER_OVERFLOW; }
    memcpy(pAddress, &m_peerAddr, m_peerAddrLen);
    *pcbAddress = m_peerAddrLen;
    return S_OK;
}

HRESULT NdConnector::NotifyDisconnect(OVERLAPPED* pOverlapped)
{
    LOG_ENTRY("Conn::NotifyDisconnect");
    return ND_PENDING;
}

HRESULT NdConnector::Disconnect(OVERLAPPED* pOverlapped)
{
    LOG_ENTRY("Conn::Disconnect");
    if (m_socket != INVALID_SOCKET)
    {
        NdCmDisconnect disc = {};
        disc.Header.Type = ND_CM_DISCONNECT;
        disc.Header.ConnectionId = 0;
        SendMsg(m_socket, &disc, sizeof(disc));
        shutdown(m_socket, SD_BOTH);
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
    }
    m_connected = false;
    if (pOverlapped && pOverlapped->hEvent) SetEvent(pOverlapped->hEvent);
    return S_OK;
}

void NdConnector::SetAcceptedSocket(
    SOCKET sock,
    const struct sockaddr_in& peerAddr,
    const NdCmConnectRequest& request)
{
    m_socket = sock;
    m_peerAddr = peerAddr;
    m_peerAddrLen = sizeof(peerAddr);
    m_peerInboundReadLimit = request.InboundReadLimit;
    m_peerOutboundReadLimit = request.OutboundReadLimit;
    m_peerPrivateDataLength = min(request.PrivateDataLength, NDPROV_MAX_PRIVATE_DATA);
    if (m_peerPrivateDataLength > 0)
    {
        memcpy(m_peerPrivateData, request.PrivateData, m_peerPrivateDataLength);
    }
}


// ============================================================================
// NdListener — TCP-based listener
// ============================================================================

NdListener::~NdListener()
{
    if (m_socket != INVALID_SOCKET)
    {
        closesocket(m_socket);
    }
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
    LOG_ENTRY("Listener::CancelOverlappedRequests");
    if (m_socket != INVALID_SOCKET) { closesocket(m_socket); m_socket = INVALID_SOCKET; }
    return S_OK;
}

HRESULT NdListener::GetOverlappedResult(OVERLAPPED* pOverlapped, BOOL wait)
{
    LOG_ENTRY("Listener::GetOverlappedResult");
    if (pOverlapped && pOverlapped->hEvent)
    {
        DWORD result = WaitForSingleObject(pOverlapped->hEvent, wait ? INFINITE : 0);
        if (result == WAIT_TIMEOUT) return ND_PENDING;
    }
    return S_OK;
}

HRESULT NdListener::Bind(const struct sockaddr* pAddress, ULONG cbAddress)
{
    LOG_ENTRY("Listener::Bind");
    if (cbAddress > sizeof(m_listenAddr)) return E_INVALIDARG;
    memcpy(&m_listenAddr, pAddress, cbAddress);
    m_listenAddrLen = cbAddress;
    return S_OK;
}

HRESULT NdListener::Listen(ULONG backlog)
{
    LOG_ENTRY("Listener::Listen");

    m_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_socket == INVALID_SOCKET) return HRESULT_FROM_WIN32(WSAGetLastError());

    BOOL reuseAddr = TRUE;
    setsockopt(m_socket, SOL_SOCKET, SO_REUSEADDR,
        reinterpret_cast<const char*>(&reuseAddr), sizeof(reuseAddr));

    struct sockaddr_in bindAddr = m_listenAddr;
    bindAddr.sin_port = htons(NDPROV_CM_PORT);

    if (bind(m_socket, reinterpret_cast<const struct sockaddr*>(&bindAddr), sizeof(bindAddr)) == SOCKET_ERROR)
    {
        int err = WSAGetLastError();
        closesocket(m_socket); m_socket = INVALID_SOCKET;
        return HRESULT_FROM_WIN32(err);
    }

    if (listen(m_socket, backlog > 0 ? backlog : SOMAXCONN) == SOCKET_ERROR)
    {
        int err = WSAGetLastError();
        closesocket(m_socket); m_socket = INVALID_SOCKET;
        return HRESULT_FROM_WIN32(err);
    }

    return S_OK;
}

HRESULT NdListener::GetLocalAddress(struct sockaddr* pAddress, ULONG* pcbAddress)
{
    LOG_ENTRY("Listener::GetLocalAddress");
    if (m_listenAddrLen == 0) return E_NOTIMPL;
    if (*pcbAddress < m_listenAddrLen) { *pcbAddress = m_listenAddrLen; return ND_BUFFER_OVERFLOW; }
    memcpy(pAddress, &m_listenAddr, m_listenAddrLen);
    *pcbAddress = m_listenAddrLen;
    return S_OK;
}

HRESULT NdListener::GetConnectionRequest(IUnknown* pConnector, OVERLAPPED* pOverlapped)
{
    LOG_ENTRY("Listener::GetConnectionRequest");
    if (m_socket == INVALID_SOCKET) return E_FAIL;

    // Accept a TCP connection (blocks until a client connects)
    struct sockaddr_in peerAddr = {};
    int peerAddrLen = sizeof(peerAddr);
    SOCKET acceptedSock = accept(m_socket,
        reinterpret_cast<struct sockaddr*>(&peerAddr), &peerAddrLen);
    if (acceptedSock == INVALID_SOCKET)
    {
        int err = WSAGetLastError();
        if (err == WSAEINTR || err == WSAENOTSOCK) return ND_CANCELED;
        return HRESULT_FROM_WIN32(err);
    }

    // Read the connect request header + fixed fields from the TCP stream
    NdCmConnectRequest request = {};
    int fixedSize = static_cast<int>(offsetof(NdCmConnectRequest, PrivateData));
    if (!NdConnector::RecvMsg(acceptedSock, &request, fixedSize))
    {
        closesocket(acceptedSock);
        return E_FAIL;
    }

    if (request.Header.Type != ND_CM_CONNECT_REQUEST)
    {
        closesocket(acceptedSock);
        return E_FAIL;
    }

    // Read private data if present
    UINT32 privLen = min(request.PrivateDataLength, NDPROV_MAX_PRIVATE_DATA);
    if (privLen > 0)
    {
        if (!NdConnector::RecvMsg(acceptedSock, request.PrivateData, privLen))
        {
            closesocket(acceptedSock);
            return E_FAIL;
        }
    }

    // Hand the accepted socket and request data to the connector
    IND2Connector* iConn = nullptr;
    HRESULT hr = pConnector->QueryInterface(IID_IND2Connector, reinterpret_cast<void**>(&iConn));
    if (SUCCEEDED(hr) && iConn)
    {
        NdConnector* pConn = static_cast<NdConnector*>(iConn);
        pConn->SetAcceptedSocket(acceptedSock, peerAddr, request);
        iConn->Release();
    }
    else
    {
        closesocket(acceptedSock);
        return E_FAIL;
    }

    if (pOverlapped && pOverlapped->hEvent) SetEvent(pOverlapped->hEvent);
    return S_OK;
}

// NdAdapter
// ============================================================================

NdAdapter::~NdAdapter()
{
    if (m_hDevice != INVALID_HANDLE_VALUE)
    {
        CloseHandle(m_hDevice);
        LOG_ENTRY("Adapter::~NdAdapter (closed device)");
    }
}

HRESULT NdAdapter::Initialize(const WCHAR* pSymLink, NET_LUID luid)
{
    m_luid = luid;

    m_hDevice = CreateFileW(
        pSymLink,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED,
        nullptr);

    if (m_hDevice == INVALID_HANDLE_VALUE)
    {
        HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
        LogToFile("[ndprov] Adapter::Initialize CreateFile failed: 0x%08X\n", hr);
        return hr;
    }

    RefreshAddresses();

    LogToFile("[ndprov] Adapter::Initialize OK, handle=%p, LUID=0x%llX, addrs=%u\n",
        m_hDevice, m_luid.Value, m_addrCount);
    return S_OK;
}

HRESULT NdAdapter::RefreshAddresses()
{
    m_addrCount = 0;

    PMIB_UNICASTIPADDRESS_TABLE table = nullptr;
    DWORD err = GetUnicastIpAddressTable(AF_INET, &table);
    if (err != NO_ERROR)
    {
        return HRESULT_FROM_WIN32(err);
    }

    for (ULONG t = 0; t < table->NumEntries && m_addrCount < MAX_ADDRS; t++)
    {
        const MIB_UNICASTIPADDRESS_ROW& row = table->Table[t];
        if (row.InterfaceLuid.Value == m_luid.Value)
        {
            memset(&m_addrs[m_addrCount], 0, sizeof(struct sockaddr_in));
            m_addrs[m_addrCount].sin_family = AF_INET;
            m_addrs[m_addrCount].sin_addr = row.Address.Ipv4.sin_addr;
            m_addrCount++;
        }
    }

    FreeMibTable(table);
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
    LOG_ENTRY("Adapter::CreateOverlappedFile");
    // Return a dummy event handle for overlapped operations.
    // Real IOCP integration would duplicate the device handle with FILE_FLAG_OVERLAPPED.
    *phOverlappedFile = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    return (*phOverlappedFile != nullptr) ? S_OK : E_OUTOFMEMORY;
}

HRESULT NdAdapter::Query(ND2_ADAPTER_INFO* pInfo, ULONG* pcbInfo)
{
    LOG_ENTRY("Adapter::Query");
    if (*pcbInfo < sizeof(ND2_ADAPTER_INFO))
    {
        *pcbInfo = sizeof(ND2_ADAPTER_INFO);
        return ND_BUFFER_OVERFLOW;
    }

    memset(pInfo, 0, sizeof(*pInfo));
    pInfo->InfoVersion = ND_VERSION_2;
    pInfo->VendorId = 0xDEAD;
    pInfo->DeviceId = 0xBEEF;
    pInfo->MaxRegistrationSize = 1ULL << 30;    // 1 GB
    pInfo->MaxWindowSize = 1ULL << 30;
    pInfo->MaxInitiatorSge = 8;
    pInfo->MaxReceiveSge = 8;
    pInfo->MaxReadSge = 8;
    pInfo->MaxTransferLength = 1 << 20;         // 1 MB
    pInfo->MaxInlineDataSize = 128;
    pInfo->MaxInboundReadLimit = 16;
    pInfo->MaxOutboundReadLimit = 16;
    pInfo->MaxReceiveQueueDepth = 1024;
    pInfo->MaxInitiatorQueueDepth = 1024;
    pInfo->MaxSharedReceiveQueueDepth = 1024;
    pInfo->MaxCompletionQueueDepth = 4096;
    pInfo->InlineRequestThreshold = 128;
    pInfo->LargeRequestThreshold = 4096;
    pInfo->MaxCallerData = 256;
    pInfo->MaxCalleeData = 256;
    pInfo->AdapterFlags = ND_ADAPTER_FLAG_IN_ORDER_DMA_SUPPORTED;

    return S_OK;
}

HRESULT NdAdapter::QueryAddressList(SOCKET_ADDRESS_LIST* pAddressList, ULONG* pcbAddressList)
{
    LOG_ENTRY("Adapter::QueryAddressList");

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
    LOG_ENTRY("Adapter::CreateCompletionQueue");

    RDMA_CREATE_COMPLETION_QUEUE_INPUT input = { 4096 };
    RDMA_CREATE_COMPLETION_QUEUE_OUTPUT output = {};
    HRESULT hr = SendIoctl(m_hDevice, IOCTL_RDMA_CREATE_COMPLETION_QUEUE,
        &input, sizeof(input), &output, sizeof(output));
    if (FAILED(hr))
    {
        LogToFile("[ndprov] Adapter::CreateCQ IOCTL failed: 0x%08X\n", hr);
        return hr;
    }

    auto* pSharedCq = reinterpret_cast<RDMA_SHARED_COMPLETION_QUEUE*>(output.SharedBufferAddress);
    if (pSharedCq == nullptr)
    {
        LogToFile("[ndprov] Adapter::CreateCQ shared buffer is NULL\n");
        return E_UNEXPECTED;
    }

    NdCompletionQueue* pCq = new (std::nothrow) NdCompletionQueue(m_hDevice, output.CompletionQueueId, pSharedCq);
    if (pCq == nullptr) return E_OUTOFMEMORY;

    LogToFile("[ndprov] Adapter::CreateCQ OK, id=%u, shared=%p, cap=%u\n",
        output.CompletionQueueId, pSharedCq, pSharedCq->Capacity);
    *ppCompletionQueue = static_cast<IND2CompletionQueue*>(pCq);
    return S_OK;
}

HRESULT NdAdapter::CreateMemoryRegion(REFIID iid, HANDLE hOverlappedFile, VOID** ppMemoryRegion)
{
    LOG_ENTRY("Adapter::CreateMemoryRegion");
    NdMemoryRegion* pMr = new (std::nothrow) NdMemoryRegion(m_hDevice);
    if (pMr == nullptr) return E_OUTOFMEMORY;
    *ppMemoryRegion = static_cast<IND2MemoryRegion*>(pMr);
    return S_OK;
}

HRESULT NdAdapter::CreateMemoryWindow(REFIID iid, VOID** ppMemoryWindow)
{
    LOG_ENTRY("Adapter::CreateMemoryWindow");
    NdMemoryWindow* pMw = new (std::nothrow) NdMemoryWindow();
    if (pMw == nullptr) return E_OUTOFMEMORY;
    *ppMemoryWindow = static_cast<IND2MemoryWindow*>(pMw);
    return S_OK;
}

HRESULT NdAdapter::CreateSharedReceiveQueue(REFIID iid, HANDLE hOverlappedFile, ULONG queueDepth,
    ULONG maxRequestSge, ULONG notifyThreshold, USHORT group, KAFFINITY affinity, VOID** ppSharedReceiveQueue)
{
    LOG_ENTRY("Adapter::CreateSharedReceiveQueue");
    return E_NOTIMPL;
}

HRESULT NdAdapter::CreateQueuePair(REFIID iid, IUnknown* pReceiveCompletionQueue,
    IUnknown* pInitiatorCompletionQueue, VOID* context, ULONG receiveQueueDepth, ULONG initiatorQueueDepth,
    ULONG maxReceiveRequestSge, ULONG maxInitiatorRequestSge, ULONG inlineDataSize, VOID** ppQueuePair)
{
    LOG_ENTRY("Adapter::CreateQueuePair");

    // Build the IOCTL input with optional CQ IDs
    RDMA_CREATE_QUEUE_PAIR_INPUT input = {};
    input.TxCompletionQueueId = RDMA_INVALID_ID;
    input.RxCompletionQueueId = RDMA_INVALID_ID;

    NdCompletionQueue* pSendCqObj = nullptr;
    NdCompletionQueue* pRecvCqObj = nullptr;

    if (pInitiatorCompletionQueue)
    {
        IND2CompletionQueue* pInitCq = nullptr;
        if (SUCCEEDED(pInitiatorCompletionQueue->QueryInterface(IID_IND2CompletionQueue, reinterpret_cast<void**>(&pInitCq))))
        {
            pSendCqObj = static_cast<NdCompletionQueue*>(pInitCq);
            input.TxCompletionQueueId = pSendCqObj->GetId();
            // QI AddRef'd; NdQueuePair constructor will AddRef again, so we Release after construction
        }
    }

    if (pReceiveCompletionQueue)
    {
        IND2CompletionQueue* pRecvCq = nullptr;
        if (SUCCEEDED(pReceiveCompletionQueue->QueryInterface(IID_IND2CompletionQueue, reinterpret_cast<void**>(&pRecvCq))))
        {
            pRecvCqObj = static_cast<NdCompletionQueue*>(pRecvCq);
            input.RxCompletionQueueId = pRecvCqObj->GetId();
        }
    }

    RDMA_CREATE_QUEUE_PAIR_OUTPUT output = {};
    HRESULT hr = SendIoctl(m_hDevice, IOCTL_RDMA_CREATE_QUEUE_PAIR,
        &input, sizeof(input), &output, sizeof(output));
    if (FAILED(hr))
    {
        LogToFile("[ndprov] Adapter::CreateQP IOCTL failed: 0x%08X\n", hr);
        return hr;
    }

    NdQueuePair* pQp = new (std::nothrow) NdQueuePair(m_hDevice, output.QueuePairIndex, context,
        pSendCqObj, pRecvCqObj);

    // Release the QI refs; NdQueuePair constructor AddRef'd the CQs it needs
    if (pSendCqObj) pSendCqObj->Release();
    if (pRecvCqObj) pRecvCqObj->Release();

    if (pQp == nullptr) return E_OUTOFMEMORY;

    LogToFile("[ndprov] Adapter::CreateQP OK, index=%u\n", output.QueuePairIndex);
    *ppQueuePair = static_cast<IND2QueuePair*>(pQp);
    return S_OK;
}

HRESULT NdAdapter::CreateQueuePairWithSrq(REFIID iid, IUnknown* pReceiveCompletionQueue,
    IUnknown* pInitiatorCompletionQueue, IUnknown* pSharedReceiveQueue, VOID* context,
    ULONG initiatorQueueDepth, ULONG maxInitiatorRequestSge, ULONG inlineDataSize, VOID** ppQueuePair)
{
    LOG_ENTRY("Adapter::CreateQueuePairWithSrq");
    return E_NOTIMPL;
}

HRESULT NdAdapter::CreateConnector(REFIID iid, HANDLE hOverlappedFile, VOID** ppConnector)
{
    LOG_ENTRY("Adapter::CreateConnector");
    NdConnector* pConn = new (std::nothrow) NdConnector(m_hDevice);
    if (pConn == nullptr) return E_OUTOFMEMORY;
    *ppConnector = static_cast<IND2Connector*>(pConn);
    return S_OK;
}

HRESULT NdAdapter::CreateListener(REFIID iid, HANDLE hOverlappedFile, VOID** ppListener)
{
    LOG_ENTRY("Adapter::CreateListener");
    NdListener* pListen = new (std::nothrow) NdListener(m_hDevice);
    if (pListen == nullptr) return E_OUTOFMEMORY;
    *ppListener = static_cast<IND2Listener*>(pListen);
    return S_OK;
}


// ============================================================================
// NdProvider
// ============================================================================

NdProvider::NdProvider()
{
    EnumerateInterfaces();
}

HRESULT NdProvider::EnumerateInterfaces()
{
    ULONG bufferLen = 0;
    CONFIGRET cr = CM_Get_Device_Interface_List_SizeW(
        &bufferLen, const_cast<GUID*>(&GUID_DEVINTERFACE_RDMA),
        nullptr, CM_GET_DEVICE_INTERFACE_LIST_PRESENT);

    if (cr != CR_SUCCESS || bufferLen <= 1)
    {
        LogToFile("[ndprov] No RDMA device interfaces found (cr=%u, len=%u)\n", cr, bufferLen);
        m_interfaceCount = 0;
        return S_OK;
    }

    WCHAR* buffer = new (std::nothrow) WCHAR[bufferLen];
    if (buffer == nullptr) return E_OUTOFMEMORY;

    cr = CM_Get_Device_Interface_ListW(
        const_cast<GUID*>(&GUID_DEVINTERFACE_RDMA),
        nullptr, buffer, bufferLen, CM_GET_DEVICE_INTERFACE_LIST_PRESENT);

    if (cr != CR_SUCCESS)
    {
        delete[] buffer;
        LogToFile("[ndprov] CM_Get_Device_Interface_List failed: %u\n", cr);
        return HRESULT_FROM_WIN32(CM_MapCrToWin32Err(cr, ERROR_GEN_FAILURE));
    }

    // Parse the multi-sz buffer
    m_interfaceCount = 0;
    WCHAR* p = buffer;
    while (*p != L'\0' && m_interfaceCount < MAX_INTERFACES)
    {
        size_t len = wcslen(p);
        m_symLinks[m_interfaceCount] = new (std::nothrow) WCHAR[len + 1];
        if (m_symLinks[m_interfaceCount])
        {
            wcscpy_s(m_symLinks[m_interfaceCount], len + 1, p);

            // Read the NET_LUID from the device interface property
            NET_LUID luid = {};
            DEVPROPTYPE propType = 0;
            ULONG propSize = sizeof(luid);
            CONFIGRET propCr = CM_Get_Device_Interface_PropertyW(
                p,
                &DEVPKEY_RdmaAdapter_Luid,
                &propType,
                reinterpret_cast<PBYTE>(&luid),
                &propSize,
                0);

            if (propCr == CR_SUCCESS && propType == DEVPROP_TYPE_UINT64)
            {
                m_luids[m_interfaceCount] = luid;
                LogToFile("[ndprov] Interface[%u] LUID=0x%llX\n",
                    m_interfaceCount, luid.Value);
            }
            else
            {
                LogToFile("[ndprov] Interface[%u] LUID query failed (cr=%u)\n",
                    m_interfaceCount, propCr);
            }

            LogToFile("[ndprov] Found interface[%u]: %ls\n", m_interfaceCount, p);
            m_interfaceCount++;
        }
        p += len + 1;
    }

    delete[] buffer;
    LogToFile("[ndprov] Enumerated %u RDMA interfaces\n", m_interfaceCount);
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
    LOG_ENTRY("Provider::QueryAddressList");

    // Gather all IPv4 unicast addresses across all our adapters
    PMIB_UNICASTIPADDRESS_TABLE table = nullptr;
    DWORD err = GetUnicastIpAddressTable(AF_INET, &table);
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

    for (ULONG t = 0; t < table->NumEntries && matchCount < 64; t++)
    {
        const MIB_UNICASTIPADDRESS_ROW& row = table->Table[t];
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

    FreeMibTable(table);

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
    LOG_ENTRY("Provider::ResolveAddress");

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
    PMIB_UNICASTIPADDRESS_TABLE table = nullptr;
    DWORD err = GetUnicastIpAddressTable(AF_INET, &table);
    if (err != NO_ERROR)
    {
        return HRESULT_FROM_WIN32(err);
    }

    HRESULT hr = ND_INVALID_ADDRESS;
    for (ULONG t = 0; t < table->NumEntries; t++)
    {
        const MIB_UNICASTIPADDRESS_ROW& row = table->Table[t];
        if (row.Address.Ipv4.sin_addr.s_addr != pAddr->sin_addr.s_addr)
        {
            continue;
        }

        for (ULONG i = 0; i < m_interfaceCount; i++)
        {
            if (row.InterfaceLuid.Value == m_luids[i].Value)
            {
                *pAdapterId = static_cast<UINT64>(i + 1);
                LogToFile("[ndprov] Provider::ResolveAddress %08X -> adapterId=%llu\n",
                    ntohl(pAddr->sin_addr.s_addr), *pAdapterId);
                hr = S_OK;
                goto done;
            }
        }
    }

done:
    FreeMibTable(table);
    return hr;
}

HRESULT NdProvider::OpenAdapter(REFIID iid, UINT64 adapterId, VOID** ppAdapter)
{
    LOG_ENTRY("Provider::OpenAdapter");

    if (adapterId == 0 || adapterId > m_interfaceCount)
    {
        LogToFile("[ndprov] Provider::OpenAdapter - invalid adapterId %llu\n", adapterId);
        return ND_INVALID_ADDRESS;
    }

    ULONG index = static_cast<ULONG>(adapterId - 1);

    NdAdapter* pAdapter = new (std::nothrow) NdAdapter();
    if (pAdapter == nullptr) return E_OUTOFMEMORY;

    HRESULT hr = pAdapter->Initialize(m_symLinks[index], m_luids[index]);
    if (FAILED(hr))
    {
        pAdapter->Release();
        return hr;
    }

    *ppAdapter = static_cast<IND2Adapter*>(pAdapter);
    LogToFile("[ndprov] Provider::OpenAdapter OK for interface[%u]\n", index);
    return S_OK;
}
