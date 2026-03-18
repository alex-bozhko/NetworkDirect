//
// transfer_file.cpp - RDMA file transfer using NetworkDirect IND2 interface
//
// Usage:
//   transfer_file -s <ip>[:<port>]                     (server: receive file)
//   transfer_file -c <ip>[:<port>] <filepath>          (client: send file)
//   transfer_file -c <ip>[:<port>] <filepath> -chunk <MB>  (custom chunk size)
//   transfer_file -c <ip>[:<port>] <filepath> -n <count>   (repeat N times)
//

#include "ndcommon.h"
#include "ndtestutil.h"
#include <logging.h>
#include <string>
#include <conio.h>

const USHORT x_DefaultPort = 54400;
const SIZE_T x_DefaultChunkSize = 10 * 1024 * 1024;  // 10 MB
const LPCWSTR TESTNAME = L"transfer_file.exe";

#define RECV_CTXT  ((void*)0x1000)
#define SEND_CTXT  ((void*)0x2000)
#define WRITE_CTXT ((void*)0x4000)

#pragma pack(push, 1)
struct FileHeader
{
    WCHAR fileName[260];
    UINT64 fileSize;
    UINT64 chunkSize;
    UINT64 repeatCount;
};

struct PeerInfo
{
    UINT32 remoteToken;
    UINT64 remoteAddress;
};

struct ChunkAck
{
    UINT64 bytesWrittenSoFar;
};
#pragma pack(pop)

// ============================================================================
// CPU measurement
// ============================================================================

static double GetSystemCpuSec()
{
    FILETIME ftIdle, ftKernel, ftUser;
    GetSystemTimes(&ftIdle, &ftKernel, &ftUser);
    ULARGE_INTEGER kernel, user, idle;
    kernel.LowPart = ftKernel.dwLowDateTime; kernel.HighPart = ftKernel.dwHighDateTime;
    user.LowPart = ftUser.dwLowDateTime; user.HighPart = ftUser.dwHighDateTime;
    idle.LowPart = ftIdle.dwLowDateTime; idle.HighPart = ftIdle.dwHighDateTime;
    return (double)(kernel.QuadPart - idle.QuadPart + user.QuadPart) / 10000000.0;
}

// ============================================================================
// Progress bar
// ============================================================================

static bool g_noProgress = false;

static void PrintProgress(UINT64 current, UINT64 total, double elapsedSec, double cpuPct)
{
    if (g_noProgress) return;
    if (total == 0) return;

    double pct = (double)current / (double)total * 100.0;
    int barWidth = 40;
    int filled = (int)(pct / 100.0 * barWidth);

    printf("\r  [");
    for (int i = 0; i < barWidth; i++)
        printf(i < filled ? "#" : ".");
    printf("] %5.1f%%", pct);

    if (elapsedSec > 0.01)
    {
        double mbps = ((double)current / (1024.0 * 1024.0)) / elapsedSec;
        printf("  %7.1f MB/s  CPU %5.1f%%", mbps, cpuPct);
    }

    if (current >= total)
        printf("  DONE\n");
    else
        printf("    ");

    fflush(stdout);
}

void ShowUsage()
{
    printf("transfer_file [options] <ip>[:<port>] [filepath]\n"
        "Options:\n"
        "\t-s              - Start as server (receive file)\n"
        "\t-c              - Start as client (send file)\n"
        "\t-chunk <MB>     - Chunk size in MB (default: 10)\n"
        "\t-n <count>      - Repeat transfer N times (default: 1)\n"
        "\t-hack           - Hack mode: skip NdResolveAddress, getch() on server\n"
        "\t-noprogress/-np - Disable progress bar\n"
        "<ip>              - IPv4 Address\n"
        "<port>            - Port number (default: %hu)\n"
        "<filepath>        - Path to file to send (client only)\n",
        x_DefaultPort
    );
}

// ============================================================================
// Server: receive file via RDMA Write from client
// ============================================================================

class FileReceiver : public NdTestServerBase
{
public:
    FileReceiver(bool hack) : m_hack(hack) {}

    // Required by NdTestServerBase - not used, we call RunTest() with our own signature
    void RunTest(const struct sockaddr_in& v4Src, DWORD, DWORD) override
    {
        RunTest(v4Src);
    }

