// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#define NOMINMAX

#include <winsock2.h>
#include <ws2spi.h>
#include <ws2tcpip.h>

#include <initguid.h>
#include "ndspi.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cwchar>
#include <tuple>
#include <utility>
#include <vector>

#include <wil/com.h>
#include <wil/resource.h>

static constexpr USHORT c_defaultPort = 54326;
static constexpr ULONG c_defaultMessageSize = 65536;
static constexpr ULONG c_maxMessageSize = 4 * 1024 * 1024;
static constexpr ULONG c_defaultDepth = 16;
static constexpr ULONG c_maxDepth = 256;
static constexpr ULONG c_defaultSeconds = 5;
static constexpr ULONG c_defaultWarmupSeconds = 1;
static constexpr ULONG c_defaultWarmupMessages = 0;
static constexpr ULONG c_defaultQueuePairs = 1;
static constexpr ULONG c_maxQueuePairs = 3;
static constexpr ULONG c_maxStatisticsUpdateSeconds = 3600;
static constexpr ULONG c_receiveMargin = 4;
static constexpr ULONG c_maxCompletionQueueDepth = 4096;
static constexpr ULONG c_resultBatchSize = 64;
static constexpr ULONG c_idlePollsPerDisconnectCheck = 1024;
static constexpr DWORD c_blockingWaitMilliseconds = 100;
static constexpr DWORD c_secondaryAcceptTimeoutMilliseconds = 10000;
static constexpr DWORD c_serverWorkloadCooldownMilliseconds = 1000;
static constexpr DWORD c_millisecondsPerSecond = 1000;
static constexpr ULONGLONG c_maxDataBufferBytes = 1024ULL * 1024 * 1024;
static constexpr size_t c_pageSize = 4096;
static constexpr size_t c_controlBufferSize = 2 * c_pageSize;
static constexpr ULONG c_controlMagic = 0x5450444E;
static constexpr ULONG c_controlVersion = 1;
static constexpr ULONG c_dataMagic = 0x41544144;
static constexpr double c_bytesPerMegabyte = 1000000.0;
static constexpr double c_bitsPerByte = 8.0;
static constexpr double c_bitsPerGigabit = 1000000000.0;
static constexpr double c_cpuTicksPerSecond = 10000000.0;
static constexpr double c_percent = 100.0;

