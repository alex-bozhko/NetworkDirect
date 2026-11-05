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
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

#include <wil/com.h>
#include <wil/resource.h>

static constexpr USHORT c_defaultPort = 54324;
static constexpr ULONG c_defaultIterations = 1000;
static constexpr ULONG c_defaultWarmupIterations = 0;
static constexpr ULONG c_defaultMessageSize = 64;
static constexpr ULONG c_maxMessageSize = 4 * 1024 * 1024;
static constexpr ULONG c_queueDepth = 16;
static constexpr DWORD c_serverWorkloadCooldownMilliseconds = 1000;
static constexpr ULONG c_protocolMagic = 0x474E504E;
static constexpr ULONG c_protocolVersion = 6;
static constexpr ULONG c_flowCount = 2;
static constexpr ULONG c_receiveSlotCount = 2;
static constexpr ULONG c_unidirectionalReceiveSlotCount = 1;
static constexpr ULONG c_primaryReceiveSlot = 0;
static constexpr ULONG c_forwardFlow = 0;
static constexpr ULONG c_reverseFlow = 1;
static constexpr size_t c_pageSize = 4096;
static constexpr double c_p50Quantile = 0.50;
static constexpr double c_p90Quantile = 0.90;
static constexpr double c_p99Quantile = 0.99;
static constexpr double c_p999Quantile = 0.999;
static constexpr double c_microsecondsPerSecond = 1000000.0;
static constexpr double c_bytesPerMegabyte = 1000000.0;
static constexpr double c_bitsPerByte = 8.0;
static constexpr double c_bitsPerGigabit = 1000000000.0;
static constexpr double c_messagesPerExchange = 2.0;