    void RunTest(const struct sockaddr_in& v4Src)
    {
        printf("[SERVER] Initializing...\n");
        NdTestBase::Init(v4Src);

        ND2_ADAPTER_INFO adapterInfo = { 0 };
        NdTestBase::GetAdapterInfo(&adapterInfo);
        printf("[SERVER] Adapter: MaxCQDepth=%lu, MaxRecvQDepth=%lu\n",
            adapterInfo.MaxCompletionQueueDepth, adapterInfo.MaxReceiveQueueDepth);

        // Create a small MR for control messages (FileHeader exchange)
        NdTestBase::CreateMR();
        m_ctrlBuf = static_cast<char*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(FileHeader) + sizeof(PeerInfo)));
        if (!m_ctrlBuf)
        {
            printf("[SERVER] Failed to allocate control buffer\n");
            exit(1);
        }
        NdTestBase::RegisterDataBuffer(m_ctrlBuf,
            (DWORD)(sizeof(FileHeader) + sizeof(PeerInfo)),
            ND_MR_FLAG_ALLOW_LOCAL_WRITE);

        DWORD queueDepth = min(adapterInfo.MaxCompletionQueueDepth, adapterInfo.MaxReceiveQueueDepth);
        queueDepth = min(queueDepth, (DWORD)64);

        NdTestBase::CreateCQ(queueDepth);
        NdTestBase::CreateConnector();
        NdTestBase::CreateQueuePair(queueDepth, 1, adapterInfo.InlineRequestThreshold);

        // Post receive for FileHeader from client
        ND2_SGE ctrlSge;
        ctrlSge.Buffer = m_ctrlBuf;
        ctrlSge.BufferLength = sizeof(FileHeader);
        ctrlSge.MemoryRegionToken = m_pMr->GetLocalToken();
        NdTestBase::PostReceive(&ctrlSge, 1, RECV_CTXT);

        // Also post receive for chunk ACKs (we'll repost as needed)
        ND2_SGE ackSge;
        ackSge.Buffer = m_ctrlBuf + sizeof(FileHeader);
        ackSge.BufferLength = sizeof(ChunkAck);
        ackSge.MemoryRegionToken = m_pMr->GetLocalToken();
        NdTestBase::PostReceive(&ackSge, 1, (void*)0x5000);

        NdTestServerBase::CreateListener();
        printf("[SERVER] Listening on %d.%d.%d.%d:%d...\n",
            v4Src.sin_addr.S_un.S_un_b.s_b1, v4Src.sin_addr.S_un.S_un_b.s_b2,
            v4Src.sin_addr.S_un.S_un_b.s_b3, v4Src.sin_addr.S_un.S_un_b.s_b4,
            ntohs(v4Src.sin_port));
        NdTestServerBase::Listen(v4Src);
        NdTestServerBase::GetConnectionRequest();
        printf("[SERVER] Connection request received. Accepting...\n");
        NdTestServerBase::Accept(0, 0);
        printf("[SERVER] Connected.\n");

        if (m_hack)
        {
            printf("[SERVER] Hack mode: press any key when Client is ready to proceed...\n");
            _getch();
        }

        // Wait for FileHeader
        printf("[SERVER] Waiting for file header...\n");
        WaitForCompletionAndCheckContext(RECV_CTXT);

        FileHeader* pHdr = reinterpret_cast<FileHeader*>(m_ctrlBuf);
        m_fileSize = pHdr->fileSize;
        m_chunkSize = pHdr->chunkSize;
        m_repeatCount = pHdr->repeatCount;
        wcsncpy_s(m_fileName, pHdr->fileName, 259);

        UINT64 totalTransferSize = m_fileSize * m_repeatCount;
        printf("[SERVER] Incoming file: %ls (%llu bytes, chunk=%llu MB, repeat=%llu, total=%llu bytes)\n",
            m_fileName, m_fileSize, m_chunkSize / (1024 * 1024), m_repeatCount, totalTransferSize);

        // Allocate the full file buffer and register for RDMA write
        m_fileBuf = static_cast<char*>(VirtualAlloc(nullptr, m_fileSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!m_fileBuf)
        {
            printf("[SERVER] VirtualAlloc failed for %llu bytes: %lu\n", m_fileSize, GetLastError());
            exit(1);
        }

        // Create a second MR for the file data
        HRESULT hr = m_pAdapter->CreateMemoryRegion(
            IID_IND2MemoryRegion, m_hAdapterFile, reinterpret_cast<void**>(&m_pFileMr));
        if (FAILED(hr))
        {
            printf("[SERVER] CreateMemoryRegion for file buffer failed: 0x%08x\n", hr);
            exit(1);
        }

        OVERLAPPED ov = { 0 };
        ov.hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        hr = m_pFileMr->Register(m_fileBuf, m_fileSize,
            ND_MR_FLAG_ALLOW_LOCAL_WRITE | ND_MR_FLAG_ALLOW_REMOTE_WRITE, &ov);
        if (hr == ND_PENDING)
        {
            hr = m_pFileMr->GetOverlappedResult(&ov, TRUE);
        }
        if (FAILED(hr))
        {
            printf("[SERVER] Register file buffer failed: 0x%08x\n", hr);
            exit(1);
        }
        CloseHandle(ov.hEvent);

        printf("[SERVER] File buffer registered: remoteToken=0x%x, addr=%p\n",
            m_pFileMr->GetRemoteToken(), m_fileBuf);

        // Send PeerInfo back to client
        PeerInfo* pInfo = reinterpret_cast<PeerInfo*>(m_ctrlBuf);
        pInfo->remoteToken = m_pFileMr->GetRemoteToken();
        pInfo->remoteAddress = reinterpret_cast<UINT64>(m_fileBuf);

        ctrlSge.Buffer = m_ctrlBuf;
        ctrlSge.BufferLength = sizeof(PeerInfo);
        NdTestBase::Send(&ctrlSge, 1, 0, SEND_CTXT);
        WaitForCompletionAndCheckContext(SEND_CTXT);
        printf("[SERVER] PeerInfo sent. Waiting for data...\n");

        // Receive chunk ACKs until all data received (across all repeats)
        UINT64 totalReceived = 0;
        UINT64 totalTransferBytes = m_fileSize * m_repeatCount;
        UINT64 chunksPerPass = (m_fileSize + m_chunkSize - 1) / m_chunkSize;
        UINT64 totalChunks = chunksPerPass * m_repeatCount;
        Timer timer;
        timer.Start();
        double startCpu = GetSystemCpuSec();

        printf("[SERVER] Receiving %llu chunks(%llu per pass x %llu repeats)...\n",
            totalChunks, chunksPerPass, m_repeatCount);

        for (UINT64 chunk = 0; chunk < totalChunks; chunk++)
        {
            // Wait for chunk ACK from client
            ND2_RESULT ndRes;
            WaitForCompletion(&ndRes, true);
            if (ndRes.Status != ND_SUCCESS)
            {
                printf("\n[SERVER] Completion error: 0x%08x\n", ndRes.Status);
                break;
            }

            if (ndRes.RequestContext == (void*)0x5000)
            {
                ChunkAck* pAck = reinterpret_cast<ChunkAck*>(m_ctrlBuf + sizeof(FileHeader));
                totalReceived = pAck->bytesWrittenSoFar;

                // Repost receive for next ACK
                if (chunk + 1 < totalChunks)
                {
                    NdTestBase::PostReceive(&ackSge, 1, (void*)0x5000);
                }

                timer.End();
                double elapsed = timer.Report() / 1000000.0;
                double cpuUsed = GetSystemCpuSec() - startCpu;
                double cpuPct = (elapsed > 0) ? (cpuUsed / elapsed) * 100.0 : 0;
                PrintProgress(totalReceived, totalTransferBytes, elapsed, cpuPct);
            }
        }

        timer.End();
        double totalTime = timer.Report() / 1000000.0;
        double totalCpu = GetSystemCpuSec() - startCpu;
        double avgCpuPct = (totalTime > 0) ? (totalCpu / totalTime) * 100.0 : 0;
        printf("[SERVER] Transfer complete: %llu bytes (%llu repeats) in %.2f sec (%.1f MB/s, CPU %.1f%%)\n",
            totalTransferBytes, m_repeatCount, totalTime,
            ((double)totalTransferBytes / (1024.0 * 1024.0)) / totalTime, avgCpuPct);

        // Save file to disk
        WCHAR outPath[MAX_PATH];
        // Extract just the filename from the path
        const WCHAR* pBaseName = wcsrchr(m_fileName, L'\\');
        if (pBaseName) pBaseName++; else pBaseName = m_fileName;

        swprintf_s(outPath, L".\\%s", pBaseName);
        printf("[SERVER] Saving to: %ls\n", outPath);

        HANDLE hFile = CreateFileW(outPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE)
        {
            printf("[SERVER] CreateFile failed: %lu\n", GetLastError());
        }
        else
        {
            UINT64 written = 0;
            while (written < m_fileSize)
            {
                DWORD toWrite = (DWORD)min(m_fileSize - written, (UINT64)(256 * 1024 * 1024));
                DWORD bytesWritten = 0;
                WriteFile(hFile, m_fileBuf + written, toWrite, &bytesWritten, nullptr);
                written += bytesWritten;
            }
            CloseHandle(hFile);
            printf("[SERVER] File saved (%llu bytes).\n", m_fileSize);
        }

        // Cleanup
        if (m_pFileMr) m_pFileMr->Release();
        if (m_fileBuf) VirtualFree(m_fileBuf, 0, MEM_RELEASE);
        NdTestBase::Shutdown();
    }

    ~FileReceiver()
    {
        if (m_ctrlBuf) HeapFree(GetProcessHeap(), 0, m_ctrlBuf);
    }