static constexpr
size_t
AlignUp(
    size_t value,
    size_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

enum class ControlType : ULONG
{
    Workload = 1,
    WorkloadReady = 2,
};

// Kind of a data-plane message, stored in the DataHeader at the start of every payload.
enum class DataKind : ULONG
{
    Warmup = 1,   // Streamed before the measurement window; excluded from results.
    Measured = 2, // Streamed inside the measurement window.
    Last = 3,     // Terminates a stream; carries the next sequence number and no measured data.
    Ack = 4,      // Sent by a receiver after Last; echoes the sequence number of Last.
};

enum class RequestKind : ULONG
{
    ControlSend = 1,
    ControlReceive = 2,
    DataSend = 3,
    DataReceive = 4,
};

enum class NotifyState
{
    Armed,
    Ready,
    Failed,
};

struct ControlHeader
{
    ULONG magic = c_controlMagic;
    ULONG version = c_controlVersion;
    ControlType type = ControlType::Workload;
    HRESULT status = S_OK;
};

static constexpr ULONG c_workloadFlagBidirectional = 0x1;
static constexpr ULONG c_workloadFlagBlocking = 0x2;
static constexpr ULONG c_workloadFlagRealtime = 0x4;
static constexpr ULONG c_workloadFlagThreadPerQueuePair = 0x8;
static constexpr ULONG c_workloadFlagIterationMode = 0x10;
static constexpr ULONG c_workloadFlagCompletionPort = 0x20;
static constexpr ULONG c_validWorkloadFlags = c_workloadFlagBidirectional | c_workloadFlagBlocking | c_workloadFlagRealtime |
                                              c_workloadFlagThreadPerQueuePair | c_workloadFlagIterationMode |
                                              c_workloadFlagCompletionPort;

struct WorkloadParameters
{
    ULONG messageSize = 0;
    ULONG depth = 0;
    ULONG queuePairs = 0;
    ULONG seconds = 0;
    ULONG iterations = 0;
    ULONG warmup = 0;
    ULONG statisticsUpdateSeconds = 0;
    ULONG flags = 0;
};

struct ControlMessage
{
    ControlHeader header{};
    WorkloadParameters parameters{};
};

static constexpr ULONG c_controlMessageLength = static_cast<ULONG>(sizeof(ControlMessage));

struct DataHeader
{
    ULONG magic = c_dataMagic;
    DataKind kind = DataKind::Warmup;
    ULONGLONG sequence = 0;
};

static constexpr ULONG c_minMessageSize = static_cast<ULONG>(sizeof(DataHeader));

struct RequestContext
{
    ULONG slot = 0;
    RequestKind kind = RequestKind::DataSend;
};

struct Options
{
    bool server = false;
    bool client = false;
    bool bidirectional = false;
    bool blocking = false;
    bool realtime = false;
    bool threadPerQueuePair = false;
    bool iterationMode = false;
    bool completionPort = false;
    bool oneShot = false;
    ULONG messageSize = c_defaultMessageSize;
    ULONG depth = c_defaultDepth;
    ULONG queuePairs = c_defaultQueuePairs;
    ULONG seconds = c_defaultSeconds;
    ULONG iterations = 0;
    ULONG warmup = c_defaultWarmupSeconds;
    ULONG statisticsUpdateSeconds = 0;
    sockaddr_in address{};
};

// Per-side sizing of one queue pair. Every send and receive slot is a page-aligned,
// page-multiple region of the data buffer so a send of messageSize bytes always lands in
// a receive of exactly messageSize bytes.
struct FlowLayout
{
    bool transmit = false;
    bool receive = false;
    ULONG messageSize = 0;
    ULONG depth = 0;
    ULONG sendSlots = 0;
    ULONG receiveSlots = 0;
    size_t stride = 0;
};

struct RunSchedule
{
    bool iterationMode = false;
    ULONGLONG warmupMessages = 0;
    ULONGLONG measuredMessages = 0;
    LONGLONG warmupEndTicks = 0;
    LONGLONG measureEndTicks = 0;
};

struct DirectionStatistics
{
    bool started = false;
    ULONGLONG messages = 0;
    ULONGLONG bytes = 0;
    LONGLONG startTicks = 0;
    LONGLONG endTicks = 0;
};

struct CompletionStatistics
{
    ULONGLONG getResultsCalls = 0;
    ULONGLONG nonEmptyGetResultsCalls = 0;
    ULONGLONG completions = 0;
    ULONGLONG notifyCalls = 0;
    ULONGLONG notifyImmediate = 0;
    ULONGLONG notifyPending = 0;
};

struct Rates
{
    double seconds = 0.0;
    double megabytesPerSecond = 0.0;
    double gigabitsPerSecond = 0.0;
    double messagesPerSecond = 0.0;
};

struct CpuTimes
{
    ULONGLONG user = 0;
    ULONGLONG kernel = 0;
};

static
FlowLayout
MakeFlowLayout(
    const Options &options,
    bool client)
{
    FlowLayout layout{};
    layout.transmit = client || options.bidirectional;
    layout.receive = !client || options.bidirectional;
    layout.messageSize = options.messageSize;
    layout.depth = options.depth;

    // A receiver needs one extra send slot for its Ack; a transmitter needs one extra
    // receive slot for the peer's Ack.
    layout.sendSlots = (layout.transmit ? options.depth : 0) + (layout.receive ? 1 : 0);
    layout.receiveSlots = (layout.receive ? options.depth + c_receiveMargin : 0) + (layout.transmit ? 1 : 0);
    layout.stride = AlignUp(
        options.messageSize,
        c_pageSize);
    return layout;
}

static
ULONGLONG
DataBufferBytes(
    const FlowLayout &layout)
{
    return static_cast<ULONGLONG>(layout.stride) * (static_cast<ULONGLONG>(layout.sendSlots) + layout.receiveSlots);
}

// Sized for every request that can be outstanding, and at least twice the send depth.
static
ULONG
CompletionDepth(
    const FlowLayout &layout)
{
    return std::max(
        layout.sendSlots + layout.receiveSlots,
        2 * layout.depth);
}

static
_Ret_maybenull_z_ const char *
ValidateWorkload(
    const Options &options)
{
    if (options.messageSize < c_minMessageSize || options.messageSize > c_maxMessageSize)
    {
        return "message size is out of range";
    }

    if (options.depth == 0 || options.depth > c_maxDepth)
    {
        return "send depth is out of range";
    }

    if (options.queuePairs == 0 || options.queuePairs > c_maxQueuePairs)
    {
        return "queue-pair count is out of range";
    }

    if (options.iterationMode ? (options.iterations == 0) : (options.seconds == 0))
    {
        return "run length must be nonzero";
    }

    if (options.statisticsUpdateSeconds > c_maxStatisticsUpdateSeconds)
    {
        return "interval length is out of range";
    }

    // All flows share the adapter's overlapped file, which binds to a single port.
    if (options.completionPort && (!options.blocking || options.threadPerQueuePair))
    {
        return "the completion-port mode requires blocking mode and one worker thread";
    }

    const ULONGLONG perFlowBytes = std::max(
        DataBufferBytes(MakeFlowLayout(
            options,
            true)),
        DataBufferBytes(MakeFlowLayout(
            options,
            false)));
    if (perFlowBytes * options.queuePairs > c_maxDataBufferBytes)
    {
        return "registered buffers would exceed 1 GiB; reduce the size, depth, or QP count";
    }

    return nullptr;
}

static
WorkloadParameters
ToWorkloadParameters(
    const Options &options)
{
    WorkloadParameters parameters{};
    parameters.messageSize = options.messageSize;
    parameters.depth = options.depth;
    parameters.queuePairs = options.queuePairs;
    parameters.seconds = options.seconds;
    parameters.iterations = options.iterations;
    parameters.warmup = options.warmup;
    parameters.statisticsUpdateSeconds = options.statisticsUpdateSeconds;
    WI_UpdateFlag(
        parameters.flags,
        c_workloadFlagBidirectional,
        options.bidirectional);
    WI_UpdateFlag(
        parameters.flags,
        c_workloadFlagBlocking,
        options.blocking);
    WI_UpdateFlag(
        parameters.flags,
        c_workloadFlagRealtime,
        options.realtime);
    WI_UpdateFlag(
        parameters.flags,
        c_workloadFlagThreadPerQueuePair,
        options.threadPerQueuePair);
    WI_UpdateFlag(
        parameters.flags,
        c_workloadFlagIterationMode,
        options.iterationMode);
    WI_UpdateFlag(
        parameters.flags,
        c_workloadFlagCompletionPort,
        options.completionPort);
    return parameters;
}

[[nodiscard]]
static
bool
FromWorkloadParameters(
    const WorkloadParameters &parameters,
    _Inout_ Options &options)
{
    if (WI_IsAnyFlagSet(
            parameters.flags,
            ~c_validWorkloadFlags))
    {
        return false;
    }

    options.messageSize = parameters.messageSize;
    options.depth = parameters.depth;
    options.queuePairs = parameters.queuePairs;
    options.seconds = parameters.seconds;
    options.iterations = parameters.iterations;
    options.warmup = parameters.warmup;
    options.statisticsUpdateSeconds = parameters.statisticsUpdateSeconds;
    options.bidirectional = WI_IsFlagSet(
        parameters.flags,
        c_workloadFlagBidirectional);
    options.blocking = WI_IsFlagSet(
        parameters.flags,
        c_workloadFlagBlocking);
    options.realtime = WI_IsFlagSet(
        parameters.flags,
        c_workloadFlagRealtime);
    options.threadPerQueuePair = WI_IsFlagSet(
        parameters.flags,
        c_workloadFlagThreadPerQueuePair);
    options.iterationMode = WI_IsFlagSet(
        parameters.flags,
        c_workloadFlagIterationMode);
    options.completionPort = WI_IsFlagSet(
        parameters.flags,
        c_workloadFlagCompletionPort);
    return true;
}

using DllGetClassObjectFn = HRESULT(
    WINAPI *)(
    REFCLSID,
    REFIID,
    void **);

class LoadedProvider
{
public:

    LoadedProvider() = default;

    ~LoadedProvider()
    {
        m_provider.reset();
        if (m_module != nullptr)
        {
            FreeLibrary(m_module);
        }
    }

    LoadedProvider(
        const LoadedProvider &) = delete;
    LoadedProvider &
    operator=(
        const LoadedProvider &) = delete;

    [[nodiscard]]
    static
    HRESULT
    OpenForAddress(
        const sockaddr_in &address,
        _Out_ wistd::unique_ptr<LoadedProvider> &loadedProvider)
    {
        DWORD bufferLength = 0;
        INT error = 0;
        INT protocolCount = WSCEnumProtocols(
            nullptr,
            nullptr,
            &bufferLength,
            &error);
        if (protocolCount != SOCKET_ERROR || error != WSAENOBUFS)
        {
            return HRESULT_FROM_WIN32(error);
        }

        std::vector<BYTE> protocolBuffer(bufferLength);
        WSAPROTOCOL_INFOW *protocolInfo = reinterpret_cast<WSAPROTOCOL_INFOW *>(protocolBuffer.data());
        protocolCount = WSCEnumProtocols(
            nullptr,
            protocolInfo,
            &bufferLength,
            &error);
        if (protocolCount == SOCKET_ERROR)
        {
            return HRESULT_FROM_WIN32(error);
        }

        for (INT index = 0; index < protocolCount; ++index)
        {
            const WSAPROTOCOL_INFOW &candidate = protocolInfo[index];
            if (!IsNetworkDirectV2(candidate))
            {
                continue;
            }

            wistd::unique_ptr<LoadedProvider> provider = wil::make_unique_nothrow<LoadedProvider>();
            if (!provider)
            {
                return E_OUTOFMEMORY;
            }

            const HRESULT loadResult = provider->Load(candidate.ProviderId);
            if (FAILED(loadResult))
            {
                continue;
            }

            if (provider->SupportsAddress(address))
            {
                loadedProvider = wistd::move(provider);
                return S_OK;
            }
        }

        return ND_INVALID_ADDRESS;
    }

    [[nodiscard]]
    HRESULT
    OpenAdapter(
        const sockaddr_in &address,
        _Out_ wil::com_ptr_nothrow<IND2Adapter> &adapter) const
    {
        UINT64 adapterId = 0;
        const HRESULT resolveResult = m_provider->ResolveAddress(
            reinterpret_cast<const sockaddr *>(&address),
            sizeof(address),
            &adapterId);
        if (FAILED(resolveResult))
        {
            return resolveResult;
        }

        return m_provider->OpenAdapter(
            IID_IND2Adapter,
            adapterId,
            adapter.put_void());
    }

private:

    static
    bool
    IsNetworkDirectV2(
        const WSAPROTOCOL_INFOW &protocolInfo)
    {
        static constexpr DWORD c_requiredServiceFlags =
            XP1_GUARANTEED_DELIVERY | XP1_GUARANTEED_ORDER | XP1_MESSAGE_ORIENTED | XP1_CONNECT_DATA;
        static constexpr DWORD c_networkDirectProviderFlag = 0x00000010;
        static constexpr DWORD c_requiredProviderFlags = PFL_HIDDEN | c_networkDirectProviderFlag;

        return protocolInfo.iVersion == ND_VERSION_2 && protocolInfo.iAddressFamily == AF_INET &&
               protocolInfo.iSocketType == -1 && protocolInfo.iProtocol == 0 && protocolInfo.iProtocolMaxOffset == 0 &&
               (protocolInfo.dwServiceFlags1 & c_requiredServiceFlags) == c_requiredServiceFlags &&
               (protocolInfo.dwProviderFlags & c_requiredProviderFlags) == c_requiredProviderFlags;
    }

    [[nodiscard]]
    HRESULT
    Load(
        const GUID &providerId)
    {
        WCHAR providerPath[MAX_PATH]{};
        INT providerPathLength = ARRAYSIZE(providerPath);
        INT error = 0;
        GUID mutableProviderId = providerId;
        if (WSCGetProviderPath(
                &mutableProviderId,
                providerPath,
                &providerPathLength,
                &error) != 0)
        {
            return HRESULT_FROM_WIN32(error);
        }

        const DWORD expandedLength = ExpandEnvironmentStringsW(
            providerPath,
            nullptr,
            0);
        if (expandedLength == 0)
        {
            return HRESULT_FROM_WIN32(GetLastError());
        }

        std::vector<WCHAR> expandedPath(expandedLength);
        if (ExpandEnvironmentStringsW(
                providerPath,
                expandedPath.data(),
                expandedLength) != expandedLength)
        {
            return HRESULT_FROM_WIN32(GetLastError());
        }

        m_module = LoadLibraryExW(
            expandedPath.data(),
            nullptr,
            0);
        if (m_module == nullptr)
        {
            return HRESULT_FROM_WIN32(GetLastError());
        }

        const DllGetClassObjectFn getClassObject = reinterpret_cast<DllGetClassObjectFn>(GetProcAddress(
            m_module,
            "DllGetClassObject"));
        if (getClassObject == nullptr)
        {
            return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
        }

        return getClassObject(
            providerId,
            IID_IND2Provider,
            m_provider.put_void());
    }

    bool
    SupportsAddress(
        const sockaddr_in &address) const
    {
        ULONG addressListLength = 0;
        HRESULT result = m_provider->QueryAddressList(
            nullptr,
            &addressListLength);
        if (result != ND_BUFFER_OVERFLOW || addressListLength == 0)
        {
            return false;
        }

        std::vector<BYTE> addressListBuffer(addressListLength);
        SOCKET_ADDRESS_LIST *addressList = reinterpret_cast<SOCKET_ADDRESS_LIST *>(addressListBuffer.data());
        result = m_provider->QueryAddressList(
            addressList,
            &addressListLength);
        if (FAILED(result))
        {
            return false;
        }

        for (INT index = 0; index < addressList->iAddressCount; ++index)
        {
            const SOCKET_ADDRESS &candidate = addressList->Address[index];
            if (candidate.iSockaddrLength < sizeof(sockaddr_in) || candidate.lpSockaddr->sa_family != AF_INET)
            {
                continue;
            }

            const sockaddr_in *candidateAddress = reinterpret_cast<const sockaddr_in *>(candidate.lpSockaddr);
            if (candidateAddress->sin_addr.S_un.S_addr == address.sin_addr.S_un.S_addr)
            {
                return true;
            }
        }

        return false;
    }

    HMODULE m_module = nullptr;
    wil::com_ptr_nothrow<IND2Provider> m_provider{};
};

// Provider, adapter, and overlapped file shared by every queue pair of one run. Members are
// declared in creation order so they are released in reverse order.
class Adapter
{
public:

    Adapter() = default;

    Adapter(
        const Adapter &) = delete;
    Adapter &
    operator=(
        const Adapter &) = delete;

    [[nodiscard]]
    HRESULT
    Initialize(
        const sockaddr_in &localAddress)
    {
        HRESULT result = LoadedProvider::OpenForAddress(
            localAddress,
            m_provider);
        if (FAILED(result))
        {
            return result;
        }

        result = m_provider->OpenAdapter(
            localAddress,
            m_adapter);
        if (FAILED(result))
        {
            return result;
        }

        HANDLE adapterFile = nullptr;
        result = m_adapter->CreateOverlappedFile(&adapterFile);
        if (FAILED(result))
        {
            return result;
        }
        m_file.reset(adapterFile);

        m_info.InfoVersion = ND_VERSION_2;
        ULONG infoLength = sizeof(m_info);
        return m_adapter->Query(
            &m_info,
            &infoLength);
    }

    IND2Adapter *
    Get() const
    {
        return m_adapter.get();
    }

    HANDLE
    File() const
    {
        return m_file.get();
    }

    const ND2_ADAPTER_INFO &
    Info() const
    {
        return m_info;
    }

    ULONG
    CompletionQueueLimit() const
    {
        return std::min(
            m_info.MaxCompletionQueueDepth,
            c_maxCompletionQueueDepth);
    }

    _Ret_maybenull_z_ const char *
    ValidateLayout(
        const FlowLayout &layout) const
    {
        if (m_info.MaxInitiatorSge == 0 || m_info.MaxReceiveSge == 0)
        {
            return "the adapter does not support single-SGE requests";
        }

        if (layout.messageSize > m_info.MaxTransferLength)
        {
            return "the message size exceeds the adapter's maximum transfer length";
        }

        if (layout.sendSlots > m_info.MaxInitiatorQueueDepth)
        {
            return "the send depth exceeds the adapter's initiator queue depth";
        }

        if (layout.receiveSlots > m_info.MaxReceiveQueueDepth)
        {
            return "the send depth exceeds the adapter's receive queue depth";
        }

        if (CompletionDepth(layout) > CompletionQueueLimit())
        {
            return "the send depth exceeds the completion queue capacity";
        }

        return nullptr;
    }

private:

    wistd::unique_ptr<LoadedProvider> m_provider{};
    wil::com_ptr_nothrow<IND2Adapter> m_adapter{};
    wil::unique_handle m_file{};
    ND2_ADAPTER_INFO m_info{};
};

// One connection: its own completion queue, queue pair, and registered regions. The
// control region (one send page and one receive page) carries the workload exchange; the
// data region carries the stream and the Last/Ack handshake.
class Flow
{
public:

    Flow(
        Adapter &adapter,
        ULONG index)
        : m_adapter(&adapter)
        , m_index(index)
    {
    }

    ~Flow()
    {
        Shutdown();
    }

    Flow(
        const Flow &) = delete;
    Flow &
    operator=(
        const Flow &) = delete;

    [[nodiscard]]
    HRESULT
    Initialize(
        ULONG initiatorDepth,
        ULONG receiveDepth,
        ULONG completionDepth)
    {
        m_controlBuffer.reset(
            static_cast<BYTE *>(VirtualAlloc(
                nullptr,
                c_controlBufferSize,
                MEM_COMMIT | MEM_RESERVE,
                PAGE_READWRITE)));
        if (!m_controlBuffer)
        {
            return E_OUTOFMEMORY;
        }

        m_operationEvent.reset(CreateEventW(
            nullptr,
            TRUE,
            FALSE,
            nullptr));
        m_cqEvent.reset(CreateEventW(
            nullptr,
            FALSE,
            FALSE,
            nullptr));
        m_disconnectEvent.reset(CreateEventW(
            nullptr,
            TRUE,
            FALSE,
            nullptr));
        if (!m_operationEvent || !m_cqEvent || !m_disconnectEvent)
        {
            return HRESULT_FROM_WIN32(GetLastError());
        }

        m_operationOverlapped.hEvent = m_operationEvent.get();
        m_cqOverlapped.hEvent = m_cqEvent.get();
        m_disconnectOverlapped.hEvent = m_disconnectEvent.get();
        m_initiatorDepth = initiatorDepth;
        m_receiveDepth = receiveDepth;
        m_completionDepth = completionDepth;
        m_inlineThreshold = m_adapter->Info().InlineRequestThreshold;

        HRESULT result = m_adapter->Get()->CreateCompletionQueue(
            IID_IND2CompletionQueue,
            m_adapter->File(),
            completionDepth,
            0,
            0,
            m_completionQueue.put_void());
        if (FAILED(result))
        {
            return result;
        }

        result = m_adapter->Get()->CreateMemoryRegion(
            IID_IND2MemoryRegion,
            m_adapter->File(),
            m_controlRegion.put_void());
        if (FAILED(result))
        {
            return result;
        }

        result = CompleteOverlapped(
            m_controlRegion.get(),
            m_controlRegion->Register(
                m_controlBuffer.get(),
                c_controlBufferSize,
                ND_MR_FLAG_ALLOW_LOCAL_WRITE,
                &m_operationOverlapped));
        if (FAILED(result))
        {
            return result;
        }
        m_controlRegistered = true;

        result = m_adapter->Get()->CreateConnector(
            IID_IND2Connector,
            m_adapter->File(),
            m_connector.put_void());
        if (FAILED(result))
        {
            return result;
        }

        return m_adapter->Get()->CreateQueuePair(
            IID_IND2QueuePair,
            m_completionQueue.get(),
            m_completionQueue.get(),
            nullptr,
            receiveDepth,
            initiatorDepth,
            1,
            1,
            m_adapter->Info().MaxInlineDataSize,
            m_queuePair.put_void());
    }

    [[nodiscard]]
    HRESULT
    PrepareData(
        const FlowLayout &layout)
    {
        if (layout.sendSlots > m_initiatorDepth || layout.receiveSlots > m_receiveDepth ||
            layout.sendSlots + layout.receiveSlots > m_completionDepth)
        {
            return ND_INSUFFICIENT_RESOURCES;
        }

        m_layout = layout;
        m_dataBufferSize = static_cast<size_t>(DataBufferBytes(layout));
        m_dataBuffer.reset(
            static_cast<BYTE *>(VirtualAlloc(
                nullptr,
                m_dataBufferSize,
                MEM_COMMIT | MEM_RESERVE,
                PAGE_READWRITE)));
        if (!m_dataBuffer)
        {
            return E_OUTOFMEMORY;
        }

        HRESULT result = m_adapter->Get()->CreateMemoryRegion(
            IID_IND2MemoryRegion,
            m_adapter->File(),
            m_dataRegion.put_void());
        if (FAILED(result))
        {
            return result;
        }

        result = CompleteOverlapped(
            m_dataRegion.get(),
            m_dataRegion->Register(
                m_dataBuffer.get(),
                m_dataBufferSize,
                ND_MR_FLAG_ALLOW_LOCAL_WRITE,
                &m_operationOverlapped));
        if (FAILED(result))
        {
            return result;
        }
        m_dataRegistered = true;
        m_dataToken = m_dataRegion->GetLocalToken();

        m_sendContexts.resize(layout.sendSlots);
        m_sendKinds.assign(
            layout.sendSlots,
            DataKind::Warmup);
        for (ULONG slot = 0; slot < layout.sendSlots; ++slot)
        {
            m_sendContexts[slot].slot = slot;
            m_sendContexts[slot].kind = RequestKind::DataSend;
        }

        m_receiveContexts.resize(layout.receiveSlots);
        for (ULONG slot = 0; slot < layout.receiveSlots; ++slot)
        {
            m_receiveContexts[slot].slot = slot;
            m_receiveContexts[slot].kind = RequestKind::DataReceive;
        }

        m_freeSendSlots.clear();
        if (layout.transmit)
        {
            m_freeSendSlots.reserve(layout.depth);
            for (ULONG slot = layout.depth; slot > 0; --slot)
            {
                m_freeSendSlots.push_back(slot - 1);
            }
        }

        m_ackSlot = layout.receive ? layout.sendSlots - 1 : 0;
        return S_OK;
    }

    [[nodiscard]]
    HRESULT
    PostDataReceives()
    {
        for (ULONG slot = 0; slot < m_layout.receiveSlots; ++slot)
        {
            const HRESULT result = PostDataReceive(slot);
            if (FAILED(result))
            {
                return result;
            }
        }

        return S_OK;
    }

    [[nodiscard]]
    HRESULT
    PostControlReceive()
    {
        ND2_SGE receiveSge{};
        receiveSge.Buffer = m_controlBuffer.get() + c_pageSize;
        receiveSge.BufferLength = c_controlMessageLength;
        receiveSge.MemoryRegionToken = m_controlRegion->GetLocalToken();
        return m_queuePair->Receive(
            &m_controlReceiveContext,
            &receiveSge,
            1);
    }

    [[nodiscard]]
    HRESULT
    Accept(
        _In_ IND2Listener *listener,
        DWORD timeoutMilliseconds)
    {
        HRESULT result = listener->GetConnectionRequest(
            m_connector.get(),
            &m_operationOverlapped);
        if (result == ND_PENDING && WaitForSingleObject(
                                        m_operationEvent.get(),
                                        timeoutMilliseconds) == WAIT_TIMEOUT)
        {
            std::ignore = listener->CancelOverlappedRequests();
        }

        result = CompleteOverlapped(
            listener,
            result);
        if (FAILED(result))
        {
            return result;
        }

        result = CompleteOverlapped(
            m_connector.get(),
            m_connector->Accept(
                m_queuePair.get(),
                0,
                0,
                nullptr,
                0,
                &m_operationOverlapped));
        if (FAILED(result))
        {
            return result;
        }

        return ArmDisconnectNotification();
    }

    [[nodiscard]]
    HRESULT
    Connect(
        const sockaddr_in &localAddress,
        const sockaddr_in &remoteAddress)
    {
        sockaddr_in bindAddress = localAddress;
        bindAddress.sin_port = 0;
        HRESULT result = m_connector->Bind(
            reinterpret_cast<const sockaddr *>(&bindAddress),
            sizeof(bindAddress));
        if (FAILED(result))
        {
            return result;
        }

        result = CompleteOverlapped(
            m_connector.get(),
            m_connector->Connect(
                m_queuePair.get(),
                reinterpret_cast<const sockaddr *>(&remoteAddress),
                sizeof(remoteAddress),
                0,
                0,
                nullptr,
                0,
                &m_operationOverlapped));
        if (FAILED(result))
        {
            return result;
        }

        result = CompleteOverlapped(
            m_connector.get(),
            m_connector->CompleteConnect(&m_operationOverlapped));
        if (FAILED(result))
        {
            return result;
        }

        return ArmDisconnectNotification();
    }

    [[nodiscard]]
    bool
    SendControl(
        const ControlMessage &message)
    {
        *reinterpret_cast<ControlMessage *>(m_controlBuffer.get()) = message;
        ND2_SGE sendSge{};
        sendSge.Buffer = m_controlBuffer.get();
        sendSge.BufferLength = c_controlMessageLength;
        sendSge.MemoryRegionToken = m_controlRegion->GetLocalToken();
        const ULONG flags = c_controlMessageLength <= m_inlineThreshold ? ND_OP_FLAG_INLINE : 0;
        const HRESULT result = m_queuePair->Send(
            &m_controlSendContext,
            &sendSge,
            1,
            flags);
        if (FAILED(result))
        {
            std::printf(
                "Control send failed: 0x%08lx.\n",
                result);
            return false;
        }

        return WaitForControlCompletion(m_controlSendContext);
    }

    [[nodiscard]]
    bool
    ReceiveControl(
        ControlType expectedType,
        _Out_ ControlMessage &message)
    {
        message = ControlMessage{};
        if (!WaitForControlCompletion(m_controlReceiveContext))
        {
            return false;
        }

        if (m_controlReceivedBytes != c_controlMessageLength)
        {
            std::printf("Received a truncated control message.\n");
            return false;
        }

        message = *reinterpret_cast<const ControlMessage *>(m_controlBuffer.get() + c_pageSize);
        if (message.header.magic != c_controlMagic || message.header.version != c_controlVersion ||
            message.header.type != expectedType)
        {
            std::printf("Received an invalid control message.\n");
            return false;
        }

        return true;
    }

    void
    BeginDataPhase(
        const RunSchedule &schedule)
    {
        m_schedule = schedule;
        m_completionStatistics = CompletionStatistics{};
    }

    // Drains one batch of completions, reposts receives, and refills the send pipeline.
    [[nodiscard]]
    bool
    Progress(
        _Out_ ULONG &completionCount)
    {
        completionCount = 0;
        if (m_done)
        {
            return true;
        }

        ++m_completionStatistics.getResultsCalls;
        const ULONG resultCount = m_completionQueue->GetResults(
            m_results.data(),
            static_cast<ULONG>(m_results.size()));
        if (resultCount != 0)
        {
            ++m_completionStatistics.nonEmptyGetResultsCalls;
            m_completionStatistics.completions += resultCount;
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            for (ULONG index = 0; index < resultCount && !m_done; ++index)
            {
                if (!ProcessCompletion(
                        m_results[index],
                        now.QuadPart))
                {
                    return false;
                }
            }

            completionCount = resultCount;
        }

        if (!m_done)
        {
            if (!PostSends())
            {
                return false;
            }

            UpdateDone();
        }

        m_publishedTransmitBytes.store(
            m_transmitTotalBytes,
            std::memory_order_relaxed);
        m_publishedReceiveBytes.store(
            m_receiveTotalBytes,
            std::memory_order_relaxed);
        return true;
    }

    [[nodiscard]]
    NotifyState
    ArmNotify()
    {
        if (m_cqNotifyPending)
        {
            return NotifyState::Armed;
        }

        ResetEvent(m_cqEvent.get());
        ++m_completionStatistics.notifyCalls;
        const HRESULT result = m_completionQueue->Notify(
            ND_CQ_NOTIFY_ANY,
            &m_cqOverlapped);
        if (result == ND_SUCCESS)
        {
            ++m_completionStatistics.notifyImmediate;
            return NotifyState::Ready;
        }

        if (result == ND_PENDING)
        {
            ++m_completionStatistics.notifyPending;
            m_cqNotifyPending = true;
            return NotifyState::Armed;
        }

        std::printf(
            "Completion notification failed on QP %lu: 0x%08lx.\n",
            m_index,
            result);
        return NotifyState::Failed;
    }

    [[nodiscard]]
    bool
    CompleteNotify()
    {
        const HRESULT result = m_completionQueue->GetOverlappedResult(
            &m_cqOverlapped,
            FALSE);
        if (result == ND_PENDING)
        {
            return true;
        }

        m_cqNotifyPending = false;
        if (FAILED(result))
        {
            std::printf(
                "Completion notification result failed on QP %lu: 0x%08lx.\n",
                m_index,
                result);
            return false;
        }

        return true;
    }

    bool
    DisconnectSignaled() const
    {
        return WaitForSingleObject(
                   m_disconnectEvent.get(),
                   0) == WAIT_OBJECT_0;
    }

    // Called once the disconnect notification fires: drains whatever completed before the
    // disconnect and accepts it only if the peer already confirmed the end of the run.
    [[nodiscard]]
    bool
    HandleDisconnect()
    {
        m_disconnectNotifyPending = false;
        ULONG completionCount = 0;
        do
        {
            if (!Progress(completionCount))
            {
                return false;
            }
        } while (completionCount != 0 && !m_done);

        return m_done || HandlePeerLoss();
    }

    void
    Shutdown()
    {
        CancelCompletionNotification();

        if (m_connector && m_connected)
        {
            std::ignore = CompleteOverlapped(
                m_connector.get(),
                m_connector->Disconnect(&m_operationOverlapped));
            m_connected = false;
        }

        if (m_connector && m_disconnectNotifyPending)
        {
            std::ignore = m_connector->CancelOverlappedRequests();
            std::ignore = m_connector->GetOverlappedResult(
                &m_disconnectOverlapped,
                TRUE);
            m_disconnectNotifyPending = false;
        }

        m_queuePair.reset();

        if (m_dataRegion && m_dataRegistered)
        {
            std::ignore = CompleteOverlapped(
                m_dataRegion.get(),
                m_dataRegion->Deregister(&m_operationOverlapped));
            m_dataRegistered = false;
        }
        m_dataRegion.reset();
        m_dataBuffer.reset();

        m_connector.reset();

        if (m_controlRegion && m_controlRegistered)
        {
            std::ignore = CompleteOverlapped(
                m_controlRegion.get(),
                m_controlRegion->Deregister(&m_operationOverlapped));
            m_controlRegistered = false;
        }
        m_controlRegion.reset();
        m_controlBuffer.reset();

        m_completionQueue.reset();
    }

    bool
    IsDone() const
    {
        return m_done;
    }

    bool
    IsNotifyPending() const
    {
        return m_cqNotifyPending;
    }

    HANDLE
    CompletionEvent() const
    {
        return m_cqEvent.get();
    }

    HANDLE
    AdapterFile() const
    {
        return m_adapter->File();
    }

    bool
    IsNotifyOverlapped(
        const OVERLAPPED *overlapped) const
    {
        return overlapped == &m_cqOverlapped;
    }

    HANDLE
    DisconnectEvent() const
    {
        return m_disconnectEvent.get();
    }

    ULONG
    Index() const
    {
        return m_index;
    }

    const DirectionStatistics &
    TransmitStatistics() const
    {
        return m_transmitStatistics;
    }

    const DirectionStatistics &
    ReceiveStatistics() const
    {
        return m_receiveStatistics;
    }

    const CompletionStatistics &
    Completions() const
    {
        return m_completionStatistics;
    }

    ULONGLONG
    PublishedTransmitBytes() const
    {
        return m_publishedTransmitBytes.load(std::memory_order_relaxed);
    }

    ULONGLONG
    PublishedReceiveBytes() const
    {
        return m_publishedReceiveBytes.load(std::memory_order_relaxed);
    }

private:

    [[nodiscard]]
    HRESULT
    CompleteOverlapped(
        _In_ IND2Overlapped *operation,
        HRESULT result)
    {
        HRESULT completionResult = result;
        if (result == ND_PENDING)
        {
            completionResult = operation->GetOverlappedResult(
                &m_operationOverlapped,
                TRUE);
        }

        ResetEvent(m_operationEvent.get());
        return completionResult;
    }

    [[nodiscard]]
    HRESULT
    ArmDisconnectNotification()
    {
        ResetEvent(m_disconnectEvent.get());
        m_connected = true;
        const HRESULT result = m_connector->NotifyDisconnect(&m_disconnectOverlapped);
        if (result == ND_PENDING)
        {
            m_disconnectNotifyPending = true;
            return S_OK;
        }

        if (SUCCEEDED(result))
        {
            SetEvent(m_disconnectEvent.get());
        }

        return result;
    }

    void
    CancelCompletionNotification()
    {
        if (!m_cqNotifyPending || !m_completionQueue)
        {
            return;
        }

        std::ignore = m_completionQueue->CancelOverlappedRequests();
        std::ignore = m_completionQueue->GetOverlappedResult(
            &m_cqOverlapped,
            TRUE);
        m_cqNotifyPending = false;
    }

    [[nodiscard]]
    bool
    WaitForControlCompletion(
        const RequestContext &expected)
    {
        for (;;)
        {
            ND2_RESULT completion{};
            if (m_completionQueue->GetResults(
                    &completion,
                    1) == 1)
            {
                if (FAILED(completion.Status))
                {
                    std::printf(
                        "Control operation failed on QP %lu: 0x%08lx.\n",
                        m_index,
                        completion.Status);
                    return false;
                }

                if (completion.RequestContext != &expected)
                {
                    std::printf(
                        "Received an unexpected completion during the control exchange on QP %lu.\n",
                        m_index);
                    return false;
                }

                m_controlReceivedBytes = completion.BytesTransferred;
                return true;
            }

            // Arm a notification, then poll again before sleeping so a completion that
            // raced with the arm is not missed.
            if (!m_cqNotifyPending)
            {
                if (ArmNotify() == NotifyState::Failed)
                {
                    return false;
                }

                continue;
            }

            const std::array<HANDLE, 2> events = { m_cqEvent.get(),
                                                   m_disconnectEvent.get() };
            const DWORD waitResult = WaitForMultipleObjects(
                static_cast<DWORD>(events.size()),
                events.data(),
                FALSE,
                INFINITE);
            if (waitResult == WAIT_OBJECT_0)
            {
                if (!CompleteNotify())
                {
                    return false;
                }

                continue;
            }

            if (waitResult == WAIT_OBJECT_0 + 1)
            {
                std::printf(
                    "Peer disconnected during the control exchange on QP %lu.\n",
                    m_index);
            }
            else
            {
                std::printf(
                    "Completion wait failed: %lu.\n",
                    GetLastError());
            }

            return false;
        }
    }

    [[nodiscard]]
    HRESULT
    PostDataReceive(
        ULONG slot)
    {
        ND2_SGE receiveSge{};
        receiveSge.Buffer = ReceiveSlotBuffer(slot);
        receiveSge.BufferLength = m_layout.messageSize;
        receiveSge.MemoryRegionToken = m_dataToken;
        return m_queuePair->Receive(
            &m_receiveContexts[slot],
            &receiveSge,
            1);
    }

    [[nodiscard]]
    bool
    PostDataSend(
        ULONG slot,
        DataKind kind,
        ULONGLONG sequence)
    {
        BYTE *buffer = SendSlotBuffer(slot);
        DataHeader *header = reinterpret_cast<DataHeader *>(buffer);
        header->magic = c_dataMagic;
        header->kind = kind;
        header->sequence = sequence;
        m_sendKinds[slot] = kind;

        ND2_SGE sendSge{};
        sendSge.Buffer = buffer;
        sendSge.BufferLength = m_layout.messageSize;
        sendSge.MemoryRegionToken = m_dataToken;
        const ULONG flags = m_layout.messageSize <= m_inlineThreshold ? ND_OP_FLAG_INLINE : 0;
        const HRESULT result = m_queuePair->Send(
            &m_sendContexts[slot],
            &sendSge,
            1,
            flags);
        if (FAILED(result))
        {
            std::printf(
                "Send failed on QP %lu: 0x%08lx.\n",
                m_index,
                result);
            return false;
        }

        return true;
    }

    DataKind
    NextSendKind(
        LONGLONG nowTicks) const
    {
        if (m_schedule.iterationMode)
        {
            if (m_nextSendSequence < m_schedule.warmupMessages)
            {
                return DataKind::Warmup;
            }

            if (m_nextSendSequence - m_schedule.warmupMessages < m_schedule.measuredMessages)
            {
                return DataKind::Measured;
            }

            return DataKind::Last;
        }

        if (nowTicks < m_schedule.warmupEndTicks)
        {
            return DataKind::Warmup;
        }

        if (nowTicks < m_schedule.measureEndTicks)
        {
            return DataKind::Measured;
        }

        return DataKind::Last;
    }

    [[nodiscard]]
    bool
    PostSends()
    {
        if (!m_layout.transmit || m_lastPosted || m_freeSendSlots.empty())
        {
            return true;
        }

        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        while (!m_freeSendSlots.empty() && !m_lastPosted)
        {
            const DataKind kind = NextSendKind(now.QuadPart);
            const ULONG slot = m_freeSendSlots.back();
            m_freeSendSlots.pop_back();
            if (kind == DataKind::Measured && !m_measuredPostStarted)
            {
                m_measuredPostStarted = true;
                m_firstMeasuredPostTicks = now.QuadPart;
            }

            if (kind == DataKind::Last)
            {
                m_lastPosted = true;
                m_lastSequence = m_nextSendSequence;
            }

            if (!PostDataSend(
                    slot,
                    kind,
                    m_nextSendSequence))
            {
                return false;
            }

            ++m_nextSendSequence;
        }

        return true;
    }

    static
    bool
    IsPeerLossStatus(
        HRESULT status)
    {
        return status == ND_CANCELED || status == ND_CONNECTION_ABORTED || status == ND_CONNECTION_INVALID;
    }

    // The peer only disconnects after it has received our Ack (if we receive) and sent its
    // Ack (if we transmit), so in that state a disconnect is the normal end of the run.
    bool
    IsPeerConfirmedComplete() const
    {
        return (!m_layout.transmit || m_ackReceived) && (!m_layout.receive || m_ackPosted);
    }

    [[nodiscard]]
    bool
    HandlePeerLoss()
    {
        if (IsPeerConfirmedComplete())
        {
            m_done = true;
            return true;
        }

        std::printf(
            "Peer disconnected on QP %lu before the run completed.\n",
            m_index);
        return false;
    }

    [[nodiscard]]
    bool
    ProcessCompletion(
        const ND2_RESULT &completion,
        LONGLONG nowTicks)
    {
        if (FAILED(completion.Status))
        {
            if (IsPeerLossStatus(completion.Status))
            {
                return HandlePeerLoss();
            }

            std::printf(
                "Completion failed on QP %lu: 0x%08lx.\n",
                m_index,
                completion.Status);
            return false;
        }

        const RequestContext *context = static_cast<const RequestContext *>(completion.RequestContext);
        if (context != nullptr && context->kind == RequestKind::DataSend && context->slot < m_layout.sendSlots)
        {
            ProcessSendCompletion(
                context->slot,
                nowTicks);
            return true;
        }

        if (context != nullptr && context->kind == RequestKind::DataReceive && context->slot < m_layout.receiveSlots)
        {
            return ProcessReceiveCompletion(
                context->slot,
                completion.BytesTransferred,
                nowTicks);
        }

        std::printf(
            "Received a completion with an unknown context on QP %lu.\n",
            m_index);
        return false;
    }

    void
    ProcessSendCompletion(
        ULONG slot,
        LONGLONG nowTicks)
    {
        const DataKind kind = m_sendKinds[slot];
        if (kind == DataKind::Ack)
        {
            m_ackSendCompleted = true;
            return;
        }

        m_freeSendSlots.push_back(slot);
        if (kind == DataKind::Last)
        {
            m_lastSendCompleted = true;
            return;
        }

        m_transmitTotalBytes += m_layout.messageSize;
        if (kind == DataKind::Warmup)
        {
            m_warmupSendCompleted = true;
            m_lastWarmupSendTicks = nowTicks;
            return;
        }

        // The window opens when the last warm-up send completes, or at the first measured
        // post when there is no warm-up.
        if (!m_transmitStatistics.started)
        {
            m_transmitStatistics.started = true;
            m_transmitStatistics.startTicks = m_warmupSendCompleted ? m_lastWarmupSendTicks : m_firstMeasuredPostTicks;
        }

        ++m_transmitStatistics.messages;
        m_transmitStatistics.bytes += m_layout.messageSize;
        m_transmitStatistics.endTicks = nowTicks;
    }

    void
    RecordMeasuredReceive(
        LONGLONG nowTicks)
    {
        if (!m_receiveStatistics.started)
        {
            m_receiveStatistics.started = true;
            m_receiveStatistics.endTicks = nowTicks;
            if (!m_dataReceived)
            {
                // Without a warm-up arrival to anchor the window, the first message only
                // starts the clock and is not counted.
                m_receiveStatistics.startTicks = nowTicks;
                return;
            }

            m_receiveStatistics.startTicks = m_lastDataReceiveTicks;
        }

        ++m_receiveStatistics.messages;
        m_receiveStatistics.bytes += m_layout.messageSize;
        m_receiveStatistics.endTicks = nowTicks;
    }

    [[nodiscard]]
    bool
    ProcessReceiveCompletion(
        ULONG slot,
        ULONG bytesTransferred,
        LONGLONG nowTicks)
    {
        if (bytesTransferred != m_layout.messageSize)
        {
            std::printf(
                "Received %lu bytes on QP %lu; expected %lu.\n",
                bytesTransferred,
                m_index,
                m_layout.messageSize);
            return false;
        }

        const DataHeader *header = reinterpret_cast<const DataHeader *>(ReceiveSlotBuffer(slot));
        const DataKind kind = header->kind;
        const ULONGLONG sequence = header->sequence;
        if (header->magic != c_dataMagic)
        {
            std::printf(
                "Received an invalid data header on QP %lu.\n",
                m_index);
            return false;
        }

        if (kind == DataKind::Ack)
        {
            if (!m_layout.transmit || !m_lastPosted || m_ackReceived || sequence != m_lastSequence)
            {
                std::printf(
                    "Received an unexpected acknowledgement on QP %lu.\n",
                    m_index);
                return false;
            }

            m_ackReceived = true;
        }
        else
        {
            if (!m_layout.receive || m_lastReceived ||
                (kind != DataKind::Warmup && kind != DataKind::Measured && kind != DataKind::Last))
            {
                std::printf(
                    "Received an unexpected data message on QP %lu.\n",
                    m_index);
                return false;
            }

            if (sequence != m_expectedSequence)
            {
                std::printf(
                    "Sequence error on QP %lu: expected %llu, received %llu (loss or reordering).\n",
                    m_index,
                    m_expectedSequence,
                    sequence);
                return false;
            }

            ++m_expectedSequence;
            if (kind == DataKind::Last)
            {
                m_lastReceived = true;
                m_ackPosted = true;
                if (!PostDataSend(
                        m_ackSlot,
                        DataKind::Ack,
                        sequence))
                {
                    return false;
                }
            }
            else
            {
                m_receiveTotalBytes += m_layout.messageSize;
                if (kind == DataKind::Measured)
                {
                    RecordMeasuredReceive(nowTicks);
                }

                m_dataReceived = true;
                m_lastDataReceiveTicks = nowTicks;
            }
        }

        const HRESULT result = PostDataReceive(slot);
        if (FAILED(result))
        {
            std::printf(
                "Receive failed on QP %lu: 0x%08lx.\n",
                m_index,
                result);
            return false;
        }

        return true;
    }

    void
    UpdateDone()
    {
        const bool transmitDone = !m_layout.transmit || (m_lastSendCompleted && m_ackReceived);
        const bool receiveDone = !m_layout.receive || (m_lastReceived && m_ackSendCompleted);
        m_done = transmitDone && receiveDone;
    }

    BYTE *
    SendSlotBuffer(
        ULONG slot) const
    {
        return m_dataBuffer.get() + m_layout.stride * slot;
    }

    BYTE *
    ReceiveSlotBuffer(
        ULONG slot) const
    {
        return m_dataBuffer.get() + m_layout.stride * (static_cast<size_t>(m_layout.sendSlots) + slot);
    }

    Adapter *m_adapter = nullptr;
    ULONG m_index = 0;
    ULONG m_initiatorDepth = 0;
    ULONG m_receiveDepth = 0;
    ULONG m_completionDepth = 0;
    ULONG m_inlineThreshold = 0;
    UINT32 m_dataToken = 0;
    FlowLayout m_layout{};
    RunSchedule m_schedule{};
    size_t m_dataBufferSize = 0;
    wil::unique_virtualalloc_ptr<BYTE> m_controlBuffer{};
    wil::unique_virtualalloc_ptr<BYTE> m_dataBuffer{};
    wil::com_ptr_nothrow<IND2CompletionQueue> m_completionQueue{};
    wil::com_ptr_nothrow<IND2MemoryRegion> m_controlRegion{};
    wil::com_ptr_nothrow<IND2Connector> m_connector{};
    wil::com_ptr_nothrow<IND2QueuePair> m_queuePair{};
    wil::com_ptr_nothrow<IND2MemoryRegion> m_dataRegion{};
    wil::unique_handle m_operationEvent{};
    wil::unique_handle m_cqEvent{};
    wil::unique_handle m_disconnectEvent{};
    OVERLAPPED m_operationOverlapped{};
    OVERLAPPED m_cqOverlapped{};
    OVERLAPPED m_disconnectOverlapped{};
    RequestContext m_controlSendContext = { 0,
                                            RequestKind::ControlSend };
    RequestContext m_controlReceiveContext = { 0,
                                               RequestKind::ControlReceive };
    ULONG m_controlReceivedBytes = 0;
    std::vector<RequestContext> m_sendContexts{};
    std::vector<RequestContext> m_receiveContexts{};
    std::vector<DataKind> m_sendKinds{};
    std::vector<ULONG> m_freeSendSlots{};
    std::array<ND2_RESULT, c_resultBatchSize> m_results{};
    ULONG m_ackSlot = 0;
    ULONGLONG m_nextSendSequence = 0;
    ULONGLONG m_lastSequence = 0;
    ULONGLONG m_expectedSequence = 0;
    LONGLONG m_firstMeasuredPostTicks = 0;
    LONGLONG m_lastWarmupSendTicks = 0;
    LONGLONG m_lastDataReceiveTicks = 0;
    ULONGLONG m_transmitTotalBytes = 0;
    ULONGLONG m_receiveTotalBytes = 0;
    DirectionStatistics m_transmitStatistics{};
    DirectionStatistics m_receiveStatistics{};
    CompletionStatistics m_completionStatistics{};
    std::atomic<ULONGLONG> m_publishedTransmitBytes{};
    std::atomic<ULONGLONG> m_publishedReceiveBytes{};
    bool m_measuredPostStarted = false;
    bool m_warmupSendCompleted = false;
    bool m_dataReceived = false;
    bool m_lastPosted = false;
    bool m_lastSendCompleted = false;
    bool m_ackReceived = false;
    bool m_lastReceived = false;
    bool m_ackPosted = false;
    bool m_ackSendCompleted = false;
    bool m_done = false;
    bool m_cqNotifyPending = false;
    bool m_disconnectNotifyPending = false;
    bool m_controlRegistered = false;
    bool m_dataRegistered = false;
    bool m_connected = false;
};

// Owns the queue pairs of one run and tears them down in reverse creation order.
class FlowSet
{
public:

    FlowSet() = default;

    ~FlowSet()
    {
        for (size_t index = m_flows.size(); index > 0; --index)
        {
            m_flows[index - 1].reset();
        }
    }

    FlowSet(
        const FlowSet &) = delete;
    FlowSet &
    operator=(
        const FlowSet &) = delete;

    [[nodiscard]]
    HRESULT
    Add(
        Adapter &adapter,
        ULONG initiatorDepth,
        ULONG receiveDepth,
        ULONG completionDepth)
    {
        wistd::unique_ptr<Flow> flow = wil::make_unique_nothrow<Flow>(
            adapter,
            static_cast<ULONG>(m_flows.size()));
        if (!flow)
        {
            return E_OUTOFMEMORY;
        }

        const HRESULT result = flow->Initialize(
            initiatorDepth,
            receiveDepth,
            completionDepth);
        m_flows.push_back(std::move(flow));
        return result;
    }

    Flow &
    operator[](
        size_t index) const
    {
        return *m_flows[index];
    }

    const std::vector<wistd::unique_ptr<Flow>> &
    Flows() const
    {
        return m_flows;
    }

private:

    std::vector<wistd::unique_ptr<Flow>> m_flows{};
};

static
DWORD
SetRealtimeThreadPriority(
    _In_ HANDLE threadHandle)
{
    if (!SetThreadPriority(
            threadHandle,
            THREAD_PRIORITY_TIME_CRITICAL))
    {
        const DWORD error = GetLastError();
        std::printf(
            "Failed to set time-critical thread priority: %lu.\n",
            error);
        return error;
    }

    return ERROR_SUCCESS;
}

static
DWORD
EnableRealtimePriority()
{
    if (!SetPriorityClass(
            GetCurrentProcess(),
            REALTIME_PRIORITY_CLASS))
    {
        const DWORD error = GetLastError();
        std::printf(
            "Failed to set real-time process priority: %lu.\n",
            error);
        return error;
    }

    return SetRealtimeThreadPriority(GetCurrentThread());
}

static
void
RestorePriority(
    DWORD processPriorityClass,
    int threadPriority)
{
    if (!SetThreadPriority(
            GetCurrentThread(),
            threadPriority))
    {
        std::printf(
            "Failed to restore server thread priority: %lu.\n",
            GetLastError());
    }

    if (!SetPriorityClass(
            GetCurrentProcess(),
            processPriorityClass))
    {
        std::printf(
            "Failed to restore server process priority: %lu.\n",
            GetLastError());
    }
}

// Drives a set of flows from one thread: polls each flow's CQ in turn, or in blocking mode
// arms a notification on every idle CQ and waits on all of them.
class Worker
{
public:

    Worker(
        std::vector<Flow *> &&flows,
        std::atomic<bool> &abortRun,
        bool blocking,
        bool realtime,
        HANDLE completionPort)
        : m_flows(std::move(flows))
        , m_abort(&abortRun)
        , m_blocking(blocking)
        , m_realtime(realtime)
        , m_completionPort(completionPort)
    {
    }

    Worker(
        const Worker &) = delete;
    Worker &
    operator=(
        const Worker &) = delete;

    [[nodiscard]]
    HRESULT
    Start()
    {
        m_thread.reset(CreateThread(
            nullptr,
            0,
            ThreadProc,
            this,
            0,
            nullptr));
        if (!m_thread)
        {
            return HRESULT_FROM_WIN32(GetLastError());
        }

        return S_OK;
    }

    HANDLE
    Thread() const
    {
        return m_thread.get();
    }

    bool
    Succeeded() const
    {
        return m_succeeded;
    }

private:

    static
    DWORD WINAPI
    ThreadProc(
        _In_ void *parameter)
    {
        Worker *worker = static_cast<Worker *>(parameter);
        worker->m_succeeded = worker->Run();
        if (!worker->m_succeeded)
        {
            worker->m_abort->store(true);
        }

        return 0;
    }

    [[nodiscard]]
    bool
    Run()
    {
        if (m_realtime && SetRealtimeThreadPriority(GetCurrentThread()) != ERROR_SUCCESS)
        {
            return false;
        }

        ULONG idlePolls = 0;
        for (;;)
        {
            if (m_abort->load(std::memory_order_relaxed))
            {
                return false;
            }

            ULONG completionCount = 0;
            if (!PollOnce(completionCount))
            {
                return false;
            }

            if (AllDone())
            {
                return true;
            }

            if (completionCount != 0)
            {
                idlePolls = 0;
                continue;
            }

            if (!m_blocking)
            {
                ++idlePolls;
                if (idlePolls % c_idlePollsPerDisconnectCheck == 0 && !CheckDisconnects())
                {
                    return false;
                }

                YieldProcessor();
                continue;
            }

            if (!WaitForCompletions())
            {
                return false;
            }
        }
    }

    [[nodiscard]]
    bool
    PollOnce(
        _Out_ ULONG &completionCount)
    {
        completionCount = 0;
        for (Flow *flow : m_flows)
        {
            ULONG flowCompletions = 0;
            if (!flow->Progress(flowCompletions))
            {
                return false;
            }

            completionCount += flowCompletions;
        }

        return true;
    }

    bool
    AllDone() const
    {
        for (const Flow *flow : m_flows)
        {
            if (!flow->IsDone())
            {
                return false;
            }
        }

        return true;
    }

    [[nodiscard]]
    bool
    CheckDisconnects()
    {
        for (Flow *flow : m_flows)
        {
            if (!flow->IsDone() && flow->DisconnectSignaled() && !flow->HandleDisconnect())
            {
                return false;
            }
        }

        return true;
    }

    [[nodiscard]]
    bool
    WaitForCompletions()
    {
        // Arm every idle CQ first, then return to polling once so completions that raced
        // with the arm are drained before sleeping.
        bool armed = false;
        for (Flow *flow : m_flows)
        {
            if (flow->IsDone() || flow->IsNotifyPending())
            {
                continue;
            }

            if (flow->ArmNotify() == NotifyState::Failed)
            {
                return false;
            }

            armed = true;
        }

        if (armed)
        {
            return true;
        }

        if (m_completionPort != nullptr)
        {
            return WaitOnCompletionPort();
        }

        std::array<HANDLE, c_maxQueuePairs * 2> handles{};
        std::array<Flow *, c_maxQueuePairs * 2> handleFlows{};
        std::array<bool, c_maxQueuePairs * 2> disconnectHandles{};
        DWORD handleCount = 0;
        for (Flow *flow : m_flows)
        {
            if (flow->IsDone())
            {
                continue;
            }

            handles[handleCount] = flow->CompletionEvent();
            handleFlows[handleCount] = flow;
            disconnectHandles[handleCount] = false;
            ++handleCount;
            handles[handleCount] = flow->DisconnectEvent();
            handleFlows[handleCount] = flow;
            disconnectHandles[handleCount] = true;
            ++handleCount;
        }

        if (handleCount == 0)
        {
            return true;
        }

        const DWORD waitResult = WaitForMultipleObjects(
            handleCount,
            handles.data(),
            FALSE,
            c_blockingWaitMilliseconds);
        if (waitResult == WAIT_TIMEOUT)
        {
            return true;
        }

        if (waitResult >= WAIT_OBJECT_0 + handleCount)
        {
            std::printf(
                "Completion wait failed: %lu.\n",
                GetLastError());
            return false;
        }

        const DWORD index = waitResult - WAIT_OBJECT_0;
        return disconnectHandles[index] ? handleFlows[index]->HandleDisconnect() : handleFlows[index]->CompleteNotify();
    }

    // Every pending operation on the adapter's overlapped file queues a packet here, including
    // disconnect notifications, which are still detected through their events.
    [[nodiscard]]
    bool
    WaitOnCompletionPort()
    {
        DWORD bytes = 0;
        ULONG_PTR key = 0;
        OVERLAPPED *overlapped = nullptr;
        const BOOL dequeued = GetQueuedCompletionStatus(
            m_completionPort,
            &bytes,
            &key,
            &overlapped,
            c_blockingWaitMilliseconds);
        if (overlapped == nullptr)
        {
            const DWORD error = GetLastError();
            if (!dequeued && error != WAIT_TIMEOUT)
            {
                std::printf(
                    "Completion port wait failed: %lu.\n",
                    error);
                return false;
            }

            return CheckDisconnects();
        }

        for (Flow *flow : m_flows)
        {
            if (flow->IsNotifyOverlapped(overlapped))
            {
                return flow->CompleteNotify();
            }
        }

        return CheckDisconnects();
    }

    std::vector<Flow *> m_flows{};
    std::atomic<bool> *m_abort = nullptr;
    wil::unique_handle m_thread{};
    bool m_blocking = false;
    bool m_realtime = false;
    HANDLE m_completionPort = nullptr;
    bool m_succeeded = false;
};

[[nodiscard]]
static
bool
QueryCpuTimes(
    _Out_ CpuTimes &times)
{
    times = CpuTimes{};
    FILETIME creationTime{};
    FILETIME exitTime{};
    FILETIME kernelTime{};
    FILETIME userTime{};
    if (!GetProcessTimes(
            GetCurrentProcess(),
            &creationTime,
            &exitTime,
            &kernelTime,
            &userTime))
    {
        std::printf(
            "Failed to query process CPU time: %lu.\n",
            GetLastError());
        return false;
    }

    ULARGE_INTEGER value{};
    value.LowPart = kernelTime.dwLowDateTime;
    value.HighPart = kernelTime.dwHighDateTime;
    times.kernel = value.QuadPart;
    value.LowPart = userTime.dwLowDateTime;
    value.HighPart = userTime.dwHighDateTime;
    times.user = value.QuadPart;
    return true;
}

static
DirectionStatistics
AggregateDirection(
    const std::vector<wistd::unique_ptr<Flow>> &flows,
    bool transmit)
{
    DirectionStatistics total{};
    for (const wistd::unique_ptr<Flow> &flow : flows)
    {
        const DirectionStatistics &flowStatistics = transmit ? flow->TransmitStatistics() : flow->ReceiveStatistics();
        if (!flowStatistics.started)
        {
            continue;
        }

        total.messages += flowStatistics.messages;
        total.bytes += flowStatistics.bytes;
        if (!total.started || flowStatistics.startTicks < total.startTicks)
        {
            total.startTicks = flowStatistics.startTicks;
        }

        if (!total.started || flowStatistics.endTicks > total.endTicks)
        {
            total.endTicks = flowStatistics.endTicks;
        }

        total.started = true;
    }

    return total;
}

static
Rates
CalculateRates(
    const DirectionStatistics &statistics,
    LONGLONG frequency)
{
    Rates rates{};
    if (!statistics.started || statistics.endTicks <= statistics.startTicks || frequency <= 0)
    {
        return rates;
    }

    rates.seconds = static_cast<double>(statistics.endTicks - statistics.startTicks) / static_cast<double>(frequency);
    const double bytesPerSecond = static_cast<double>(statistics.bytes) / rates.seconds;
    rates.megabytesPerSecond = bytesPerSecond / c_bytesPerMegabyte;
    rates.gigabitsPerSecond = bytesPerSecond * c_bitsPerByte / c_bitsPerGigabit;
    rates.messagesPerSecond = static_cast<double>(statistics.messages) / rates.seconds;
    return rates;
}

static
const char *
DirectionName(
    bool client,
    bool transmit)
{
    return transmit == client ? "client -> server" : "server -> client";
}

struct IntervalState
{
    LONGLONG phaseStartTicks = 0;
    LONGLONG lastTicks = 0;
    ULONGLONG lastTransmitBytes = 0;
    ULONGLONG lastReceiveBytes = 0;
};

static
void
PrintInterval(
    const std::vector<wistd::unique_ptr<Flow>> &flows,
    const FlowLayout &layout,
    LONGLONG frequency,
    _Inout_ IntervalState &state)
{
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    ULONGLONG transmitBytes = 0;
    ULONGLONG receiveBytes = 0;
    for (const wistd::unique_ptr<Flow> &flow : flows)
    {
        transmitBytes += flow->PublishedTransmitBytes();
        receiveBytes += flow->PublishedReceiveBytes();
    }

    const double seconds = static_cast<double>(now.QuadPart - state.lastTicks) / static_cast<double>(frequency);
    if (seconds > 0.0)
    {
        std::printf(
            "[%7.1f-%7.1f s]",
            static_cast<double>(state.lastTicks - state.phaseStartTicks) / static_cast<double>(frequency),
            static_cast<double>(now.QuadPart - state.phaseStartTicks) / static_cast<double>(frequency));
        if (layout.transmit)
        {
            const double bytesPerSecond = static_cast<double>(transmitBytes - state.lastTransmitBytes) / seconds;
            std::printf(
                " tx %10.3f MB/s %8.3f Gb/s",
                bytesPerSecond / c_bytesPerMegabyte,
                bytesPerSecond * c_bitsPerByte / c_bitsPerGigabit);
        }

        if (layout.receive)
        {
            const double bytesPerSecond = static_cast<double>(receiveBytes - state.lastReceiveBytes) / seconds;
            std::printf(
                " rx %10.3f MB/s %8.3f Gb/s",
                bytesPerSecond / c_bytesPerMegabyte,
                bytesPerSecond * c_bitsPerByte / c_bitsPerGigabit);
        }

        std::printf("\n");
        std::fflush(stdout);
    }

    state.lastTicks = now.QuadPart;
    state.lastTransmitBytes = transmitBytes;
    state.lastReceiveBytes = receiveBytes;
}

static
void
PrintDirectionReport(
    const std::vector<wistd::unique_ptr<Flow>> &flows,
    const Options &options,
    bool client,
    bool transmit,
    LONGLONG frequency)
{
    const DirectionStatistics total = AggregateDirection(
        flows,
        transmit);
    const Rates rates = CalculateRates(
        total,
        frequency);
    std::printf(
        "\n"
        "Throughput summary (%s %s, %s)\n"
        "----------------------------------------\n"
        "  Queue pairs    : %lu\n"
        "  Message size   : %lu bytes\n"
        "  Depth          : %lu sends per QP\n"
        "  Completion mode: %s, %s\n"
        "  Messages       : %llu\n"
        "  Bytes          : %llu\n"
        "  Elapsed        : %.6f seconds\n"
        "  Throughput     : %.3f MB/s\n"
        "  Data rate      : %.3f Gb/s\n"
        "  Message rate   : %.1f messages/s\n",
        client ? "client" : "server",
        transmit ? "tx" : "rx",
        DirectionName(
            client,
            transmit),
        options.queuePairs,
        options.messageSize,
        options.depth,
        options.blocking ? "blocking" : "polling",
        options.threadPerQueuePair ? "one thread per QP" : "one thread for all QPs",
        total.messages,
        total.bytes,
        rates.seconds,
        rates.megabytesPerSecond,
        rates.gigabitsPerSecond,
        rates.messagesPerSecond);

    if (flows.size() > 1)
    {
        for (const wistd::unique_ptr<Flow> &flow : flows)
        {
            const DirectionStatistics &flowStatistics = transmit ? flow->TransmitStatistics() : flow->ReceiveStatistics();
            const Rates flowRates = CalculateRates(
                flowStatistics,
                frequency);
            std::printf(
                "  QP %lu           : %.3f MB/s, %.3f Gb/s (%llu messages)\n",
                flow->Index(),
                flowRates.megabytesPerSecond,
                flowRates.gigabitsPerSecond,
                flowStatistics.messages);
        }
    }

    if (total.messages == 0)
    {
        std::printf("  Warning: no measured messages completed in this direction.\n");
    }
}

static
void
PrintResultLine(
    const std::vector<wistd::unique_ptr<Flow>> &flows,
    const Options &options,
    bool client,
    bool transmit,
    LONGLONG frequency,
    double cpuPercent)
{
    const DirectionStatistics total = AggregateDirection(
        flows,
        transmit);
    const Rates rates = CalculateRates(
        total,
        frequency);
    std::printf(
        "RESULT role=%s dir=%s size=%lu depth=%lu qps=%lu mode=%s bytes=%llu seconds=%.6f MBps=%.3f Gbps=%.3f "
        "msgps=%.1f cpu=%.1f\n",
        client ? "client" : "server",
        transmit ? "tx" : "rx",
        options.messageSize,
        options.depth,
        options.queuePairs,
        options.completionPort ? "iocp" : (options.blocking ? "block" : "poll"),
        total.bytes,
        rates.seconds,
        rates.megabytesPerSecond,
        rates.gigabitsPerSecond,
        rates.messagesPerSecond,
        cpuPercent);
}

static
void
PrintReport(
    const std::vector<wistd::unique_ptr<Flow>> &flows,
    const Options &options,
    bool client,
    LONGLONG frequency,
    LONGLONG phaseTicks,
    const CpuTimes &cpuStart,
    const CpuTimes &cpuEnd)
{
    const FlowLayout layout = MakeFlowLayout(
        options,
        client);
    if (layout.transmit)
    {
        PrintDirectionReport(
            flows,
            options,
            client,
            true,
            frequency);
    }

    if (layout.receive)
    {
        PrintDirectionReport(
            flows,
            options,
            client,
            false,
            frequency);
    }

    CompletionStatistics completions{};
    for (const wistd::unique_ptr<Flow> &flow : flows)
    {
        const CompletionStatistics &flowCompletions = flow->Completions();
        completions.getResultsCalls += flowCompletions.getResultsCalls;
        completions.nonEmptyGetResultsCalls += flowCompletions.nonEmptyGetResultsCalls;
        completions.completions += flowCompletions.completions;
        completions.notifyCalls += flowCompletions.notifyCalls;
        completions.notifyImmediate += flowCompletions.notifyImmediate;
        completions.notifyPending += flowCompletions.notifyPending;
    }

    const double phaseSeconds = static_cast<double>(phaseTicks) / static_cast<double>(frequency);
    const double userSeconds = static_cast<double>(cpuEnd.user - cpuStart.user) / c_cpuTicksPerSecond;
    const double kernelSeconds = static_cast<double>(cpuEnd.kernel - cpuStart.kernel) / c_cpuTicksPerSecond;
    const double cpuSeconds = userSeconds + kernelSeconds;
    const double cpuPercent = phaseSeconds > 0.0 ? cpuSeconds / phaseSeconds * c_percent : 0.0;
    const double completionsPerCall = completions.getResultsCalls != 0 ? static_cast<double>(completions.completions) /
                                                                             static_cast<double>(completions.getResultsCalls)
                                                                       : 0.0;
    const double completionsPerNonEmptyCall =
        completions.nonEmptyGetResultsCalls != 0
            ? static_cast<double>(completions.completions) / static_cast<double>(completions.nonEmptyGetResultsCalls)
            : 0.0;

    std::printf(
        "\n"
        "Completion and CPU summary (%s)\n"
        "----------------------------------------\n"
        "  GetResults calls : %llu (%llu returned completions)\n"
        "  Completions      : %llu\n"
        "  Completions/call : %.2f (%.2f per non-empty call, batch %lu)\n",
        client ? "client" : "server",
        completions.getResultsCalls,
        completions.nonEmptyGetResultsCalls,
        completions.completions,
        completionsPerCall,
        completionsPerNonEmptyCall,
        c_resultBatchSize);
    if (options.blocking)
    {
        std::printf(
            "  Notify calls     : %llu (ND_SUCCESS immediately: %llu, ND_PENDING: %llu)\n",
            completions.notifyCalls,
            completions.notifyImmediate,
            completions.notifyPending);
    }

    std::printf(
        "  Data phase       : %.3f seconds (includes warm-up and teardown handshake)\n"
        "  CPU time         : %.3f seconds (user %.3f, kernel %.3f)\n"
        "  CPU utilization  : %.1f%% of one core\n"
        "\n",
        phaseSeconds,
        cpuSeconds,
        userSeconds,
        kernelSeconds,
        cpuPercent);

    if (layout.transmit)
    {
        PrintResultLine(
            flows,
            options,
            client,
            true,
            frequency,
            cpuPercent);
    }

    if (layout.receive)
    {
        PrintResultLine(
            flows,
            options,
            client,
            false,
            frequency,
            cpuPercent);
    }

    std::fflush(stdout);
}

[[nodiscard]]
static
bool
WaitForWorkers(
    const std::vector<HANDLE> &threads,
    const std::vector<wistd::unique_ptr<Flow>> &flows,
    const FlowLayout &layout,
    ULONG statisticsUpdateSeconds,
    LONGLONG frequency,
    LONGLONG startTicks)
{
    if (threads.empty())
    {
        return true;
    }

    const DWORD timeout = statisticsUpdateSeconds != 0 ? statisticsUpdateSeconds * c_millisecondsPerSecond : INFINITE;
    IntervalState interval{};
    interval.phaseStartTicks = startTicks;
    interval.lastTicks = startTicks;
    for (;;)
    {
        const DWORD waitResult = WaitForMultipleObjects(
            static_cast<DWORD>(threads.size()),
            threads.data(),
            TRUE,
            timeout);
        if (waitResult == WAIT_TIMEOUT)
        {
            PrintInterval(
                flows,
                layout,
                frequency,
                interval);
            continue;
        }

        if (waitResult >= WAIT_OBJECT_0 + threads.size())
        {
            std::printf(
                "Worker wait failed: %lu.\n",
                GetLastError());
            return false;
        }

        return true;
    }
}

static
void
JoinWorkers(
    const std::vector<HANDLE> &threads)
{
    for (const HANDLE thread : threads)
    {
        WaitForSingleObject(
            thread,
            INFINITE);
    }
}

[[nodiscard]]
static
bool
RunDataPhase(
    const std::vector<wistd::unique_ptr<Flow>> &flows,
    const Options &options,
    bool client)
{
    LARGE_INTEGER frequency{};
    if (!QueryPerformanceFrequency(&frequency))
    {
        std::printf("Failed to query the performance-counter frequency.\n");
        return false;
    }

    CpuTimes cpuStart{};
    if (!QueryCpuTimes(cpuStart))
    {
        return false;
    }

    LARGE_INTEGER startTime{};
    QueryPerformanceCounter(&startTime);

    RunSchedule schedule{};
    schedule.iterationMode = options.iterationMode;
    if (options.iterationMode)
    {
        schedule.warmupMessages = options.warmup;
        schedule.measuredMessages = options.iterations;
    }
    else
    {
        schedule.warmupEndTicks = startTime.QuadPart + static_cast<LONGLONG>(options.warmup) * frequency.QuadPart;
        schedule.measureEndTicks = schedule.warmupEndTicks + static_cast<LONGLONG>(options.seconds) * frequency.QuadPart;
    }

    for (const wistd::unique_ptr<Flow> &flow : flows)
    {
        flow->BeginDataPhase(schedule);
    }

    wil::unique_handle completionPort{};
    if (options.completionPort && !flows.empty())
    {
        completionPort.reset(CreateIoCompletionPort(
            INVALID_HANDLE_VALUE,
            nullptr,
            0,
            1));
        if (!completionPort || CreateIoCompletionPort(
                                   flows.front()->AdapterFile(),
                                   completionPort.get(),
                                   0,
                                   0) == nullptr)
        {
            std::printf(
                "Failed to bind the adapter file to a completion port: %lu.\n",
                GetLastError());
            return false;
        }
    }

    std::atomic<bool> abortRun{};
    std::vector<wistd::unique_ptr<Worker>> workers;
    if (options.threadPerQueuePair)
    {
        for (const wistd::unique_ptr<Flow> &flow : flows)
        {
            std::vector<Flow *> workerFlows(
                1,
                flow.get());
            workers.push_back(
                wil::make_unique_nothrow<Worker>(
                    std::move(workerFlows),
                    abortRun,
                    options.blocking,
                    options.realtime,
                    nullptr));
        }
    }
    else
    {
        std::vector<Flow *> workerFlows;
        for (const wistd::unique_ptr<Flow> &flow : flows)
        {
            workerFlows.push_back(flow.get());
        }

        workers.push_back(
            wil::make_unique_nothrow<Worker>(
                std::move(workerFlows),
                abortRun,
                options.blocking,
                options.realtime,
                completionPort.get()));
    }

    std::vector<HANDLE> threads;
    bool started = true;
    for (const wistd::unique_ptr<Worker> &worker : workers)
    {
        const HRESULT result = worker ? worker->Start() : E_OUTOFMEMORY;
        if (FAILED(result))
        {
            std::printf(
                "Failed to start a worker thread: 0x%08lx.\n",
                result);
            abortRun.store(true);
            started = false;
            break;
        }

        threads.push_back(worker->Thread());
    }

    const FlowLayout layout = MakeFlowLayout(
        options,
        client);
    if (!started || !WaitForWorkers(
                        threads,
                        flows,
                        layout,
                        options.statisticsUpdateSeconds,
                        frequency.QuadPart,
                        startTime.QuadPart))
    {
        abortRun.store(true);
        JoinWorkers(threads);
        return false;
    }

    LARGE_INTEGER endTime{};
    QueryPerformanceCounter(&endTime);
    CpuTimes cpuEnd{};
    if (!QueryCpuTimes(cpuEnd))
    {
        return false;
    }

    for (const wistd::unique_ptr<Worker> &worker : workers)
    {
        if (!worker->Succeeded())
        {
            return false;
        }
    }

    PrintReport(
        flows,
        options,
        client,
        frequency.QuadPart,
        endTime.QuadPart - startTime.QuadPart,
        cpuStart,
        cpuEnd);
    return true;
}

[[nodiscard]]
static
HRESULT
CreateListener(
    const Adapter &adapter,
    const sockaddr_in &address,
    _Out_ wil::com_ptr_nothrow<IND2Listener> &listener)
{
    HRESULT result = adapter.Get()->CreateListener(
        IID_IND2Listener,
        adapter.File(),
        listener.put_void());
    if (FAILED(result))
    {
        return result;
    }

    result = listener->Bind(
        reinterpret_cast<const sockaddr *>(&address),
        sizeof(address));
    if (FAILED(result))
    {
        return result;
    }

    return listener->Listen(0);
}

static
void
ShowUsage()
{
    std::printf(
        "ndtput -s [-1] <listen-ip>[:port]\n"
        "ndtput -c [-z bytes] [-q depth] [-t seconds | -i messages] [-w warmup] [-n qps]\n"
        "          [-T] [-d] [-b] [-r] [-u seconds] <server-ip>[:port]\n"
        "  -s, --server          Listen for client-defined workloads.\n"
        "  -1, --one-shot        Exit after one server workload.\n"
        "  -c, --client          Connect and define the workload.\n"
        "  -z, --size N          Message size in bytes (default: %lu, min: %lu, max: %lu).\n"
        "  -q, --depth N         Outstanding sends per QP (default: %lu, max: %lu, bounded\n"
        "                        by the adapter queue depths).\n"
        "  -t, --time N          Measured streaming time in seconds (default: %lu).\n"
        "  -i, --iterations N    Stream N measured messages per QP and direction instead of\n"
        "                        running for a fixed time.\n"
        "  -w, --warmup N        Warm-up seconds with -t (default: %lu) or warm-up messages\n"
        "                        with -i (default: %lu); excluded from the results.\n"
        "  -n, --qps N           Queue pairs (one connection and CQ each), 1-%lu (default: %lu).\n"
        "  -T, --thread-per-qp   Drive each QP from its own thread (default: one thread\n"
        "                        polls every QP round-robin).\n"
        "  -d, --bidirectional   Stream in both directions on every QP.\n"
        "  -b, --blocking        Wait on CQ notifications instead of busy polling.\n"
        "  -I, --iocp            Blocking mode that collects CQ notifications from an I/O\n"
        "                        completion port bound to the adapter's overlapped file\n"
        "                        (not with -T).\n"
        "  -r, --realtime        Use real-time process priority and time-critical\n"
        "                        priority for the data-path threads.\n"
        "  -u, --update-seconds N\n"
        "                        Print interval throughput every N seconds.\n"
        "  -h, --help            Show this help.\n"
        "Default port: %hu. The server mirrors the client's workload.\n"
        "MB/s and Gb/s are decimal; CPU utilization is a percentage of one core.\n",
        c_defaultMessageSize,
        c_minMessageSize,
        c_maxMessageSize,
        c_defaultDepth,
        c_maxDepth,
        c_defaultSeconds,
        c_defaultWarmupSeconds,
        c_defaultWarmupMessages,
        c_maxQueuePairs,
        c_defaultQueuePairs,
        c_defaultPort);
}

[[nodiscard]]
static
bool
ParseUnsigned(
    _In_z_ const WCHAR *text,
    _Out_ ULONG &value)
{
    value = 0;
    WCHAR *end = nullptr;
    const unsigned long long parsedValue = _wcstoui64(
        text,
        &end,
        10);
    if (end == text || *end != L'\0' || parsedValue > ULONG_MAX)
    {
        return false;
    }

    value = static_cast<ULONG>(parsedValue);
    return true;
}

static
bool
IsArgument(
    _In_z_ const WCHAR *argument,
    _In_z_ const WCHAR *shortName,
    _In_z_ const WCHAR *longName)
{
    return wcscmp(
               argument,
               shortName) == 0 ||
           wcscmp(
               argument,
               longName) == 0;
}

[[nodiscard]]
static
bool
ParseValue(
    int argumentCount,
    _In_reads_(argumentCount) WCHAR **arguments,
    _Inout_ int &index,
    _Out_ ULONG &value)
{
    value = 0;
    ++index;
    return index < argumentCount && ParseUnsigned(
                                        arguments[index],
                                        value);
}

[[nodiscard]]
static
bool
ParseOptions(
    int argumentCount,
    _In_reads_(argumentCount) WCHAR **arguments,
    _Inout_ Options &options)
{
    const WCHAR *addressText = nullptr;
    bool workloadOptionSpecified = false;
    bool timeSpecified = false;
    bool warmupSpecified = false;

    for (int index = 1; index < argumentCount; ++index)
    {
        const WCHAR *argument = arguments[index];
        bool valid = true;
        if (IsArgument(
                argument,
                L"-s",
                L"--server"))
        {
            options.server = true;
        }
        else if (IsArgument(
                     argument,
                     L"-c",
                     L"--client"))
        {
            options.client = true;
        }
        else if (IsArgument(
                     argument,
                     L"-1",
                     L"--one-shot"))
        {
            options.oneShot = true;
        }
        else if (IsArgument(
                     argument,
                     L"-d",
                     L"--bidirectional"))
        {
            options.bidirectional = true;
            workloadOptionSpecified = true;
        }
        else if (IsArgument(
                     argument,
                     L"-b",
                     L"--blocking"))
        {
            options.blocking = true;
            workloadOptionSpecified = true;
        }
        else if (IsArgument(
                     argument,
                     L"-I",
                     L"--iocp"))
        {
            options.completionPort = true;
            options.blocking = true;
            workloadOptionSpecified = true;
        }
        else if (IsArgument(
                     argument,
                     L"-r",
                     L"--realtime"))
        {
            options.realtime = true;
            workloadOptionSpecified = true;
        }
        else if (IsArgument(
                     argument,
                     L"-T",
                     L"--thread-per-qp"))
        {
            options.threadPerQueuePair = true;
            workloadOptionSpecified = true;
        }
        else if (IsArgument(
                     argument,
                     L"-z",
                     L"--size"))
        {
            workloadOptionSpecified = true;
            valid = ParseValue(
                argumentCount,
                arguments,
                index,
                options.messageSize);
        }
        else if (IsArgument(
                     argument,
                     L"-q",
                     L"--depth"))
        {
            workloadOptionSpecified = true;
            valid = ParseValue(
                argumentCount,
                arguments,
                index,
                options.depth);
        }
        else if (IsArgument(
                     argument,
                     L"-t",
                     L"--time"))
        {
            workloadOptionSpecified = true;
            timeSpecified = true;
            valid = ParseValue(
                argumentCount,
                arguments,
                index,
                options.seconds);
        }
        else if (IsArgument(
                     argument,
                     L"-i",
                     L"--iterations"))
        {
            workloadOptionSpecified = true;
            options.iterationMode = true;
            valid = ParseValue(
                argumentCount,
                arguments,
                index,
                options.iterations);
        }
        else if (IsArgument(
                     argument,
                     L"-w",
                     L"--warmup"))
        {
            workloadOptionSpecified = true;
            warmupSpecified = true;
            valid = ParseValue(
                argumentCount,
                arguments,
                index,
                options.warmup);
        }
        else if (IsArgument(
                     argument,
                     L"-n",
                     L"--qps"))
        {
            workloadOptionSpecified = true;
            valid = ParseValue(
                argumentCount,
                arguments,
                index,
                options.queuePairs);
        }
        else if (IsArgument(
                     argument,
                     L"-u",
                     L"--update-seconds"))
        {
            workloadOptionSpecified = true;
            valid = ParseValue(
                argumentCount,
                arguments,
                index,
                options.statisticsUpdateSeconds);
        }
        else if (IsArgument(
                     argument,
                     L"-h",
                     L"--help"))
        {
            ShowUsage();
            ExitProcess(0);
        }
        else if (argument[0] == L'-' || addressText != nullptr)
        {
            valid = false;
        }
        else
        {
            addressText = argument;
        }

        if (!valid)
        {
            return false;
        }
    }

    if (options.server == options.client || addressText == nullptr || (options.server && workloadOptionSpecified) ||
        (options.client && options.oneShot) || (timeSpecified && options.iterationMode))
    {
        return false;
    }

    if (options.iterationMode && !warmupSpecified)
    {
        options.warmup = c_defaultWarmupMessages;
    }

    if (options.client)
    {
        const char *workloadError = ValidateWorkload(options);
        if (workloadError != nullptr)
        {
            std::printf(
                "Invalid workload: %s.\n",
                workloadError);
            return false;
        }
    }

    std::vector<WCHAR> mutableAddressText(
        addressText,
        addressText + wcslen(addressText) + 1);
    INT addressLength = sizeof(options.address);
    if (WSAStringToAddressW(
            mutableAddressText.data(),
            AF_INET,
            nullptr,
            reinterpret_cast<sockaddr *>(&options.address),
            &addressLength) != 0)
    {
        return false;
    }

    if (options.address.sin_addr.S_un.S_addr == 0)
    {
        return false;
    }

    if (options.address.sin_port == 0)
    {
        options.address.sin_port = htons(c_defaultPort);
    }

    return true;
}

[[nodiscard]]
static
HRESULT
ResolveLocalAddress(
    const sockaddr_in &remoteAddress,
    _Out_ sockaddr_in &localAddress)
{
    localAddress = sockaddr_in{};
    const SOCKET socketHandle = WSASocketW(
        AF_INET,
        SOCK_DGRAM,
        IPPROTO_UDP,
        nullptr,
        0,
        0);
    if (socketHandle == INVALID_SOCKET)
    {
        return HRESULT_FROM_WIN32(WSAGetLastError());
    }

    INT result = connect(
        socketHandle,
        reinterpret_cast<const sockaddr *>(&remoteAddress),
        sizeof(remoteAddress));
    if (result == SOCKET_ERROR)
    {
        const HRESULT connectResult = HRESULT_FROM_WIN32(WSAGetLastError());
        closesocket(socketHandle);
        return connectResult;
    }

    INT addressLength = sizeof(localAddress);
    result = getsockname(
        socketHandle,
        reinterpret_cast<sockaddr *>(&localAddress),
        &addressLength);
    const HRESULT addressResult = result == SOCKET_ERROR ? HRESULT_FROM_WIN32(WSAGetLastError()) : S_OK;
    closesocket(socketHandle);
    return addressResult;
}

static
void
PrintWorkload(
    const Options &options)
{
    std::printf(
        "Workload: size=%lu, depth=%lu, qps=%lu, %s=%lu, warmup=%lu %s, bidirectional=%s, "
        "blocking=%s, iocp=%s, thread-per-qp=%s, realtime=%s.\n",
        options.messageSize,
        options.depth,
        options.queuePairs,
        options.iterationMode ? "messages" : "seconds",
        options.iterationMode ? options.iterations : options.seconds,
        options.warmup,
        options.iterationMode ? "messages" : "seconds",
        options.bidirectional ? "yes" : "no",
        options.blocking ? "yes" : "no",
        options.completionPort ? "yes" : "no",
        options.threadPerQueuePair ? "yes" : "no",
        options.realtime ? "yes" : "no");
}

[[nodiscard]]
static
bool
ExecuteServerWorkload(
    const std::vector<wistd::unique_ptr<Flow>> &flows,
    const Options &workloadOptions)
{
    const DWORD originalProcessPriorityClass = GetPriorityClass(GetCurrentProcess());
    if (originalProcessPriorityClass == 0)
    {
        std::printf(
            "Failed to query server process priority: %lu.\n",
            GetLastError());
        return false;
    }

    const int originalThreadPriority = GetThreadPriority(GetCurrentThread());
    if (originalThreadPriority == THREAD_PRIORITY_ERROR_RETURN)
    {
        std::printf(
            "Failed to query server thread priority: %lu.\n",
            GetLastError());
        return false;
    }

    if (workloadOptions.realtime && EnableRealtimePriority() != ERROR_SUCCESS)
    {
        RestorePriority(
            originalProcessPriorityClass,
            originalThreadPriority);
        return false;
    }

    const bool workloadResult = RunDataPhase(
        flows,
        workloadOptions,
        false);

    if (workloadOptions.realtime)
    {
        RestorePriority(
            originalProcessPriorityClass,
            originalThreadPriority);
    }

    return workloadResult;
}

enum class ServerWorkloadResult
{
    Completed,
    Rejected,
    Fatal,
};

// Accepts QP 0 with queues sized for the largest workload, learns the workload over the
// control exchange, then sizes and accepts the remaining QPs to match it.
static
ServerWorkloadResult
RunServerWorkload(
    const Options &serverOptions)
{
    Adapter adapter;
    HRESULT result = adapter.Initialize(serverOptions.address);
    if (FAILED(result))
    {
        std::printf(
            "Failed to open the NetworkDirect adapter: 0x%08lx.\n",
            result);
        return ServerWorkloadResult::Fatal;
    }

    wil::com_ptr_nothrow<IND2Listener> listener;
    result = CreateListener(
        adapter,
        serverOptions.address,
        listener);
    if (FAILED(result))
    {
        std::printf(
            "Failed to create the listener: 0x%08lx.\n",
            result);
        return ServerWorkloadResult::Fatal;
    }

    const ND2_ADAPTER_INFO &info = adapter.Info();
    const ULONG initiatorCapacity = std::min(
        c_maxDepth + 1,
        info.MaxInitiatorQueueDepth);
    const ULONG receiveCapacity = std::min(
        c_maxDepth + c_receiveMargin + 1,
        info.MaxReceiveQueueDepth);
    const ULONG completionCapacity = std::min(
        initiatorCapacity + receiveCapacity,
        adapter.CompletionQueueLimit());

    FlowSet flows;
    result = flows.Add(
        adapter,
        initiatorCapacity,
        receiveCapacity,
        completionCapacity);
    if (FAILED(result))
    {
        std::printf(
            "QP 0 initialization failed: 0x%08lx.\n",
            result);
        return ServerWorkloadResult::Fatal;
    }

    Flow &primary = flows[0];
    result = primary.PostControlReceive();
    if (FAILED(result))
    {
        std::printf(
            "Failed to post the control receive: 0x%08lx.\n",
            result);
        return ServerWorkloadResult::Fatal;
    }

    std::printf(
        "Listening on %u.%u.%u.%u:%hu.\n",
        serverOptions.address.sin_addr.S_un.S_un_b.s_b1,
        serverOptions.address.sin_addr.S_un.S_un_b.s_b2,
        serverOptions.address.sin_addr.S_un.S_un_b.s_b3,
        serverOptions.address.sin_addr.S_un.S_un_b.s_b4,
        ntohs(serverOptions.address.sin_port));
    std::fflush(stdout);

    result = primary.Accept(
        listener.get(),
        INFINITE);
    if (FAILED(result))
    {
        std::printf(
            "Failed to accept QP 0: 0x%08lx.\n",
            result);
        return ServerWorkloadResult::Rejected;
    }

    ControlMessage request{};
    if (!primary.ReceiveControl(
            ControlType::Workload,
            request))
    {
        return ServerWorkloadResult::Rejected;
    }

    Options workloadOptions{};
    workloadOptions.address = serverOptions.address;
    const char *workloadError = FromWorkloadParameters(
                                    request.parameters,
                                    workloadOptions)
                                    ? ValidateWorkload(workloadOptions)
                                    : "unknown workload flags";
    const FlowLayout layout = MakeFlowLayout(
        workloadOptions,
        false);
    if (workloadError == nullptr)
    {
        workloadError = adapter.ValidateLayout(layout);
    }

    HRESULT status = S_OK;
    if (workloadError != nullptr)
    {
        std::printf(
            "Rejected the workload: %s.\n",
            workloadError);
        status = E_INVALIDARG;
    }
    else
    {
        status = primary.PrepareData(layout);
        for (ULONG index = 1; SUCCEEDED(status) && index < workloadOptions.queuePairs; ++index)
        {
            status = flows.Add(
                adapter,
                layout.sendSlots,
                layout.receiveSlots,
                CompletionDepth(layout));
            if (SUCCEEDED(status))
            {
                status = flows[index].PrepareData(layout);
            }

            if (SUCCEEDED(status))
            {
                status = flows[index].PostDataReceives();
            }
        }

        if (FAILED(status))
        {
            std::printf(
                "Failed to prepare the queue pairs: 0x%08lx.\n",
                status);
        }
    }

    ControlMessage response{};
    response.header.type = ControlType::WorkloadReady;
    response.header.status = status;
    if (!primary.SendControl(response) || FAILED(status))
    {
        return ServerWorkloadResult::Rejected;
    }

    // QP 0 data receives are posted only after the control exchange completes so that no
    // control message can land in a data-sized receive. The client's first sends simply
    // wait until these receives are posted.
    result = primary.PostDataReceives();
    if (FAILED(result))
    {
        std::printf(
            "Failed to post receives on QP 0: 0x%08lx.\n",
            result);
        return ServerWorkloadResult::Rejected;
    }

    for (ULONG index = 1; index < workloadOptions.queuePairs; ++index)
    {
        result = flows[index].Accept(
            listener.get(),
            c_secondaryAcceptTimeoutMilliseconds);
        if (FAILED(result))
        {
            std::printf(
                "Failed to accept QP %lu: 0x%08lx.\n",
                index,
                result);
            return ServerWorkloadResult::Rejected;
        }
    }

    std::printf(
        "Connected %lu queue pair(s).\n",
        workloadOptions.queuePairs);
    PrintWorkload(workloadOptions);
    std::fflush(stdout);

    return ExecuteServerWorkload(
               flows.Flows(),
               workloadOptions)
               ? ServerWorkloadResult::Completed
               : ServerWorkloadResult::Rejected;
}

[[nodiscard]]
static
bool
RunServer(
    const Options &options)
{
    for (;;)
    {
        const ServerWorkloadResult result = RunServerWorkload(options);

        if (result == ServerWorkloadResult::Fatal)
        {
            return false;
        }

        if (result != ServerWorkloadResult::Completed)
        {
            std::printf("Workload failed; waiting for the next client.\n");
        }

        if (options.oneShot)
        {
            return result == ServerWorkloadResult::Completed;
        }

        Sleep(c_serverWorkloadCooldownMilliseconds);
    }
}

[[nodiscard]]
static
bool
RunClient(
    const Options &options)
{
    sockaddr_in localAddress{};
    HRESULT result = ResolveLocalAddress(
        options.address,
        localAddress);
    if (FAILED(result))
    {
        std::printf(
            "Failed to resolve the local address: 0x%08lx.\n",
            result);
        return false;
    }

    Adapter adapter;
    result = adapter.Initialize(localAddress);
    if (FAILED(result))
    {
        std::printf(
            "Failed to open the NetworkDirect adapter: 0x%08lx.\n",
            result);
        return false;
    }

    const FlowLayout layout = MakeFlowLayout(
        options,
        true);
    const char *layoutError = adapter.ValidateLayout(layout);
    if (layoutError != nullptr)
    {
        std::printf(
            "Invalid workload: %s.\n",
            layoutError);
        return false;
    }

    FlowSet flows;
    for (ULONG index = 0; index < options.queuePairs; ++index)
    {
        result = flows.Add(
            adapter,
            layout.sendSlots,
            layout.receiveSlots,
            CompletionDepth(layout));
        if (SUCCEEDED(result))
        {
            result = flows[index].PrepareData(layout);
        }

        if (FAILED(result))
        {
            std::printf(
                "QP %lu initialization failed: 0x%08lx.\n",
                index,
                result);
            return false;
        }
    }

    Flow &primary = flows[0];
    result = primary.PostControlReceive();
    if (SUCCEEDED(result))
    {
        result = primary.Connect(
            localAddress,
            options.address);
    }

    if (FAILED(result))
    {
        std::printf(
            "Failed to connect QP 0: 0x%08lx.\n",
            result);
        return false;
    }

    ControlMessage request{};
    request.header.type = ControlType::Workload;
    request.parameters = ToWorkloadParameters(options);
    ControlMessage response{};
    if (!primary.SendControl(request) || !primary.ReceiveControl(
                                             ControlType::WorkloadReady,
                                             response))
    {
        std::printf("Failed to exchange the workload configuration.\n");
        return false;
    }

    if (FAILED(response.header.status))
    {
        std::printf(
            "The server rejected the workload: 0x%08lx.\n",
            response.header.status);
        return false;
    }

    for (ULONG index = 0; index < options.queuePairs; ++index)
    {
        result = flows[index].PostDataReceives();
        if (SUCCEEDED(result) && index != 0)
        {
            result = flows[index].Connect(
                localAddress,
                options.address);
        }

        if (FAILED(result))
        {
            std::printf(
                "Failed to connect QP %lu: 0x%08lx.\n",
                index,
                result);
            return false;
        }
    }

    std::printf(
        "Connected %lu queue pair(s).\n",
        options.queuePairs);
    PrintWorkload(options);
    std::fflush(stdout);

    return RunDataPhase(
        flows.Flows(),
        options,
        true);
}

int __cdecl
wmain(
    int argumentCount,
    WCHAR **arguments)
{
    WSADATA winsockData{};
    const INT result = WSAStartup(
        MAKEWORD(
            2,
            2),
        &winsockData);
    if (result != 0)
    {
        std::printf(
            "WSAStartup failed: %d.\n",
            result);
        return result;
    }

    Options options{};
    if (!ParseOptions(
            argumentCount,
            arguments,
            options))
    {
        ShowUsage();
        WSACleanup();
        return ERROR_INVALID_PARAMETER;
    }

    if (options.client && options.realtime)
    {
        const DWORD priorityError = EnableRealtimePriority();
        if (priorityError != ERROR_SUCCESS)
        {
            WSACleanup();
            return static_cast<int>(priorityError);
        }
    }

    const bool testResult = options.server ? RunServer(options) : RunClient(options);
    WSACleanup();

    if (!testResult)
    {
        std::printf("ndtput stopped before completing the run.\n");
        return ERROR_CONNECTION_ABORTED;
    }

    return 0;
}