static constexpr
size_t
AlignUp(
    size_t value,
    size_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

enum class MessageType : ULONG
{
    Ping = 1,
    Done = 2,
    DoneAck = 3,
    BidirectionalReady = 4,
    Workload = 5,
    ReversePing = 6,
    ReverseDone = 7,
    ReverseDoneAck = 8,
    WorkloadReady = 9,
};

struct MessageHeader
{
    ULONG magic = c_protocolMagic;
    ULONG version = c_protocolVersion;
    MessageType type = MessageType::Ping;
    ULONG sequence = 0;
    ULONG payloadLength = 0;
};

static constexpr ULONG c_workloadFlagBidirectional = 0x1;
static constexpr ULONG c_workloadFlagBlocking = 0x2;
static constexpr ULONG c_workloadFlagRealtime = 0x4;
static constexpr ULONG c_validWorkloadFlags = c_workloadFlagBidirectional | c_workloadFlagBlocking | c_workloadFlagRealtime;

struct WorkloadParameters
{
    ULONG iterations = 0;
    ULONG warmupIterations = 0;
    ULONG messageSize = 0;
    ULONG statisticsUpdateSeconds = 0;
    ULONG outlierThresholdMicroseconds = 0;
    ULONG flags = 0;
};

struct WorkloadMessage
{
    MessageHeader header{};
    WorkloadParameters parameters{};
};

class RttStatistics
{
public:

    void
    Record(
        LONGLONG startTicks,
        LONGLONG endTicks,
        LONGLONG ticksPerSecond)
    {
        const LONGLONG elapsedTicks = endTicks - startTicks;
        const double microseconds =
            static_cast<double>(elapsedTicks) * c_microsecondsPerSecond / static_cast<double>(ticksPerSecond);

        if (m_count == 0)
        {
            m_measurementStartTicks = startTicks;
            m_ticksPerSecond = ticksPerSecond;
        }

        if (m_count == 0 || microseconds < m_minimumMicroseconds)
        {
            m_minimumMicroseconds = microseconds;
        }
        if (m_count == 0 || microseconds > m_maximumMicroseconds)
        {
            m_maximumMicroseconds = microseconds;
        }

        ++m_count;
        m_measurementEndTicks = endTicks;
        m_totalMicroseconds += microseconds;
        m_totalSquaredMicroseconds += microseconds * microseconds;
        m_samples.push_back(microseconds);
    }

    void
    Reset()
    {
        m_count = 0;
        m_minimumMicroseconds = 0.0;
        m_maximumMicroseconds = 0.0;
        m_totalMicroseconds = 0.0;
        m_totalSquaredMicroseconds = 0.0;
        m_cycleNumber = 0;
        m_measurementStartTicks = 0;
        m_measurementEndTicks = 0;
        m_ticksPerSecond = 0;
        m_samples.clear();
    }

    void
    Print(
        _In_z_ const char *directionName,
        ULONG messageSize) const
    {
        if (!HasSamples())
        {
            return;
        }

        const Summary summary = CalculateSummary();
        const double aggregateBytesPerSecond =
            summary.exchangesPerSecond * static_cast<double>(messageSize) * c_messagesPerExchange;
        const double megabytesPerSecond = aggregateBytesPerSecond / c_bytesPerMegabyte;
        const double gigabitsPerSecond = aggregateBytesPerSecond * c_bitsPerByte / c_bitsPerGigabit;
        std::printf(
            "\n"
            "Total RTT summary (%s)\n"
            "----------------------------------------\n"
            "  Exchanges      : %lu\n"
            "  Message size   : %lu bytes\n"
            "  Duration       : %.3f seconds\n"
            "  Exchange rate  : %.2f exchanges/sec\n"
            "  Throughput     : %.3f MB/s\n"
            "  Data rate      : %.6f Gbps\n"
            "                   (request + echo message bytes)\n"
            "\n"
            "  Latency (us)\n"
            "    Minimum      : %.3f\n"
            "    Average      : %.3f\n"
            "    P50          : %.3f\n"
            "    P90          : %.3f\n"
            "    P99          : %.3f\n"
            "    P99.9        : %.3f\n"
            "    Maximum      : %.3f\n"
            "    Std deviation: %.3f\n",
            directionName,
            m_count,
            messageSize,
            summary.elapsedSeconds,
            summary.exchangesPerSecond,
            megabytesPerSecond,
            gigabitsPerSecond,
            m_minimumMicroseconds,
            summary.averageMicroseconds,
            summary.p50Microseconds,
            summary.p90Microseconds,
            summary.p99Microseconds,
            summary.p999Microseconds,
            m_maximumMicroseconds,
            summary.standardDeviationMicroseconds);
    }

    bool
    HasSamples() const
    {
        return m_count != 0;
    }

    void
    PrintCycle(
        _In_z_ const char *directionName,
        const RttStatistics &cycleStatistics)
    {
        if (!cycleStatistics.HasSamples())
        {
            return;
        }

        ++m_cycleNumber;
        const Summary summary = cycleStatistics.CalculateSummary();
        std::printf(
            "Cycle %lu | %s | n=%lu | avg=%.3f us | p50=%.3f | "
            "p90=%.3f | p99=%.3f | p99.9=%.3f | max=%.3f | "
            "rate=%.2f/s\n",
            m_cycleNumber,
            directionName,
            cycleStatistics.m_count,
            summary.averageMicroseconds,
            summary.p50Microseconds,
            summary.p90Microseconds,
            summary.p99Microseconds,
            summary.p999Microseconds,
            cycleStatistics.m_maximumMicroseconds,
            summary.exchangesPerSecond);
        std::fflush(stdout);
    }

private:

    struct Summary
    {
        double averageMicroseconds = 0.0;
        double standardDeviationMicroseconds = 0.0;
        double elapsedSeconds = 0.0;
        double exchangesPerSecond = 0.0;
        double p50Microseconds = 0.0;
        double p90Microseconds = 0.0;
        double p99Microseconds = 0.0;
        double p999Microseconds = 0.0;
    };

    Summary
    CalculateSummary() const
    {
        Summary summary{};
        if (!HasSamples())
        {
            return summary;
        }

        summary.averageMicroseconds = m_totalMicroseconds / static_cast<double>(m_count);
        const double variance = std::max(
            0.0,
            m_totalSquaredMicroseconds / static_cast<double>(m_count) -
                summary.averageMicroseconds * summary.averageMicroseconds);
        summary.standardDeviationMicroseconds = std::sqrt(variance);
        if (m_ticksPerSecond > 0 && m_measurementEndTicks > m_measurementStartTicks)
        {
            summary.elapsedSeconds =
                static_cast<double>(m_measurementEndTicks - m_measurementStartTicks) / static_cast<double>(m_ticksPerSecond);
            summary.exchangesPerSecond = static_cast<double>(m_count) / summary.elapsedSeconds;
        }

        CalculatePercentiles(
            summary.p50Microseconds,
            summary.p90Microseconds,
            summary.p99Microseconds,
            summary.p999Microseconds);
        return summary;
    }

    void
    CalculatePercentiles(
        double &p50Microseconds,
        double &p90Microseconds,
        double &p99Microseconds,
        double &p999Microseconds) const
    {
        std::vector<double> samples = m_samples;
        const size_t sampleCount = samples.size();
        if (sampleCount == 0)
        {
            p50Microseconds = 0.0;
            p90Microseconds = 0.0;
            p99Microseconds = 0.0;
            p999Microseconds = 0.0;
            return;
        }

        const size_t p50Index = static_cast<size_t>(std::ceil(c_p50Quantile * static_cast<double>(sampleCount))) - 1;
        const size_t p90Index = static_cast<size_t>(std::ceil(c_p90Quantile * static_cast<double>(sampleCount))) - 1;
        const size_t p99Index = static_cast<size_t>(std::ceil(c_p99Quantile * static_cast<double>(sampleCount))) - 1;
        const size_t p999Index = static_cast<size_t>(std::ceil(c_p999Quantile * static_cast<double>(sampleCount))) - 1;

        std::nth_element(
            samples.begin(),
            samples.begin() + p50Index,
            samples.end());
        p50Microseconds = samples[p50Index];

        std::nth_element(
            samples.begin(),
            samples.begin() + p90Index,
            samples.end());
        p90Microseconds = samples[p90Index];

        std::nth_element(
            samples.begin(),
            samples.begin() + p99Index,
            samples.end());
        p99Microseconds = samples[p99Index];

        std::nth_element(
            samples.begin(),
            samples.begin() + p999Index,
            samples.end());
        p999Microseconds = samples[p999Index];
    }

    ULONG m_count = 0;
    double m_minimumMicroseconds = 0.0;
    double m_maximumMicroseconds = 0.0;
    double m_totalMicroseconds = 0.0;
    double m_totalSquaredMicroseconds = 0.0;
    ULONG m_cycleNumber = 0;
    LONGLONG m_measurementStartTicks = 0;
    LONGLONG m_measurementEndTicks = 0;
    LONGLONG m_ticksPerSecond = 0;
    std::vector<double> m_samples;
};

struct Options
{
    bool server = false;
    bool client = false;
    bool bidirectional = false;
    bool blocking = false;
    bool realtime = false;
    bool oneShot = false;
    ULONG iterations = c_defaultIterations;
    ULONG warmupIterations = c_defaultWarmupIterations;
    ULONG messageSize = c_defaultMessageSize;
    ULONG statisticsUpdateSeconds = 0;
    ULONG outlierThresholdMicroseconds = 0;
    sockaddr_in address{};
};

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

    static
    HRESULT
    OpenForAddress(
        _In_ const sockaddr_in &address,
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
            if (provider == nullptr)
            {
                return E_OUTOFMEMORY;
            }

            HRESULT result = provider->Load(candidate.ProviderId);
            if (FAILED(result))
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

    HRESULT
    OpenAdapter(
        _In_ const sockaddr_in &address,
        _Out_ wil::com_ptr_nothrow<IND2Adapter> &adapter) const
    {
        UINT64 adapterId = 0;
        HRESULT result = m_provider->ResolveAddress(
            reinterpret_cast<const sockaddr *>(&address),
            sizeof(address),
            &adapterId);
        if (FAILED(result))
        {
            return result;
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
        _In_ const WSAPROTOCOL_INFOW &protocolInfo)
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

    HRESULT
    Load(
        _In_ const GUID &providerId)
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

        DWORD expandedLength = ExpandEnvironmentStringsW(
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
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (m_module == nullptr)
        {
            return HRESULT_FROM_WIN32(GetLastError());
        }

        DllGetClassObjectFn getClassObject = reinterpret_cast<DllGetClassObjectFn>(GetProcAddress(
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
        _In_ const sockaddr_in &address) const
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
    wil::com_ptr_nothrow<IND2Provider> m_provider;
};

class Connection
{
public:

    Connection(
        _In_ const sockaddr_in &localAddress,
        bool blocking,
        ULONG messageSize,
        ULONG messageLength)
        : m_localAddress(localAddress)
        , m_blocking(blocking)
        , m_messageCapacity(
              std::max<ULONG>(
                  messageSize,
                  sizeof(WorkloadMessage)))
        , m_messageLength(messageLength)
        , m_bufferStride(AlignUp(
              m_messageCapacity,
              c_pageSize))
        , m_bufferSize(m_bufferStride * (c_flowCount + c_receiveSlotCount))
        , m_buffer(
              static_cast<BYTE *>(VirtualAlloc(
                  nullptr,
                  m_bufferSize,
                  MEM_COMMIT | MEM_RESERVE,
                  PAGE_READWRITE)))
    {
    }

    ~Connection()
    {
        Shutdown();
    }

    Connection(
        const Connection &) = delete;
    Connection &
    operator=(
        const Connection &) = delete;

    HRESULT
    Initialize()
    {
        if (m_buffer.get() == nullptr)
        {
            return E_OUTOFMEMORY;
        }

        HRESULT result = LoadedProvider::OpenForAddress(
            m_localAddress,
            m_provider);
        if (FAILED(result))
        {
            return result;
        }

        result = m_provider->OpenAdapter(
            m_localAddress,
            m_adapter);
        if (FAILED(result))
        {
            return result;
        }

        m_operationEvent.reset(CreateEventW(
            nullptr,
            FALSE,
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
        if (m_operationEvent.get() == nullptr || m_cqEvent.get() == nullptr || m_disconnectEvent.get() == nullptr)
        {
            return HRESULT_FROM_WIN32(GetLastError());
        }

        m_operationOverlapped.hEvent = m_operationEvent.get();
        m_cqOverlapped.hEvent = m_cqEvent.get();
        m_disconnectOverlapped.hEvent = m_disconnectEvent.get();

        HANDLE adapterFile = nullptr;
        result = m_adapter->CreateOverlappedFile(&adapterFile);
        if (FAILED(result))
        {
            return result;
        }
        m_adapterFile.reset(adapterFile);

        ND2_ADAPTER_INFO adapterInfo{};
        adapterInfo.InfoVersion = ND_VERSION_2;
        ULONG adapterInfoLength = sizeof(adapterInfo);
        result = m_adapter->Query(
            &adapterInfo,
            &adapterInfoLength);
        if (FAILED(result))
        {
            return result;
        }

        m_inlineThreshold = adapterInfo.InlineRequestThreshold;
        ULONG queueDepth = std::min(
            c_queueDepth,
            std::min(
                adapterInfo.MaxCompletionQueueDepth,
                std::min(
                    adapterInfo.MaxReceiveQueueDepth,
                    adapterInfo.MaxInitiatorQueueDepth)));
        if (queueDepth < 2)
        {
            return ND_INSUFFICIENT_RESOURCES;
        }

        result = m_adapter->CreateCompletionQueue(
            IID_IND2CompletionQueue,
            m_adapterFile.get(),
            queueDepth,
            0,
            0,
            m_completionQueue.put_void());
        if (FAILED(result))
        {
            return result;
        }

        result = m_adapter->CreateMemoryRegion(
            IID_IND2MemoryRegion,
            m_adapterFile.get(),
            m_memoryRegion.put_void());
        if (FAILED(result))
        {
            return result;
        }

        result = CompleteOverlapped(
            m_memoryRegion.get(),
            m_memoryRegion->Register(
                m_buffer.get(),
                m_bufferSize,
                ND_MR_FLAG_ALLOW_LOCAL_WRITE,
                &m_operationOverlapped));
        if (FAILED(result))
        {
            return result;
        }
        m_memoryRegistered = true;

        result = m_adapter->CreateConnector(
            IID_IND2Connector,
            m_adapterFile.get(),
            m_connector.put_void());
        if (FAILED(result))
        {
            return result;
        }

        return m_adapter->CreateQueuePair(
            IID_IND2QueuePair,
            m_completionQueue.get(),
            m_completionQueue.get(),
            nullptr,
            queueDepth,
            queueDepth,
            1,
            1,
            adapterInfo.MaxInlineDataSize,
            m_queuePair.put_void());
    }

    HRESULT
    PrepareListener()
    {
        HRESULT result = m_adapter->CreateListener(
            IID_IND2Listener,
            m_adapterFile.get(),
            m_listener.put_void());
        if (FAILED(result))
        {
            return result;
        }

        result = m_listener->Bind(
            reinterpret_cast<const sockaddr *>(&m_localAddress),
            sizeof(m_localAddress));
        if (FAILED(result))
        {
            return result;
        }

        return m_listener->Listen(0);
    }

    HRESULT
    AcceptPrepared()
    {
        HRESULT result = CompleteOverlapped(
            m_listener.get(),
            m_listener->GetConnectionRequest(
                m_connector.get(),
                &m_operationOverlapped));
        if (FAILED(result))
        {
            return result;
        }

        result = PostReceive(c_primaryReceiveSlot);
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

    HRESULT
    Connect(
        _In_ const sockaddr_in &remoteAddress)
    {
        sockaddr_in bindAddress = m_localAddress;
        bindAddress.sin_port = 0;
        HRESULT result = m_connector->Bind(
            reinterpret_cast<const sockaddr *>(&bindAddress),
            sizeof(bindAddress));
        if (FAILED(result))
        {
            return result;
        }

        result = PostReceive(c_primaryReceiveSlot);
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

    bool
    SendWorkload(
        _In_ const Options &options)
    {
        WorkloadMessage *workload = reinterpret_cast<WorkloadMessage *>(SendBuffer(c_forwardFlow));
        workload->header.magic = c_protocolMagic;
        workload->header.version = c_protocolVersion;
        workload->header.type = MessageType::Workload;
        workload->header.sequence = 0;
        workload->header.payloadLength = sizeof(WorkloadParameters);
        workload->parameters.iterations = options.iterations;
        workload->parameters.warmupIterations = options.warmupIterations;
        workload->parameters.messageSize = options.messageSize;
        workload->parameters.statisticsUpdateSeconds = options.statisticsUpdateSeconds;
        workload->parameters.outlierThresholdMicroseconds = options.outlierThresholdMicroseconds;
        workload->parameters.flags = (options.bidirectional ? c_workloadFlagBidirectional : 0) |
                                     (options.blocking ? c_workloadFlagBlocking : 0) |
                                     (options.realtime ? c_workloadFlagRealtime : 0);

        return SendCurrentMessage(
            c_forwardFlow,
            sizeof(WorkloadMessage));
    }

    HRESULT
    EnableBidirectionalReceiveSlot()
    {
        if (m_activeReceiveSlotCount == c_receiveSlotCount)
        {
            return S_OK;
        }

        if (m_activeReceiveSlotCount != c_unidirectionalReceiveSlotCount)
        {
            return E_UNEXPECTED;
        }

        const HRESULT result = PostReceive(m_activeReceiveSlotCount);
        if (SUCCEEDED(result))
        {
            ++m_activeReceiveSlotCount;
        }

        return result;
    }

    bool
    ReceiveWorkload(
        _Out_ Options &options)
    {
        ULONG receiveSlot = 0;
        if (!ReceiveCurrentMessage(receiveSlot))
        {
            return false;
        }

        const WorkloadMessage *workload = reinterpret_cast<const WorkloadMessage *>(ReceiveBuffer(receiveSlot));
        if (m_receivedBytes[receiveSlot] != sizeof(WorkloadMessage) || workload->header.magic != c_protocolMagic ||
            workload->header.version != c_protocolVersion || workload->header.type != MessageType::Workload ||
            workload->header.sequence != 0 || workload->header.payloadLength != sizeof(WorkloadParameters) ||
            workload->parameters.iterations == 0 ||
            workload->parameters.warmupIterations > ULONG_MAX - workload->parameters.iterations ||
            workload->parameters.messageSize < sizeof(MessageHeader) || workload->parameters.messageSize > c_maxMessageSize ||
            (workload->parameters.flags & ~c_validWorkloadFlags) != 0)
        {
            std::printf("Received an invalid workload configuration.\n");
            return false;
        }

        options.iterations = workload->parameters.iterations;
        options.warmupIterations = workload->parameters.warmupIterations;
        options.messageSize = workload->parameters.messageSize;
        options.statisticsUpdateSeconds = workload->parameters.statisticsUpdateSeconds;
        options.outlierThresholdMicroseconds = workload->parameters.outlierThresholdMicroseconds;
        options.bidirectional = WI_IsFlagSet(
            workload->parameters.flags,
            c_workloadFlagBidirectional);
        options.blocking = WI_IsFlagSet(
            workload->parameters.flags,
            c_workloadFlagBlocking);
        options.realtime = WI_IsFlagSet(
            workload->parameters.flags,
            c_workloadFlagRealtime);
        m_blocking = options.blocking;
        m_messageLength = options.messageSize;

        if (FAILED(PostReceive(receiveSlot)))
        {
            return false;
        }

        if (options.bidirectional && FAILED(EnableBidirectionalReceiveSlot()))
        {
            return false;
        }

        return SendControl(MessageType::WorkloadReady);
    }

    bool
    SendControl(
        MessageType type)
    {
        FillControl(
            c_forwardFlow,
            type);
        return SendCurrentMessage(
            c_forwardFlow,
            m_messageLength);
    }

    bool
    ReceiveControl(
        MessageType expectedType)
    {
        ULONG receiveSlot = 0;
        if (!ReceiveCurrentMessage(receiveSlot))
        {
            return false;
        }

        const MessageHeader *header = GetReceiveHeader(receiveSlot);
        if (m_receivedBytes[receiveSlot] != m_messageLength || header->magic != c_protocolMagic ||
            header->version != c_protocolVersion || header->type != expectedType || header->sequence != 0 ||
            header->payloadLength != 0)
        {
            std::printf("Received an invalid control message.\n");
            return false;
        }

        return SUCCEEDED(PostReceive(receiveSlot));
    }

    bool
    RunInitiator(
        ULONG iterations,
        ULONG warmupIterations,
        ULONG messageSize,
        ULONG statisticsUpdateSeconds,
        ULONG outlierThresholdMicroseconds,
        _In_z_ const char *directionName)
    {
        LARGE_INTEGER performanceFrequency{};
        if (!QueryPerformanceFrequency(&performanceFrequency))
        {
            std::printf("Failed to query the performance-counter frequency.\n");
            return false;
        }

        const ULONG totalIterations = warmupIterations + iterations;
        LARGE_INTEGER lastStatisticsTime{};
        bool statisticsTimingStarted = false;

        RttStatistics statistics;
        RttStatistics cycleStatistics;
        for (ULONG sequence = 0; sequence < totalIterations; ++sequence)
        {
            const bool measuredSequence = sequence >= warmupIterations;
            const bool collectOutlierData = measuredSequence && outlierThresholdMicroseconds != 0;
            ULONG receiveSlot = 0;
            FillPing(
                c_forwardFlow,
                sequence,
                messageSize);
            LARGE_INTEGER startTime{};
            LARGE_INTEGER sendEndTime{};
            LARGE_INTEGER endTime{};
            FILETIME startUtc{};
            DWORD startProcessor = 0;
            DWORD sendEndProcessor = 0;
            DWORD endProcessor = 0;

            if (collectOutlierData)
            {
                GetSystemTimePreciseAsFileTime(&startUtc);
                QueryPerformanceCounter(&startTime);
                startProcessor = GetCurrentProcessorNumber();
                if (!SendCurrentMessage(
                        c_forwardFlow,
                        messageSize))
                {
                    return false;
                }

                QueryPerformanceCounter(&sendEndTime);
                sendEndProcessor = GetCurrentProcessorNumber();
                if (!ReceiveCurrentMessage(receiveSlot))
                {
                    return false;
                }

                QueryPerformanceCounter(&endTime);
                endProcessor = GetCurrentProcessorNumber();
            }
            else
            {
                QueryPerformanceCounter(&startTime);
                if (!SendCurrentMessage(
                        c_forwardFlow,
                        messageSize) ||
                    !ReceiveCurrentMessage(receiveSlot))
                {
                    return false;
                }
                QueryPerformanceCounter(&endTime);
            }

            if (measuredSequence && !statisticsTimingStarted)
            {
                lastStatisticsTime = startTime;
                statisticsTimingStarted = true;
            }

            const MessageHeader *header = GetReceiveHeader(receiveSlot);
            if (m_receivedBytes[receiveSlot] != messageSize || header->magic != c_protocolMagic ||
                header->version != c_protocolVersion || header->type != MessageType::Ping || header->sequence != sequence ||
                header->payloadLength != messageSize - sizeof(MessageHeader) ||
                !ValidatePayload(
                    receiveSlot,
                    sequence,
                    messageSize))
            {
                std::printf(
                    "Received an invalid echo for sequence %lu.\n",
                    sequence);
                return false;
            }

            if (measuredSequence)
            {
                statistics.Record(
                    startTime.QuadPart,
                    endTime.QuadPart,
                    performanceFrequency.QuadPart);
                cycleStatistics.Record(
                    startTime.QuadPart,
                    endTime.QuadPart,
                    performanceFrequency.QuadPart);

                if (collectOutlierData)
                {
                    const double totalMicroseconds = static_cast<double>(endTime.QuadPart - startTime.QuadPart) *
                                                     c_microsecondsPerSecond / static_cast<double>(performanceFrequency.QuadPart);
                    if (totalMicroseconds >= static_cast<double>(outlierThresholdMicroseconds))
                    {
                        const double sendMicroseconds = static_cast<double>(sendEndTime.QuadPart - startTime.QuadPart) *
                                                        c_microsecondsPerSecond /
                                                        static_cast<double>(performanceFrequency.QuadPart);
                        const double receiveMicroseconds = static_cast<double>(endTime.QuadPart - sendEndTime.QuadPart) *
                                                           c_microsecondsPerSecond /
                                                           static_cast<double>(performanceFrequency.QuadPart);
                        ULARGE_INTEGER startUtcValue{};
                        startUtcValue.LowPart = startUtc.dwLowDateTime;
                        startUtcValue.HighPart = startUtc.dwHighDateTime;
                        std::fprintf(
                            stderr,
                            "RTT outlier: direction=%s sequence=%lu "
                            "utc100ns=%llu qpcStart=%lld qpcEnd=%lld "
                            "total=%.3f us send=%.3f us receive=%.3f us "
                            "cpu=%lu>%lu>%lu\n",
                            directionName,
                            sequence,
                            startUtcValue.QuadPart,
                            startTime.QuadPart,
                            endTime.QuadPart,
                            totalMicroseconds,
                            sendMicroseconds,
                            receiveMicroseconds,
                            startProcessor,
                            sendEndProcessor,
                            endProcessor);
                    }
                }

                if (sequence + 1 < totalIterations && statisticsUpdateSeconds != 0 &&
                    (endTime.QuadPart - lastStatisticsTime.QuadPart) / performanceFrequency.QuadPart >= statisticsUpdateSeconds)
                {
                    statistics.PrintCycle(
                        directionName,
                        cycleStatistics);
                    cycleStatistics.Reset();
                    lastStatisticsTime = endTime;
                }
            }

            if (FAILED(PostReceive(receiveSlot)))
            {
                return false;
            }
        }

        FillControl(
            c_forwardFlow,
            MessageType::Done);
        ULONG receiveSlot = 0;
        if (!SendCurrentMessage(
                c_forwardFlow,
                m_messageLength) ||
            !ReceiveCurrentMessage(receiveSlot))
        {
            return false;
        }

        const MessageHeader *header = GetReceiveHeader(receiveSlot);
        const bool completed = m_receivedBytes[receiveSlot] == m_messageLength && header->magic == c_protocolMagic &&
                               header->version == c_protocolVersion && header->type == MessageType::DoneAck &&
                               header->sequence == 0 && header->payloadLength == 0;
        if (completed)
        {
            if (statisticsUpdateSeconds != 0 && cycleStatistics.HasSamples())
            {
                statistics.PrintCycle(
                    directionName,
                    cycleStatistics);
            }

            statistics.Print(
                directionName,
                messageSize);
        }

        return completed;
    }

    bool
    RunResponder(
        ULONG warmupIterations,
        ULONG outlierThresholdMicroseconds,
        _In_z_ const char *directionName)
    {
        LARGE_INTEGER performanceFrequency{};
        if (outlierThresholdMicroseconds != 0 && !QueryPerformanceFrequency(&performanceFrequency))
        {
            std::printf("Failed to query the performance-counter frequency.\n");
            return false;
        }

        for (;;)
        {
            ULONG receiveSlot = 0;
            LARGE_INTEGER receiveStartTime{};
            LARGE_INTEGER receiveEndTime{};
            DWORD receiveStartProcessor = 0;
            DWORD receiveEndProcessor = 0;
            if (outlierThresholdMicroseconds != 0)
            {
                QueryPerformanceCounter(&receiveStartTime);
                receiveStartProcessor = GetCurrentProcessorNumber();
            }

            if (!ReceiveCurrentMessage(receiveSlot))
            {
                return false;
            }

            if (outlierThresholdMicroseconds != 0)
            {
                QueryPerformanceCounter(&receiveEndTime);
                receiveEndProcessor = GetCurrentProcessorNumber();
            }

            if (m_receivedBytes[receiveSlot] < sizeof(MessageHeader))
            {
                std::printf("Received a truncated protocol header.\n");
                return false;
            }

            const MessageHeader *header = GetReceiveHeader(receiveSlot);
            if (header->magic != c_protocolMagic || header->version != c_protocolVersion)
            {
                std::printf("Received an invalid protocol header.\n");
                return false;
            }

            if (header->type == MessageType::Done)
            {
                if (m_receivedBytes[receiveSlot] != m_messageLength || header->sequence != 0 || header->payloadLength != 0)
                {
                    std::printf("Received an invalid completion message.\n");
                    return false;
                }

                FillControl(
                    c_forwardFlow,
                    MessageType::DoneAck);
                return SendCurrentMessage(
                    c_forwardFlow,
                    m_messageLength);
            }

            if (header->type != MessageType::Ping ||
                header->payloadLength != m_receivedBytes[receiveSlot] - sizeof(MessageHeader))
            {
                std::printf("Received an invalid ping.\n");
                return false;
            }

            const ULONG echoLength = m_receivedBytes[receiveSlot];
            const ULONG sequence = header->sequence;
            std::copy_n(
                ReceiveBuffer(receiveSlot),
                echoLength,
                SendBuffer(c_forwardFlow));
            if (FAILED(PostReceive(receiveSlot)) || !SendCurrentMessage(
                                                        c_forwardFlow,
                                                        echoLength))
            {
                return false;
            }

            if (outlierThresholdMicroseconds != 0 && sequence >= warmupIterations)
            {
                LARGE_INTEGER sendEndTime{};
                QueryPerformanceCounter(&sendEndTime);
                DWORD sendEndProcessor = GetCurrentProcessorNumber();
                const double receiveWaitMicroseconds = static_cast<double>(receiveEndTime.QuadPart - receiveStartTime.QuadPart) *
                                                       c_microsecondsPerSecond /
                                                       static_cast<double>(performanceFrequency.QuadPart);
                const double echoMicroseconds = static_cast<double>(sendEndTime.QuadPart - receiveEndTime.QuadPart) *
                                                c_microsecondsPerSecond / static_cast<double>(performanceFrequency.QuadPart);
                if (receiveWaitMicroseconds >= static_cast<double>(outlierThresholdMicroseconds) ||
                    echoMicroseconds >= static_cast<double>(outlierThresholdMicroseconds))
                {
                    std::fprintf(
                        stderr,
                        "Responder outlier: direction=%s sequence=%lu "
                        "receiveWait=%.3f us echo=%.3f us cpu=%lu>%lu>%lu\n",
                        directionName,
                        sequence,
                        receiveWaitMicroseconds,
                        echoMicroseconds,
                        receiveStartProcessor,
                        receiveEndProcessor,
                        sendEndProcessor);
                }
            }
        }
    }

    bool
    RunBidirectional(
        bool client,
        ULONG iterations,
        ULONG warmupIterations,
        ULONG messageSize,
        ULONG statisticsUpdateSeconds,
        ULONG outlierThresholdMicroseconds)
    {
        LARGE_INTEGER performanceFrequency{};
        if (!QueryPerformanceFrequency(&performanceFrequency))
        {
            std::printf("Failed to query the performance-counter frequency.\n");
            return false;
        }

        const ULONG totalIterations = warmupIterations + iterations;
        const ULONG initiatorFlow = client ? c_forwardFlow : c_reverseFlow;
        const ULONG responderFlow = client ? c_reverseFlow : c_forwardFlow;
        const char *initiatorDirection = client ? "client -> server" : "server -> client";
        const char *responderDirection = client ? "server -> client" : "client -> server";

        LARGE_INTEGER lastStatisticsTime{};

        RttStatistics statistics;
        RttStatistics cycleStatistics;
        ULONG initiatorSequence = 0;
        FILETIME initiatorStartUtc{};
        LARGE_INTEGER initiatorStartTime{};
        LARGE_INTEGER initiatorSendEndTime{};
        DWORD initiatorStartProcessor = 0;
        DWORD initiatorSendEndProcessor = 0;

        if (!StartPing(
                initiatorFlow,
                initiatorSequence,
                messageSize,
                warmupIterations == 0 ? outlierThresholdMicroseconds : 0,
                initiatorStartUtc,
                initiatorStartTime,
                initiatorSendEndTime,
                initiatorStartProcessor,
                initiatorSendEndProcessor))
        {
            return false;
        }

        if (warmupIterations == 0)
        {
            lastStatisticsTime = initiatorStartTime;
        }

        bool initiatorCompleted = false;
        bool responderCompleted = false;
        while (!initiatorCompleted || !responderCompleted)
        {
            ULONG receiveSlot = 0;
            LARGE_INTEGER receiveStartTime{};
            DWORD receiveStartProcessor = 0;
            if (outlierThresholdMicroseconds != 0)
            {
                QueryPerformanceCounter(&receiveStartTime);
                receiveStartProcessor = GetCurrentProcessorNumber();
            }

            if (!ReceiveCurrentMessage(receiveSlot))
            {
                return false;
            }

            LARGE_INTEGER receiveEndTime{};
            QueryPerformanceCounter(&receiveEndTime);
            const DWORD receiveEndProcessor = GetCurrentProcessorNumber();

            if (m_receivedBytes[receiveSlot] < sizeof(MessageHeader))
            {
                std::printf("Received a truncated protocol header.\n");
                return false;
            }

            const MessageHeader *header = GetReceiveHeader(receiveSlot);
            if (header->magic != c_protocolMagic || header->version != c_protocolVersion)
            {
                std::printf("Received an invalid protocol header.\n");
                return false;
            }

            if (header->type == GetPingType(initiatorFlow))
            {
                if (m_receivedBytes[receiveSlot] != messageSize || header->sequence != initiatorSequence ||
                    header->payloadLength != messageSize - sizeof(MessageHeader) ||
                    !ValidatePayload(
                        receiveSlot,
                        initiatorSequence,
                        messageSize))
                {
                    std::printf(
                        "Received an invalid echo for sequence %lu.\n",
                        initiatorSequence);
                    return false;
                }

                const bool measuredSequence = initiatorSequence >= warmupIterations;
                if (measuredSequence)
                {
                    statistics.Record(
                        initiatorStartTime.QuadPart,
                        receiveEndTime.QuadPart,
                        performanceFrequency.QuadPart);
                    cycleStatistics.Record(
                        initiatorStartTime.QuadPart,
                        receiveEndTime.QuadPart,
                        performanceFrequency.QuadPart);

                    if (outlierThresholdMicroseconds != 0)
                    {
                        const double totalMicroseconds =
                            static_cast<double>(receiveEndTime.QuadPart - initiatorStartTime.QuadPart) * c_microsecondsPerSecond /
                            static_cast<double>(performanceFrequency.QuadPart);
                        if (totalMicroseconds >= static_cast<double>(outlierThresholdMicroseconds))
                        {
                            const double sendMicroseconds =
                                static_cast<double>(initiatorSendEndTime.QuadPart - initiatorStartTime.QuadPart) *
                                c_microsecondsPerSecond / static_cast<double>(performanceFrequency.QuadPart);
                            const double receiveMicroseconds =
                                static_cast<double>(receiveEndTime.QuadPart - initiatorSendEndTime.QuadPart) *
                                c_microsecondsPerSecond / static_cast<double>(performanceFrequency.QuadPart);
                            ULARGE_INTEGER startUtcValue{};
                            startUtcValue.LowPart = initiatorStartUtc.dwLowDateTime;
                            startUtcValue.HighPart = initiatorStartUtc.dwHighDateTime;
                            std::fprintf(
                                stderr,
                                "RTT outlier: direction=%s sequence=%lu "
                                "utc100ns=%llu qpcStart=%lld qpcEnd=%lld "
                                "total=%.3f us send=%.3f us receive=%.3f us "
                                "cpu=%lu>%lu>%lu\n",
                                initiatorDirection,
                                initiatorSequence,
                                startUtcValue.QuadPart,
                                initiatorStartTime.QuadPart,
                                receiveEndTime.QuadPart,
                                totalMicroseconds,
                                sendMicroseconds,
                                receiveMicroseconds,
                                initiatorStartProcessor,
                                initiatorSendEndProcessor,
                                receiveEndProcessor);
                        }
                    }

                    if (initiatorSequence + 1 < totalIterations && statisticsUpdateSeconds != 0 &&
                        (receiveEndTime.QuadPart - lastStatisticsTime.QuadPart) / performanceFrequency.QuadPart >=
                            statisticsUpdateSeconds)
                    {
                        statistics.PrintCycle(
                            initiatorDirection,
                            cycleStatistics);
                        cycleStatistics.Reset();
                        lastStatisticsTime = receiveEndTime;
                    }
                }

                ++initiatorSequence;
                if (FAILED(PostReceive(receiveSlot)))
                {
                    return false;
                }

                if (initiatorSequence < totalIterations)
                {
                    const ULONG currentOutlierThresholdMicroseconds =
                        initiatorSequence >= warmupIterations ? outlierThresholdMicroseconds : 0;
                    if (!StartPing(
                            initiatorFlow,
                            initiatorSequence,
                            messageSize,
                            currentOutlierThresholdMicroseconds,
                            initiatorStartUtc,
                            initiatorStartTime,
                            initiatorSendEndTime,
                            initiatorStartProcessor,
                            initiatorSendEndProcessor))
                    {
                        return false;
                    }

                    if (initiatorSequence == warmupIterations)
                    {
                        lastStatisticsTime = initiatorStartTime;
                    }
                }
                else
                {
                    FillControl(
                        initiatorFlow,
                        GetDoneType(initiatorFlow));
                    if (!SendCurrentMessage(
                            initiatorFlow,
                            m_messageLength))
                    {
                        return false;
                    }
                }
            }
            else if (header->type == GetDoneAckType(initiatorFlow))
            {
                if (m_receivedBytes[receiveSlot] != m_messageLength || header->sequence != 0 || header->payloadLength != 0)
                {
                    std::printf("Received an invalid completion acknowledgement.\n");
                    return false;
                }

                initiatorCompleted = true;
                if (!responderCompleted && FAILED(PostReceive(receiveSlot)))
                {
                    return false;
                }

                if (statisticsUpdateSeconds != 0)
                {
                    if (cycleStatistics.HasSamples())
                    {
                        statistics.PrintCycle(
                            initiatorDirection,
                            cycleStatistics);
                    }
                }

                statistics.Print(
                    initiatorDirection,
                    messageSize);
            }
            else if (header->type == GetPingType(responderFlow))
            {
                if (header->payloadLength != m_receivedBytes[receiveSlot] - sizeof(MessageHeader))
                {
                    std::printf("Received an invalid ping.\n");
                    return false;
                }

                const ULONG echoLength = m_receivedBytes[receiveSlot];
                const ULONG sequence = header->sequence;
                std::copy_n(
                    ReceiveBuffer(receiveSlot),
                    echoLength,
                    SendBuffer(responderFlow));
                if (FAILED(PostReceive(receiveSlot)) || !SendCurrentMessage(
                                                            responderFlow,
                                                            echoLength))
                {
                    return false;
                }

                if (outlierThresholdMicroseconds != 0 && sequence >= warmupIterations)
                {
                    LARGE_INTEGER sendEndTime{};
                    QueryPerformanceCounter(&sendEndTime);
                    const DWORD sendEndProcessor = GetCurrentProcessorNumber();
                    const double receiveWaitMicroseconds =
                        static_cast<double>(receiveEndTime.QuadPart - receiveStartTime.QuadPart) * c_microsecondsPerSecond /
                        static_cast<double>(performanceFrequency.QuadPart);
                    const double echoMicroseconds = static_cast<double>(sendEndTime.QuadPart - receiveEndTime.QuadPart) *
                                                    c_microsecondsPerSecond / static_cast<double>(performanceFrequency.QuadPart);
                    if (receiveWaitMicroseconds >= static_cast<double>(outlierThresholdMicroseconds) ||
                        echoMicroseconds >= static_cast<double>(outlierThresholdMicroseconds))
                    {
                        std::fprintf(
                            stderr,
                            "Responder outlier: direction=%s sequence=%lu "
                            "receiveWait=%.3f us echo=%.3f us "
                            "cpu=%lu>%lu>%lu\n",
                            responderDirection,
                            sequence,
                            receiveWaitMicroseconds,
                            echoMicroseconds,
                            receiveStartProcessor,
                            receiveEndProcessor,
                            sendEndProcessor);
                    }
                }
            }
            else if (header->type == GetDoneType(responderFlow))
            {
                if (m_receivedBytes[receiveSlot] != m_messageLength || header->sequence != 0 || header->payloadLength != 0)
                {
                    std::printf("Received an invalid completion message.\n");
                    return false;
                }

                responderCompleted = true;
                if (!initiatorCompleted && FAILED(PostReceive(receiveSlot)))
                {
                    return false;
                }

                FillControl(
                    responderFlow,
                    GetDoneAckType(responderFlow));
                if (!SendCurrentMessage(
                        responderFlow,
                        m_messageLength))
                {
                    return false;
                }
            }
            else
            {
                std::printf("Received an unexpected bidirectional message.\n");
                return false;
            }
        }

        return true;
    }

    void
    Shutdown()
    {
        CancelCompletionNotification();

        if (m_connector.get() != nullptr && m_connected)
        {
            CompleteOverlapped(
                m_connector.get(),
                m_connector->Disconnect(&m_operationOverlapped));
            m_connected = false;
        }

        m_queuePair.reset();

        if (m_memoryRegion.get() != nullptr && m_memoryRegistered)
        {
            CompleteOverlapped(
                m_memoryRegion.get(),
                m_memoryRegion->Deregister(&m_operationOverlapped));
            m_memoryRegistered = false;
        }
    }

private:

    static
    MessageType
    GetPingType(
        ULONG flow)
    {
        return flow == c_forwardFlow ? MessageType::Ping : MessageType::ReversePing;
    }

    static
    MessageType
    GetDoneType(
        ULONG flow)
    {
        return flow == c_forwardFlow ? MessageType::Done : MessageType::ReverseDone;
    }

    static
    MessageType
    GetDoneAckType(
        ULONG flow)
    {
        return flow == c_forwardFlow ? MessageType::DoneAck : MessageType::ReverseDoneAck;
    }

    bool
    StartPing(
        ULONG flow,
        ULONG sequence,
        ULONG messageSize,
        ULONG outlierThresholdMicroseconds,
        _Out_ FILETIME &startUtc,
        _Out_ LARGE_INTEGER &startTime,
        _Out_ LARGE_INTEGER &sendEndTime,
        _Out_ DWORD &startProcessor,
        _Out_ DWORD &sendEndProcessor)
    {
        FillPing(
            flow,
            sequence,
            messageSize);
        if (outlierThresholdMicroseconds != 0)
        {
            GetSystemTimePreciseAsFileTime(&startUtc);
        }

        QueryPerformanceCounter(&startTime);
        if (outlierThresholdMicroseconds != 0)
        {
            startProcessor = GetCurrentProcessorNumber();
        }

        if (!SendCurrentMessage(
                flow,
                messageSize))
        {
            return false;
        }

        if (outlierThresholdMicroseconds != 0)
        {
            QueryPerformanceCounter(&sendEndTime);
            sendEndProcessor = GetCurrentProcessorNumber();
        }

        return true;
    }

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

    HRESULT
    ArmDisconnectNotification()
    {
        ResetEvent(m_disconnectEvent.get());
        m_connected = true;
        HRESULT result = m_connector->NotifyDisconnect(&m_disconnectOverlapped);
        if (result == ND_PENDING)
        {
            return S_OK;
        }

        if (SUCCEEDED(result))
        {
            SetEvent(m_disconnectEvent.get());
        }

        return result;
    }

    HRESULT
    PostReceive(
        ULONG receiveSlot)
    {
        m_receiveCompleted[receiveSlot] = false;
        m_receivedBytes[receiveSlot] = 0;
        ND2_SGE receiveSge{};
        receiveSge.Buffer = ReceiveBuffer(receiveSlot);
        receiveSge.BufferLength = m_messageLength;
        receiveSge.MemoryRegionToken = m_memoryRegion->GetLocalToken();
        return m_queuePair->Receive(
            &m_receiveContexts[receiveSlot],
            &receiveSge,
            1);
    }

    void
    CancelCompletionNotification()
    {
        if (!m_cqNotifyPending || m_completionQueue.get() == nullptr)
        {
            return;
        }

        m_completionQueue->CancelOverlappedRequests();
        m_completionQueue->GetOverlappedResult(
            &m_cqOverlapped,
            TRUE);
        m_cqNotifyPending = false;
    }

    bool
    SendCurrentMessage(
        ULONG flow,
        ULONG messageSize)
    {
        if (flow >= c_flowCount)
        {
            std::printf(
                "Invalid send flow %lu.\n",
                flow);
            return false;
        }

        m_sendCompleted[flow] = false;
        ND2_SGE sendSge{};
        sendSge.Buffer = SendBuffer(flow);
        sendSge.BufferLength = messageSize;
        sendSge.MemoryRegionToken = m_memoryRegion->GetLocalToken();
        ULONG flags = messageSize <= m_inlineThreshold ? ND_OP_FLAG_INLINE : 0;
        HRESULT result = m_queuePair->Send(
            &m_sendContexts[flow],
            &sendSge,
            1,
            flags);
        if (FAILED(result))
        {
            std::printf(
                "Send failed: 0x%08lx.\n",
                result);
            return false;
        }

        return WaitForCompletion(
            true,
            flow);
    }

    bool
    ReceiveCurrentMessage(
        _Out_ ULONG &receiveSlot)
    {
        if (!WaitForCompletion(
                false,
                c_forwardFlow))
        {
            return false;
        }

        for (receiveSlot = 0; receiveSlot < m_activeReceiveSlotCount; ++receiveSlot)
        {
            if (m_receiveCompleted[receiveSlot])
            {
                m_receiveCompleted[receiveSlot] = false;
                return true;
            }
        }

        std::printf("Receive completion was not associated with a slot.\n");
        return false;
    }

    bool
    WaitForCompletion(
        bool waitForSend,
        ULONG flow)
    {
        while (waitForSend ? !m_sendCompleted[flow] : !HasCompletedReceive())
        {
            ND2_RESULT completion{};
            while (m_completionQueue->GetResults(
                       &completion,
                       1) == 1)
            {
                if (completion.Status == ND_CANCELED || completion.Status == ND_CONNECTION_ABORTED ||
                    completion.Status == ND_CONNECTION_INVALID)
                {
                    std::printf("Peer disconnected.\n");
                    return false;
                }

                if (FAILED(completion.Status))
                {
                    std::printf(
                        "Completion failed: 0x%08lx.\n",
                        completion.Status);
                    return false;
                }

                bool sendCompletion = false;
                for (ULONG sendFlow = 0; sendFlow < c_flowCount; ++sendFlow)
                {
                    if (completion.RequestContext == &m_sendContexts[sendFlow])
                    {
                        m_sendCompleted[sendFlow] = true;
                        sendCompletion = true;
                        break;
                    }
                }

                if (!sendCompletion)
                {
                    bool receiveCompletion = false;
                    for (ULONG receiveSlot = 0; receiveSlot < m_activeReceiveSlotCount; ++receiveSlot)
                    {
                        if (completion.RequestContext == &m_receiveContexts[receiveSlot])
                        {
                            m_receiveCompleted[receiveSlot] = true;
                            m_receivedBytes[receiveSlot] = completion.BytesTransferred;
                            receiveCompletion = true;
                            break;
                        }
                    }

                    if (!receiveCompletion)
                    {
                        std::printf("Received a completion with an unknown context.\n");
                        return false;
                    }
                }
            }

            if (waitForSend ? m_sendCompleted[flow] : HasCompletedReceive())
            {
                break;
            }

            if (WaitForSingleObject(
                    m_disconnectEvent.get(),
                    0) == WAIT_OBJECT_0)
            {
                std::printf("Peer disconnected.\n");
                return false;
            }

            if (!m_blocking)
            {
                YieldProcessor();
                continue;
            }

            ResetEvent(m_cqEvent.get());
            HRESULT result = m_completionQueue->Notify(
                ND_CQ_NOTIFY_ANY,
                &m_cqOverlapped);
            if (result == S_OK)
            {
                continue;
            }

            if (result != ND_PENDING)
            {
                std::printf(
                    "Completion notification failed: 0x%08lx.\n",
                    result);
                return false;
            }
            m_cqNotifyPending = true;

            HANDLE events[] = { m_cqEvent.get(),
                                m_disconnectEvent.get() };
            DWORD waitResult = WaitForMultipleObjects(
                ARRAYSIZE(events),
                events,
                FALSE,
                INFINITE);
            if (waitResult == WAIT_OBJECT_0 + 1)
            {
                CancelCompletionNotification();
                std::printf("Peer disconnected.\n");
                return false;
            }

            if (waitResult != WAIT_OBJECT_0)
            {
                CancelCompletionNotification();
                std::printf(
                    "Completion wait failed: %lu.\n",
                    GetLastError());
                return false;
            }

            result = m_completionQueue->GetOverlappedResult(
                &m_cqOverlapped,
                FALSE);
            m_cqNotifyPending = false;
            if (FAILED(result))
            {
                std::printf(
                    "Completion notification result failed: 0x%08lx.\n",
                    result);
                return false;
            }
        }

        return true;
    }

    bool
    HasCompletedReceive() const
    {
        for (ULONG receiveSlot = 0; receiveSlot < m_activeReceiveSlotCount; ++receiveSlot)
        {
            if (m_receiveCompleted[receiveSlot])
            {
                return true;
            }
        }

        return false;
    }

    void
    FillPing(
        ULONG flow,
        ULONG sequence,
        ULONG messageSize)
    {
        MessageHeader *header = GetSendHeader(flow);
        header->magic = c_protocolMagic;
        header->version = c_protocolVersion;
        header->type = GetPingType(flow);
        header->sequence = sequence;
        header->payloadLength = messageSize - sizeof(MessageHeader);

        for (ULONG index = sizeof(MessageHeader); index < messageSize; ++index)
        {
            SendBuffer(flow)[index] = static_cast<BYTE>((sequence + index) & 0xff);
        }
    }

    void
    FillControl(
        ULONG flow,
        MessageType type)
    {
        MessageHeader *header = GetSendHeader(flow);
        header->magic = c_protocolMagic;
        header->version = c_protocolVersion;
        header->type = type;
        header->sequence = 0;
        header->payloadLength = 0;
    }

    bool
    ValidatePayload(
        ULONG receiveSlot,
        ULONG sequence,
        ULONG messageSize) const
    {
        for (ULONG index = sizeof(MessageHeader); index < messageSize; ++index)
        {
            BYTE expected = static_cast<BYTE>((sequence + index) & 0xff);
            if (ReceiveBuffer(receiveSlot)[index] != expected)
            {
                return false;
            }
        }

        return true;
    }

    // Each flow's send buffer and each receive slot starts on its own page.
    BYTE *
    SendBuffer(
        ULONG flow)
    {
        return m_buffer.get() + m_bufferStride * flow;
    }

    const BYTE *
    ReceiveBuffer(
        ULONG receiveSlot) const
    {
        return m_buffer.get() + m_bufferStride * (c_flowCount + receiveSlot);
    }

    BYTE *
    ReceiveBuffer(
        ULONG receiveSlot)
    {
        return m_buffer.get() + m_bufferStride * (c_flowCount + receiveSlot);
    }

    MessageHeader *
    GetSendHeader(
        ULONG flow)
    {
        return reinterpret_cast<MessageHeader *>(SendBuffer(flow));
    }

    const MessageHeader *
    GetReceiveHeader(
        ULONG receiveSlot) const
    {
        return reinterpret_cast<const MessageHeader *>(ReceiveBuffer(receiveSlot));
    }

    sockaddr_in m_localAddress{};
    bool m_blocking = false;
    ULONG m_messageCapacity = 0;

    // Length of every receive and control message, so each message spans as
    // many pages as the receive it lands in.
    ULONG m_messageLength = 0;
    size_t m_bufferStride = 0;
    size_t m_bufferSize = 0;
    wil::unique_virtualalloc_ptr<BYTE> m_buffer;
    wistd::unique_ptr<LoadedProvider> m_provider;
    wil::com_ptr_nothrow<IND2Adapter> m_adapter;

    // Declared before the objects created with it so that it is closed last.
    wil::unique_handle m_adapterFile;
    wil::com_ptr_nothrow<IND2CompletionQueue> m_completionQueue;
    wil::com_ptr_nothrow<IND2MemoryRegion> m_memoryRegion;
    wil::com_ptr_nothrow<IND2Connector> m_connector;
    wil::com_ptr_nothrow<IND2QueuePair> m_queuePair;
    wil::com_ptr_nothrow<IND2Listener> m_listener;
    wil::unique_handle m_operationEvent;
    wil::unique_handle m_cqEvent;
    wil::unique_handle m_disconnectEvent;
    OVERLAPPED m_operationOverlapped{};
    OVERLAPPED m_cqOverlapped{};
    OVERLAPPED m_disconnectOverlapped{};
    ULONG m_inlineThreshold = 0;
    std::array<ULONG, c_receiveSlotCount> m_receivedBytes{};
    std::array<ULONG, c_flowCount> m_sendContexts{};
    std::array<ULONG, c_receiveSlotCount> m_receiveContexts{};
    std::array<bool, c_flowCount> m_sendCompleted{};
    std::array<bool, c_receiveSlotCount> m_receiveCompleted{};
    ULONG m_activeReceiveSlotCount = c_unidirectionalReceiveSlotCount;
    bool m_cqNotifyPending = false;
    bool m_memoryRegistered = false;
    bool m_connected = false;
};

static
void
ShowUsage()
{
    std::printf(
        "ndlat -s [-1] <listen-ip>[:port]\n"
        "ndlat -c [-d] [-b] [-r] [-i iterations] [-w iterations] "
        "[-z bytes] [-u seconds] [-o microseconds] <server-ip>[:port]\n"
        "  -s, --server         Listen for client-defined workloads.\n"
        "  -1, --one-shot       Exit after one server workload.\n"
        "  -c, --client         Connect and define the workload.\n"
        "  -d, --bidirectional  Run a second ping-pong flow in the reverse direction.\n"
        "                       Both flows share one queue pair.\n"
        "  -b, --blocking       Wait for completion notifications instead of polling.\n"
        "  -r, --realtime       Use real-time process priority and time-critical\n"
        "                       priority for the data-path thread.\n"
        "  -i, --iterations N   Ping count per direction (default: %lu).\n"
        "  -w, --warmup N       Warm-up ping count per direction, excluded from\n"
        "                       statistics (default: %lu).\n"
        "  -z, --size N         Message size in bytes (default: %lu, max: %lu).\n"
        "  -u, --update-seconds N\n"
        "                       Print one RTT cycle summary every N seconds.\n"
        "                       Cycle updates are disabled by default.\n"
        "  -o, --outlier-us N   Print phase timing for RTT samples >= N us.\n"
        "                       Outlier diagnostics are disabled by default.\n"
        "  -h, --help           Show this help.\n"
        "Default port: %hu.\n"
        "All workload options are specified by the client and sent to the server.\n",
        c_defaultIterations,
        c_defaultWarmupIterations,
        c_defaultMessageSize,
        c_maxMessageSize,
        c_defaultPort);
}

static
bool
ParseUnsigned(
    _In_ const WCHAR *text,
    _Out_ ULONG &value)
{
    WCHAR *end = nullptr;
    unsigned long long parsedValue = _wcstoui64(
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
ParseOptions(
    int argumentCount,
    _In_reads_(argumentCount) WCHAR **arguments,
    _Out_ Options &options)
{
    const WCHAR *addressText = nullptr;
    bool workloadOptionSpecified = false;

    for (int index = 1; index < argumentCount; ++index)
    {
        const WCHAR *argument = arguments[index];
        if (wcscmp(
                argument,
                L"-s") == 0 ||
            wcscmp(
                argument,
                L"--server") == 0)
        {
            options.server = true;
        }
        else if (wcscmp(
                     argument,
                     L"-c") == 0 ||
                 wcscmp(
                     argument,
                     L"--client") == 0)
        {
            options.client = true;
        }
        else if (wcscmp(
                     argument,
                     L"-d") == 0 ||
                 wcscmp(
                     argument,
                     L"--bidirectional") == 0)
        {
            options.bidirectional = true;
            workloadOptionSpecified = true;
        }
        else if (wcscmp(
                     argument,
                     L"-b") == 0 ||
                 wcscmp(
                     argument,
                     L"--blocking") == 0)
        {
            options.blocking = true;
            workloadOptionSpecified = true;
        }
        else if (wcscmp(
                     argument,
                     L"-r") == 0 ||
                 wcscmp(
                     argument,
                     L"--realtime") == 0)
        {
            options.realtime = true;
            workloadOptionSpecified = true;
        }
        else if (wcscmp(
                     argument,
                     L"-1") == 0 ||
                 wcscmp(
                     argument,
                     L"--one-shot") == 0)
        {
            options.oneShot = true;
        }
        else if (wcscmp(
                     argument,
                     L"-i") == 0 ||
                 wcscmp(
                     argument,
                     L"--iterations") == 0)
        {
            workloadOptionSpecified = true;
            if (++index >= argumentCount || !ParseUnsigned(
                                                arguments[index],
                                                options.iterations))
            {
                return false;
            }
        }
        else if (wcscmp(
                     argument,
                     L"-w") == 0 ||
                 wcscmp(
                     argument,
                     L"--warmup") == 0)
        {
            workloadOptionSpecified = true;
            if (++index >= argumentCount || !ParseUnsigned(
                                                arguments[index],
                                                options.warmupIterations))
            {
                return false;
            }
        }
        else if (wcscmp(
                     argument,
                     L"-z") == 0 ||
                 wcscmp(
                     argument,
                     L"--size") == 0)
        {
            workloadOptionSpecified = true;
            if (++index >= argumentCount || !ParseUnsigned(
                                                arguments[index],
                                                options.messageSize))
            {
                return false;
            }
        }
        else if (wcscmp(
                     argument,
                     L"-u") == 0 ||
                 wcscmp(
                     argument,
                     L"--update-seconds") == 0)
        {
            workloadOptionSpecified = true;
            if (++index >= argumentCount || !ParseUnsigned(
                                                arguments[index],
                                                options.statisticsUpdateSeconds))
            {
                return false;
            }
        }
        else if (wcscmp(
                     argument,
                     L"-o") == 0 ||
                 wcscmp(
                     argument,
                     L"--outlier-us") == 0)
        {
            workloadOptionSpecified = true;
            if (++index >= argumentCount || !ParseUnsigned(
                                                arguments[index],
                                                options.outlierThresholdMicroseconds))
            {
                return false;
            }
        }
        else if (wcscmp(
                     argument,
                     L"-h") == 0 ||
                 wcscmp(
                     argument,
                     L"--help") == 0)
        {
            ShowUsage();
            ExitProcess(0);
        }
        else if (argument[0] == L'-' || addressText != nullptr)
        {
            return false;
        }
        else
        {
            addressText = argument;
        }
    }

    if (options.server == options.client || addressText == nullptr || (options.server && workloadOptionSpecified) ||
        (options.client && options.oneShot) || options.iterations == 0 ||
        options.warmupIterations > ULONG_MAX - options.iterations || options.messageSize < sizeof(MessageHeader) ||
        options.messageSize > c_maxMessageSize)
    {
        return false;
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

static
HRESULT
ResolveLocalAddress(
    _In_ const sockaddr_in &remoteAddress,
    _Out_ sockaddr_in &localAddress)
{
    SOCKET socketHandle = WSASocketW(
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
        HRESULT connectResult = HRESULT_FROM_WIN32(WSAGetLastError());
        closesocket(socketHandle);
        return connectResult;
    }

    INT addressLength = sizeof(localAddress);
    result = getsockname(
        socketHandle,
        reinterpret_cast<sockaddr *>(&localAddress),
        &addressLength);
    HRESULT addressResult = result == SOCKET_ERROR ? HRESULT_FROM_WIN32(WSAGetLastError()) : S_OK;
    closesocket(socketHandle);
    return addressResult;
}

static
HRESULT
InitializeConnection(
    _Inout_ Connection &connection)
{
    HRESULT result = connection.Initialize();
    if (FAILED(result))
    {
        std::printf(
            "Connection initialization failed: 0x%08lx.\n",
            result);
    }

    return result;
}

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

static
bool
ExecuteServerWorkload(
    _Inout_ Connection &primary,
    _In_ const Options &options)
{
    if (!options.bidirectional)
    {
        return primary.RunResponder(
            options.warmupIterations,
            options.outlierThresholdMicroseconds,
            "client -> server");
    }

    if (!primary.ReceiveControl(MessageType::BidirectionalReady) || !primary.SendControl(MessageType::BidirectionalReady))
    {
        std::printf("Failed to synchronize bidirectional execution.\n");
        return false;
    }

    return primary.RunBidirectional(
        false,
        options.iterations,
        options.warmupIterations,
        options.messageSize,
        options.statisticsUpdateSeconds,
        options.outlierThresholdMicroseconds);
}

enum class ServerWorkloadResult
{
    Completed,
    Rejected,
    Fatal,
};

static
ServerWorkloadResult
RunServerWorkload(
    _In_ const Options &serverOptions)
{
    Connection primary(
        serverOptions.address,
        false,
        c_maxMessageSize,
        static_cast<ULONG>(sizeof(WorkloadMessage)));
    if (FAILED(InitializeConnection(primary)) || FAILED(primary.PrepareListener()))
    {
        return ServerWorkloadResult::Fatal;
    }

    std::printf(
        "Listening on %u.%u.%u.%u:%hu.\n",
        serverOptions.address.sin_addr.S_un.S_un_b.s_b1,
        serverOptions.address.sin_addr.S_un.S_un_b.s_b2,
        serverOptions.address.sin_addr.S_un.S_un_b.s_b3,
        serverOptions.address.sin_addr.S_un.S_un_b.s_b4,
        ntohs(serverOptions.address.sin_port));

    if (FAILED(primary.AcceptPrepared()))
    {
        std::printf("Failed to accept the primary connection.\n");
        return ServerWorkloadResult::Rejected;
    }
    std::printf("Primary connection established.\n");

    Options workloadOptions{};
    workloadOptions.address = serverOptions.address;
    if (!primary.ReceiveWorkload(workloadOptions))
    {
        return ServerWorkloadResult::Rejected;
    }

    std::printf(
        "Workload: iterations=%lu, warmup=%lu, size=%lu, blocking=%s, "
        "bidirectional=%s, realtime=%s.\n",
        workloadOptions.iterations,
        workloadOptions.warmupIterations,
        workloadOptions.messageSize,
        workloadOptions.blocking ? "yes" : "no",
        workloadOptions.bidirectional ? "yes" : "no",
        workloadOptions.realtime ? "yes" : "no");

    const DWORD originalProcessPriorityClass = GetPriorityClass(GetCurrentProcess());
    if (originalProcessPriorityClass == 0)
    {
        std::printf(
            "Failed to query server process priority: %lu.\n",
            GetLastError());
        return ServerWorkloadResult::Rejected;
    }

    const int originalThreadPriority = GetThreadPriority(GetCurrentThread());
    if (originalThreadPriority == THREAD_PRIORITY_ERROR_RETURN)
    {
        std::printf(
            "Failed to query server thread priority: %lu.\n",
            GetLastError());
        return ServerWorkloadResult::Rejected;
    }

    if (workloadOptions.realtime)
    {
        const DWORD priorityError = EnableRealtimePriority();
        if (priorityError != ERROR_SUCCESS)
        {
            RestorePriority(
                originalProcessPriorityClass,
                originalThreadPriority);
            return ServerWorkloadResult::Rejected;
        }
    }

    const bool workloadResult = ExecuteServerWorkload(
        primary,
        workloadOptions);

    if (workloadOptions.realtime)
    {
        RestorePriority(
            originalProcessPriorityClass,
            originalThreadPriority);
    }

    return workloadResult ? ServerWorkloadResult::Completed : ServerWorkloadResult::Rejected;
}

static
bool
RunServer(
    _In_ const Options &options)
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

static
bool
RunClient(
    _In_ const Options &options)
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

    const ULONG connectionMessageCapacity = std::max(
        options.messageSize,
        static_cast<ULONG>(sizeof(WorkloadMessage)));
    Connection primary(
        localAddress,
        options.blocking,
        connectionMessageCapacity,
        options.messageSize);
    if (FAILED(InitializeConnection(primary)) || FAILED(primary.Connect(options.address)))
    {
        std::printf("Failed to establish the primary connection.\n");
        return false;
    }
    std::printf("Primary connection established.\n");

    if (!primary.SendWorkload(options))
    {
        std::printf("Failed to send workload configuration.\n");
        return false;
    }

    if (!primary.ReceiveControl(MessageType::WorkloadReady))
    {
        std::printf("Failed to synchronize workload readiness.\n");
        return false;
    }

    if (!options.bidirectional)
    {
        return primary.RunInitiator(
            options.iterations,
            options.warmupIterations,
            options.messageSize,
            options.statisticsUpdateSeconds,
            options.outlierThresholdMicroseconds,
            "client -> server");
    }

    if (FAILED(primary.EnableBidirectionalReceiveSlot()))
    {
        std::printf("Failed to enable the bidirectional receive slot.\n");
        return false;
    }

    if (!primary.SendControl(MessageType::BidirectionalReady) || !primary.ReceiveControl(MessageType::BidirectionalReady))
    {
        std::printf("Failed to synchronize bidirectional execution.\n");
        return false;
    }

    return primary.RunBidirectional(
        true,
        options.iterations,
        options.warmupIterations,
        options.messageSize,
        options.statisticsUpdateSeconds,
        options.outlierThresholdMicroseconds);
}

int __cdecl
wmain(
    int argumentCount,
    WCHAR **arguments)
{
    WSADATA winsockData{};
    INT result = WSAStartup(
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

    bool testResult = options.server ? RunServer(options) : RunClient(options);
    WSACleanup();

    if (!testResult)
    {
        std::printf("ndlat stopped before completing all exchanges.\n");
        return ERROR_CONNECTION_ABORTED;
    }

    return 0;
}