private:
    bool m_hack = false;
    char* m_ctrlBuf = nullptr;
    char* m_fileBuf = nullptr;
    IND2MemoryRegion* m_pFileMr = nullptr;
    WCHAR m_fileName[260] = { 0 };
    UINT64 m_fileSize = 0;
    UINT64 m_chunkSize = 0;
    UINT64 m_repeatCount = 1;
};

// ============================================================================
// Client: send file via RDMA Write to server
// ============================================================================

class FileSender : public NdTestClientBase
{
public:
    FileSender(SIZE_T chunkSize, UINT64 repeatCount) :
        m_chunkSize(chunkSize), m_repeatCount(repeatCount) {}

    // Required by NdTestClientBase - not used directly
    void RunTest(const struct sockaddr_in& v4Src, const struct sockaddr_in& v4Dst, DWORD, DWORD) override
    {
        // Not called directly
    }

    void RunTest(
        const struct sockaddr_in& v4Src,
        const struct sockaddr_in& v4Dst,
        const WCHAR* filePath)
    {
        printf("[CLIENT] Opening file: %ls\n", filePath);

        // Read the file into memory
        HANDLE hFile = CreateFileW(filePath, GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE)
        {
            printf("[CLIENT] Cannot open file: error %lu\n", GetLastError());
            exit(1);
        }

        LARGE_INTEGER liSize;
        GetFileSizeEx(hFile, &liSize);
        m_fileSize = liSize.QuadPart;
        printf("[CLIENT] File size: %llu bytes\n", m_fileSize);

        m_fileBuf = static_cast<char*>(VirtualAlloc(nullptr, m_fileSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!m_fileBuf)
        {
            printf("[CLIENT] VirtualAlloc failed: %lu\n", GetLastError());
            CloseHandle(hFile);
            exit(1);
        }

        // Read file into buffer
        UINT64 totalRead = 0;
        while (totalRead < m_fileSize)
        {
            DWORD toRead = (DWORD)min(m_fileSize - totalRead, (UINT64)(256 * 1024 * 1024));
            DWORD bytesRead = 0;
            ReadFile(hFile, m_fileBuf + totalRead, toRead, &bytesRead, nullptr);
            totalRead += bytesRead;
        }
        CloseHandle(hFile);
        printf("[CLIENT] File loaded into memory.\n");

        // Initialize ND
        printf("[CLIENT] Initializing adapter...\n");
        NdTestBase::Init(v4Src);

        ND2_ADAPTER_INFO adapterInfo = { 0 };
        NdTestBase::GetAdapterInfo(&adapterInfo);
        printf("[CLIENT] Adapter: MaxCQDepth=%lu, MaxInitQDepth=%lu\n",
            adapterInfo.MaxCompletionQueueDepth, adapterInfo.MaxInitiatorQueueDepth);

        DWORD queueDepth = min(adapterInfo.MaxCompletionQueueDepth, adapterInfo.MaxInitiatorQueueDepth);
        queueDepth = min(queueDepth, (DWORD)64);

        // Register file buffer for local read (RDMA Write source)
        NdTestBase::CreateMR();
        NdTestBase::RegisterDataBuffer(m_fileBuf, (DWORD)m_fileSize, ND_MR_FLAG_ALLOW_LOCAL_WRITE);

        // Also need a control buffer for messages
        m_ctrlBuf = static_cast<char*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
            sizeof(FileHeader) + sizeof(PeerInfo) + sizeof(ChunkAck)));
        if (!m_ctrlBuf)
        {
            printf("[CLIENT] Failed to allocate control buffer\n");
            exit(1);
        }

        HRESULT hr = m_pAdapter->CreateMemoryRegion(
            IID_IND2MemoryRegion, m_hAdapterFile, reinterpret_cast<void**>(&m_pCtrlMr));
        if (FAILED(hr))
        {
            printf("[CLIENT] CreateMemoryRegion for ctrl buffer failed: 0x%08x\n", hr);
            exit(1);
        }
        OVERLAPPED ov = { 0 };
        ov.hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        hr = m_pCtrlMr->Register(m_ctrlBuf,
            (DWORD)(sizeof(FileHeader) + sizeof(PeerInfo) + sizeof(ChunkAck)),
            ND_MR_FLAG_ALLOW_LOCAL_WRITE, &ov);
        if (hr == ND_PENDING) hr = m_pCtrlMr->GetOverlappedResult(&ov, TRUE);
        if (FAILED(hr))
        {
            printf("[CLIENT] Register ctrl buffer failed: 0x%08x\n", hr);
            exit(1);
        }
        CloseHandle(ov.hEvent);

        NdTestBase::CreateCQ(queueDepth);
        NdTestBase::CreateConnector();
        NdTestBase::CreateQueuePair(queueDepth, 1, adapterInfo.InlineRequestThreshold);

        // Post receive for PeerInfo from server
        ND2_SGE recvSge;
        recvSge.Buffer = m_ctrlBuf + sizeof(FileHeader);
        recvSge.BufferLength = sizeof(PeerInfo);
        recvSge.MemoryRegionToken = m_pCtrlMr->GetLocalToken();
        NdTestBase::PostReceive(&recvSge, 1, RECV_CTXT);

        // Connect
        printf("[CLIENT] Connecting to %d.%d.%d.%d:%d...\n",
            v4Dst.sin_addr.S_un.S_un_b.s_b1, v4Dst.sin_addr.S_un.S_un_b.s_b2,
            v4Dst.sin_addr.S_un.S_un_b.s_b3, v4Dst.sin_addr.S_un.S_un_b.s_b4,
            ntohs(v4Dst.sin_port));
        NdTestClientBase::Connect(v4Src, v4Dst, 0, 0);
        NdTestClientBase::CompleteConnect();
        printf("[CLIENT] Connected.\n");

        // Send FileHeader
        FileHeader* pHdr = reinterpret_cast<FileHeader*>(m_ctrlBuf);
        wcsncpy_s(pHdr->fileName, filePath, 259);
        pHdr->fileSize = m_fileSize;
        pHdr->chunkSize = m_chunkSize;
        pHdr->repeatCount = m_repeatCount;

        ND2_SGE sendSge;
        sendSge.Buffer = m_ctrlBuf;
        sendSge.BufferLength = sizeof(FileHeader);
        sendSge.MemoryRegionToken = m_pCtrlMr->GetLocalToken();
        NdTestBase::Send(&sendSge, 1, 0, SEND_CTXT);
        WaitForCompletionAndCheckContext(SEND_CTXT);
        printf("[CLIENT] FileHeader sent. Waiting for PeerInfo...\n");

        // Wait for PeerInfo from server
        WaitForCompletionAndCheckContext(RECV_CTXT);
        PeerInfo* pInfo = reinterpret_cast<PeerInfo*>(m_ctrlBuf + sizeof(FileHeader));
        UINT32 remoteToken = pInfo->remoteToken;
        UINT64 remoteAddress = pInfo->remoteAddress;
        printf("[CLIENT] PeerInfo received: token=0x%x, addr=0x%llx\n", remoteToken, remoteAddress);

        // Transfer file in chunks via RDMA Write, repeated N times
        UINT64 totalTransferSize = m_fileSize * m_repeatCount;
        UINT64 totalBytesWritten = 0;
        UINT64 numChunks = (m_fileSize + m_chunkSize - 1) / m_chunkSize;
        Timer timer;
        timer.Start();
        double startCpu = GetSystemCpuSec();

        printf("[CLIENT] Sending %llu chunks per pass x %llu repeats (%llu MB each)...\n",
            numChunks, m_repeatCount, m_chunkSize / (1024 * 1024));

        for (UINT64 rep = 0; rep < m_repeatCount; rep++)
        {
            UINT64 offset = 0;
            for (UINT64 chunk = 0; chunk < numChunks; chunk++)
            {
                UINT64 thisChunk = min(m_chunkSize, m_fileSize - offset);

                // RDMA Write this chunk to server's buffer at the right offset
                ND2_SGE writeSge;
                writeSge.Buffer = m_fileBuf + offset;
                writeSge.BufferLength = (ULONG)thisChunk;
                writeSge.MemoryRegionToken = m_pMr->GetLocalToken();

                HRESULT hr = m_pQp->Write(WRITE_CTXT, &writeSge, 1,
                    remoteAddress + offset, remoteToken, 0);
                if (FAILED(hr))
                {
                    printf("\n[CLIENT] QP::Write failed: 0x%08x\n", hr);
                    goto done;
                }

                // Wait for Write completion
                WaitForCompletionAndCheckContext(WRITE_CTXT);

                offset += thisChunk;
                totalBytesWritten += thisChunk;

                // Send chunk ACK to server
                ChunkAck* pAck = reinterpret_cast<ChunkAck*>(m_ctrlBuf + sizeof(FileHeader) + sizeof(PeerInfo));
                pAck->bytesWrittenSoFar = totalBytesWritten;

                ND2_SGE ackSge;
                ackSge.Buffer = pAck;
                ackSge.BufferLength = sizeof(ChunkAck);
                ackSge.MemoryRegionToken = m_pCtrlMr->GetLocalToken();
                NdTestBase::Send(&ackSge, 1, 0, SEND_CTXT);
                WaitForCompletionAndCheckContext(SEND_CTXT);

                timer.End();
                double elapsed = timer.Report() / 1000000.0;
                double cpuUsed = GetSystemCpuSec() - startCpu;
                double cpuPct = (elapsed > 0) ? (cpuUsed / elapsed) * 100.0 : 0;
                PrintProgress(totalBytesWritten, totalTransferSize, elapsed, cpuPct);
            }
        }

        done:
        timer.End();
        double totalTime = timer.Report() / 1000000.0;
        double totalCpu = GetSystemCpuSec() - startCpu;
        double avgCpuPct = (totalTime > 0) ? (totalCpu / totalTime) * 100.0 : 0;
        printf("[CLIENT] Transfer complete: %llu bytes (%llu repeats) in %.2f sec (%.1f MB/s, CPU %.1f%%)\n",
            totalTransferSize, m_repeatCount, totalTime,
            ((double)totalTransferSize / (1024.0 * 1024.0)) / totalTime, avgCpuPct);

        NdTestBase::Shutdown();
    }

    ~FileSender()
    {
        if (m_fileBuf) VirtualFree(m_fileBuf, 0, MEM_RELEASE);
        if (m_ctrlBuf) HeapFree(GetProcessHeap(), 0, m_ctrlBuf);
        if (m_pCtrlMr) m_pCtrlMr->Release();
    }

private:
    char* m_fileBuf = nullptr;
    char* m_ctrlBuf = nullptr;
    IND2MemoryRegion* m_pCtrlMr = nullptr;
    UINT64 m_fileSize = 0;
    SIZE_T m_chunkSize;
    UINT64 m_repeatCount;
};

// ============================================================================
// Main
// ============================================================================

int __cdecl _tmain(int argc, TCHAR* argv[])
{
    bool bServer = false;
    bool bClient = false;
    bool bHack = false;
    bool bNoProgress = false;
    struct sockaddr_in v4Server = { 0 };
    SIZE_T chunkSize = x_DefaultChunkSize;
    UINT64 repeatCount = 1;
    TCHAR* filePath = nullptr;

    INIT_LOG(TESTNAME);

    WSADATA wsaData;
    int ret = ::WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (ret != 0)
    {
        printf("Failed to initialize Windows Sockets: %d\n", ret);
        exit(1);
    }

    for (int i = 1; i < argc; i++)
    {
        TCHAR* arg = argv[i];
        if (wcscmp(arg, L"-s") == 0 || wcscmp(arg, L"-S") == 0)
        {
            bServer = true;
        }
        else if (wcscmp(arg, L"-c") == 0 || wcscmp(arg, L"-C") == 0)
        {
            bClient = true;
        }
        else if (wcscmp(arg, L"-chunk") == 0)
        {
            if (i + 1 < argc)
            {
                chunkSize = (SIZE_T)_ttol(argv[++i]) * 1024 * 1024;
            }
        }
        else if (wcscmp(arg, L"-n") == 0 || wcscmp(arg, L"-N") == 0)
        {
            if (i + 1 < argc)
            {
                repeatCount = (UINT64)_ttol(argv[++i]);
                if (repeatCount == 0) repeatCount = 1;
            }
        }
        else if (wcscmp(arg, L"-hack") == 0)
        {
            bHack = true;
        }
        else if (wcscmp(arg, L"-noprogress") == 0 || wcscmp(arg, L"-np") == 0)
        {
            bNoProgress = true;
        }
        else if ((wcscmp(arg, L"-h") == 0) || (wcscmp(arg, L"--help") == 0))
        {
            ShowUsage();
            exit(0);
        }
    }

    if ((bClient && bServer) || (!bClient && !bServer))
    {
        printf("Exactly one of -c (client) or -s (server) must be specified.\n");
        ShowUsage();
        exit(1);
    }

    // Parse IP address: second-to-last or last arg depending on client/server
    if (bClient)
    {
        // Find the IP and filepath args (skip flags)
        // IP is the arg after -c (or -chunk N), filepath is the last non-flag arg
        for (int i = 1; i < argc; i++)
        {
            TCHAR* arg = argv[i];
            if (arg[0] == L'-')
            {
                if (wcscmp(arg, L"-chunk") == 0 || wcscmp(arg, L"-n") == 0 || wcscmp(arg, L"-N") == 0) i++; // skip value
                continue;
            }
            // First non-flag = IP, second non-flag = filepath
            if (v4Server.sin_addr.s_addr == 0)
            {
                int len = sizeof(v4Server);
                WSAStringToAddress(arg, AF_INET, nullptr,
                    reinterpret_cast<struct sockaddr*>(&v4Server), &len);
            }
            else
            {
                filePath = arg;
            }
        }

        if (filePath == nullptr)
        {
            printf("Client mode requires a file path.\n");
            ShowUsage();
            exit(1);
        }
    }
    else
    {
        // Server: last non-flag arg is IP
        for (int i = 1; i < argc; i++)
        {
            TCHAR* arg = argv[i];
            if (arg[0] == L'-')
            {
                if (wcscmp(arg, L"-chunk") == 0 || wcscmp(arg, L"-n") == 0 || wcscmp(arg, L"-N") == 0) i++;
                continue;
            }
            int len = sizeof(v4Server);
            WSAStringToAddress(arg, AF_INET, nullptr,
                reinterpret_cast<struct sockaddr*>(&v4Server), &len);
        }
    }

    if (v4Server.sin_addr.s_addr == 0)
    {
        printf("Bad or missing address.\n");
        ShowUsage();
        exit(1);
    }

    if (v4Server.sin_port == 0)
    {
        v4Server.sin_port = htons(x_DefaultPort);
    }

    printf("transfer_file: %s mode, chunk=%llu MB, repeat=%llu\n",
        bServer ? "SERVER" : "CLIENT", chunkSize / (1024 * 1024), repeatCount);

    g_noProgress = bNoProgress;

    HRESULT hr = NdStartup();
    if (FAILED(hr))
    {
        printf("NdStartup failed: 0x%08x\n", hr);
        exit(1);
    }

    if (bServer)
    {
        FileReceiver server(bHack);
        server.RunTest(v4Server);
    }
    else
    {
        if (bHack)
        {
            // Hack mode: skip NdResolveAddress, use server address as both src and dst
            FileSender client(chunkSize, repeatCount);
            client.RunTest(v4Server, v4Server, filePath);
        }
        else
        {
            struct sockaddr_in v4Src;
            SIZE_T len = sizeof(v4Src);
            hr = NdResolveAddress(
                (const struct sockaddr*)&v4Server, sizeof(v4Server),
                (struct sockaddr*)&v4Src, &len);
            if (FAILED(hr))
            {
                printf("NdResolveAddress failed: 0x%08x\n", hr);
                exit(1);
            }
            printf("[CLIENT] Resolved local address: %d.%d.%d.%d\n",
                v4Src.sin_addr.S_un.S_un_b.s_b1, v4Src.sin_addr.S_un.S_un_b.s_b2,
                v4Src.sin_addr.S_un.S_un_b.s_b3, v4Src.sin_addr.S_un.S_un_b.s_b4);

            FileSender client(chunkSize, repeatCount);
            client.RunTest(v4Src, v4Server, filePath);
        }
    }

    NdCleanup();
    END_LOG(TESTNAME);
    _fcloseall();
    WSACleanup();
    return 0;
}
