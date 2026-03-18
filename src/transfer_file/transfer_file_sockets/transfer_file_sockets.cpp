//
// transfer_file_sockets.cpp - TCP file transfer for comparison with RDMA version
//
// Usage:
//   transfer_file_sockets -s <ip>[:<port>]
//   transfer_file_sockets -c <ip>[:<port>] <filepath> [-n <count>]
//

#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "ws2_32.lib")

const USHORT x_DefaultPort = 54500;
const DWORD  x_SendChunk = 256 * 1024;  // 256 KB per send() call

#pragma pack(push, 1)
struct FileHeader
{
    WCHAR fileName[260];
    UINT64 fileSize;
    UINT64 repeatCount;
};
#pragma pack(pop)

// ============================================================================
// Helpers
// ============================================================================

static double GetTimeSec()
{
    static LARGE_INTEGER freq = { 0 };
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart / (double)freq.QuadPart;
}

// Returns system-wide CPU usage: total time spent across all cores (kernel + user) in seconds
static double GetSystemCpuSec()
{
    FILETIME ftIdle, ftKernel, ftUser;
    GetSystemTimes(&ftIdle, &ftKernel, &ftUser);
    ULARGE_INTEGER kernel, user;
    kernel.LowPart = ftKernel.dwLowDateTime; kernel.HighPart = ftKernel.dwHighDateTime;
    user.LowPart = ftUser.dwLowDateTime; user.HighPart = ftUser.dwHighDateTime;
    // kernel time includes idle time, so total busy = (kernel - idle) + user
    ULARGE_INTEGER idle;
    idle.LowPart = ftIdle.dwLowDateTime; idle.HighPart = ftIdle.dwHighDateTime;
    return (double)(kernel.QuadPart - idle.QuadPart + user.QuadPart) / 10000000.0;
}

static void PrintProgress(UINT64 current, UINT64 total, double elapsedSec, double cpuPct)
{
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

// Send exactly n bytes over TCP
static bool SendAll(SOCKET s, const char* buf, int len)
{
    int total = 0;
    while (total < len)
    {
        int ret = send(s, buf + total, len - total, 0);
        if (ret == SOCKET_ERROR || ret == 0) return false;
        total += ret;
    }
    return true;
}

// Receive exactly n bytes over TCP
static bool RecvAll(SOCKET s, char* buf, int len)
{
    int total = 0;
    while (total < len)
    {
        int ret = recv(s, buf + total, len - total, 0);
        if (ret == SOCKET_ERROR || ret == 0) return false;
        total += ret;
    }
    return true;
}

void ShowUsage()
{
    printf("transfer_file_sockets [options] <ip>[:<port>] [filepath]\n"
        "Options:\n"
        "\t-s              - Start as server (receive file)\n"
        "\t-c              - Start as client (send file)\n"
        "\t-n <count>      - Repeat transfer N times (default: 1)\n"
        "<ip>              - IPv4 Address\n"
        "<port>            - Port number (default: %hu)\n"
        "<filepath>        - Path to file to send (client only)\n",
        x_DefaultPort
    );
}

// ============================================================================
// Server
// ============================================================================

static int RunServer(const struct sockaddr_in& addr)
{
    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock == INVALID_SOCKET)
    {
        printf("[SERVER] socket() failed: %d\n", WSAGetLastError());
        return 1;
    }

    int yes = 1;
    setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, (char*)&yes, sizeof(yes));

    if (bind(listenSock, (const sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR)
    {
        printf("[SERVER] bind() failed: %d\n", WSAGetLastError());
        closesocket(listenSock);
        return 1;
    }

    if (listen(listenSock, 1) == SOCKET_ERROR)
    {
        printf("[SERVER] listen() failed: %d\n", WSAGetLastError());
        closesocket(listenSock);
        return 1;
    }

    printf("[SERVER] Listening on %d.%d.%d.%d:%d (TCP)...\n",
        addr.sin_addr.S_un.S_un_b.s_b1, addr.sin_addr.S_un.S_un_b.s_b2,
        addr.sin_addr.S_un.S_un_b.s_b3, addr.sin_addr.S_un.S_un_b.s_b4,
        ntohs(addr.sin_port));

    struct sockaddr_in clientAddr;
    int clientAddrLen = sizeof(clientAddr);
    SOCKET clientSock = accept(listenSock, (sockaddr*)&clientAddr, &clientAddrLen);
    if (clientSock == INVALID_SOCKET)
    {
        printf("[SERVER] accept() failed: %d\n", WSAGetLastError());
        closesocket(listenSock);
        return 1;
    }
    printf("[SERVER] Client connected.\n");

    // Receive FileHeader
    FileHeader hdr;
    if (!RecvAll(clientSock, (char*)&hdr, sizeof(hdr)))
    {
        printf("[SERVER] Failed to receive file header\n");
        closesocket(clientSock);
        closesocket(listenSock);
        return 1;
    }

    UINT64 fileSize = hdr.fileSize;
    UINT64 repeatCount = hdr.repeatCount;
    UINT64 totalTransferSize = fileSize * repeatCount;

    const WCHAR* pBaseName = wcsrchr(hdr.fileName, L'\\');
    if (pBaseName) pBaseName++; else pBaseName = hdr.fileName;

    printf("[SERVER] Incoming file: %ls (%llu bytes, repeat=%llu, total=%llu bytes)\n",
        pBaseName, fileSize, repeatCount, totalTransferSize);

    // Allocate file buffer
    char* fileBuf = static_cast<char*>(VirtualAlloc(nullptr, fileSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!fileBuf)
    {
        printf("[SERVER] VirtualAlloc failed: %lu\n", GetLastError());
        closesocket(clientSock);
        closesocket(listenSock);
        return 1;
    }

    printf("[SERVER] Receiving data...\n");
    double startTime = GetTimeSec();
    double startCpu = GetSystemCpuSec();
    UINT64 totalReceived = 0;
    double lastProgressTime = startTime;

    for (UINT64 rep = 0; rep < repeatCount; rep++)
    {
        UINT64 fileOffset = 0;
        while (fileOffset < fileSize)
        {
            DWORD toRecv = (DWORD)min(fileSize - fileOffset, (UINT64)x_SendChunk);
            int ret = recv(clientSock, fileBuf + fileOffset, toRecv, 0);
            if (ret <= 0)
            {
                printf("\n[SERVER] recv failed: %d\n", WSAGetLastError());
                goto server_done;
            }
            fileOffset += ret;
            totalReceived += ret;

            double now = GetTimeSec();
            if (now - lastProgressTime >= 2.0 || totalReceived >= totalTransferSize)
            {
                double elapsed = now - startTime;
                double cpuUsed = GetSystemCpuSec() - startCpu;
                double cpuPct = (elapsed > 0) ? (cpuUsed / elapsed) * 100.0 : 0;
                PrintProgress(totalReceived, totalTransferSize, elapsed, cpuPct);
                lastProgressTime = now;
            }
        }
    }

server_done:
    double totalTime = GetTimeSec() - startTime;
    double totalCpu = GetSystemCpuSec() - startCpu;
    double avgCpuPct = (totalTime > 0) ? (totalCpu / totalTime) * 100.0 : 0;
    printf("[SERVER] Transfer complete: %llu bytes (%llu repeats) in %.2f sec (%.1f MB/s, CPU %.1f%%)\n",
        totalTransferSize, repeatCount, totalTime,
        ((double)totalTransferSize / (1024.0 * 1024.0)) / totalTime, avgCpuPct);

    // Save file
    WCHAR outPath[MAX_PATH];
    swprintf_s(outPath, L".\\%s", pBaseName);
    printf("[SERVER] Saving to: %ls\n", outPath);

    HANDLE hFile = CreateFileW(outPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE)
    {
        UINT64 written = 0;
        while (written < fileSize)
        {
            DWORD toWrite = (DWORD)min(fileSize - written, (UINT64)(256 * 1024 * 1024));
            DWORD bytesWritten = 0;
            WriteFile(hFile, fileBuf + written, toWrite, &bytesWritten, nullptr);
            written += bytesWritten;
        }
        CloseHandle(hFile);
        printf("[SERVER] File saved (%llu bytes).\n", fileSize);
    }
    else
    {
        printf("[SERVER] CreateFile failed: %lu\n", GetLastError());
    }

    VirtualFree(fileBuf, 0, MEM_RELEASE);
    closesocket(clientSock);
    closesocket(listenSock);
    return 0;
}

// ============================================================================
// Client
// ============================================================================

static int RunClient(const struct sockaddr_in& serverAddr, const WCHAR* filePath, UINT64 repeatCount)
{
    printf("[CLIENT] Opening file: %ls\n", filePath);

    HANDLE hFile = CreateFileW(filePath, GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        printf("[CLIENT] Cannot open file: error %lu\n", GetLastError());
        return 1;
    }

    LARGE_INTEGER liSize;
    GetFileSizeEx(hFile, &liSize);
    UINT64 fileSize = liSize.QuadPart;
    printf("[CLIENT] File size: %llu bytes\n", fileSize);

    char* fileBuf = static_cast<char*>(VirtualAlloc(nullptr, fileSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!fileBuf)
    {
        printf("[CLIENT] VirtualAlloc failed: %lu\n", GetLastError());
        CloseHandle(hFile);
        return 1;
    }

    UINT64 totalRead = 0;
    while (totalRead < fileSize)
    {
        DWORD toRead = (DWORD)min(fileSize - totalRead, (UINT64)(256 * 1024 * 1024));
        DWORD bytesRead = 0;
        ReadFile(hFile, fileBuf + totalRead, toRead, &bytesRead, nullptr);
        totalRead += bytesRead;
    }
    CloseHandle(hFile);
    printf("[CLIENT] File loaded into memory.\n");

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET)
    {
        printf("[CLIENT] socket() failed: %d\n", WSAGetLastError());
        VirtualFree(fileBuf, 0, MEM_RELEASE);
        return 1;
    }

    // Increase send buffer
    int sndBuf = 4 * 1024 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (char*)&sndBuf, sizeof(sndBuf));

    // Disable Nagle for maximum throughput
    int noDelay = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char*)&noDelay, sizeof(noDelay));

    printf("[CLIENT] Connecting to %d.%d.%d.%d:%d...\n",
        serverAddr.sin_addr.S_un.S_un_b.s_b1, serverAddr.sin_addr.S_un.S_un_b.s_b2,
        serverAddr.sin_addr.S_un.S_un_b.s_b3, serverAddr.sin_addr.S_un.S_un_b.s_b4,
        ntohs(serverAddr.sin_port));

    if (connect(sock, (const sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR)
    {
        printf("[CLIENT] connect() failed: %d\n", WSAGetLastError());
        closesocket(sock);
        VirtualFree(fileBuf, 0, MEM_RELEASE);
        return 1;
    }
    printf("[CLIENT] Connected.\n");

    // Send FileHeader
    FileHeader hdr = { 0 };
    wcsncpy_s(hdr.fileName, filePath, 259);
    hdr.fileSize = fileSize;
    hdr.repeatCount = repeatCount;

    if (!SendAll(sock, (char*)&hdr, sizeof(hdr)))
    {
        printf("[CLIENT] Failed to send header\n");
        closesocket(sock);
        VirtualFree(fileBuf, 0, MEM_RELEASE);
        return 1;
    }

    UINT64 totalTransferSize = fileSize * repeatCount;
    UINT64 totalBytesSent = 0;

    double startTime = GetTimeSec();
    double startCpu = GetSystemCpuSec();
    double lastProgressTime = startTime;

    printf("[CLIENT] Sending data (%llu repeats)...\n", repeatCount);

    for (UINT64 rep = 0; rep < repeatCount; rep++)
    {
        UINT64 fileOffset = 0;
        while (fileOffset < fileSize)
        {
            DWORD toSend = (DWORD)min(fileSize - fileOffset, (UINT64)x_SendChunk);
            int ret = send(sock, fileBuf + fileOffset, toSend, 0);
            if (ret == SOCKET_ERROR)
            {
                printf("\n[CLIENT] send failed: %d\n", WSAGetLastError());
                goto client_done;
            }
            fileOffset += ret;
            totalBytesSent += ret;

            double now = GetTimeSec();
            if (now - lastProgressTime >= 2.0)
            {
                double elapsed = now - startTime;
                double cpuUsed = GetSystemCpuSec() - startCpu;
                double cpuPct = (elapsed > 0) ? (cpuUsed / elapsed) * 100.0 : 0;
                PrintProgress(totalBytesSent, totalTransferSize, elapsed, cpuPct);
                lastProgressTime = now;
            }
        }
    }
    {
        double elapsed = GetTimeSec() - startTime;
        double cpuUsed = GetSystemCpuSec() - startCpu;
        double cpuPct = (elapsed > 0) ? (cpuUsed / elapsed) * 100.0 : 0;
        PrintProgress(totalBytesSent, totalTransferSize, elapsed, cpuPct);
    }

client_done:
    double totalTime = GetTimeSec() - startTime;
    double totalCpu = GetSystemCpuSec() - startCpu;
    double avgCpuPct = (totalTime > 0) ? (totalCpu / totalTime) * 100.0 : 0;
    printf("[CLIENT] Transfer complete: %llu bytes (%llu repeats) in %.2f sec (%.1f MB/s, CPU %.1f%%)\n",
        totalTransferSize, repeatCount, totalTime,
        ((double)totalTransferSize / (1024.0 * 1024.0)) / totalTime, avgCpuPct);

    VirtualFree(fileBuf, 0, MEM_RELEASE);
    closesocket(sock);
    return 0;
}

// ============================================================================
// Main
// ============================================================================

int wmain(int argc, WCHAR* argv[])
{
    bool bServer = false;
    bool bClient = false;
    struct sockaddr_in v4Server = { 0 };
    UINT64 repeatCount = 1;
    WCHAR* filePath = nullptr;

    WSADATA wsaData;
    int ret = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (ret != 0)
    {
        printf("WSAStartup failed: %d\n", ret);
        return 1;
    }

    for (int i = 1; i < argc; i++)
    {
        WCHAR* arg = argv[i];
        if (wcscmp(arg, L"-s") == 0 || wcscmp(arg, L"-S") == 0)
            bServer = true;
        else if (wcscmp(arg, L"-c") == 0 || wcscmp(arg, L"-C") == 0)
            bClient = true;
        else if (wcscmp(arg, L"-n") == 0 || wcscmp(arg, L"-N") == 0)
        {
            if (i + 1 < argc)
            {
                repeatCount = (UINT64)_wtol(argv[++i]);
                if (repeatCount == 0) repeatCount = 1;
            }
        }
        else if (wcscmp(arg, L"-h") == 0 || wcscmp(arg, L"--help") == 0)
        {
            ShowUsage();
            return 0;
        }
    }

    if ((bClient && bServer) || (!bClient && !bServer))
    {
        printf("Exactly one of -c (client) or -s (server) must be specified.\n");
        ShowUsage();
        return 1;
    }

    if (bClient)
    {
        for (int i = 1; i < argc; i++)
        {
            WCHAR* arg = argv[i];
            if (arg[0] == L'-')
            {
                if (wcscmp(arg, L"-n") == 0 || wcscmp(arg, L"-N") == 0) i++;
                continue;
            }
            if (v4Server.sin_addr.s_addr == 0)
            {
                int len = sizeof(v4Server);
                WSAStringToAddressW(arg, AF_INET, nullptr, (sockaddr*)&v4Server, &len);
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
            return 1;
        }
    }
    else
    {
        for (int i = 1; i < argc; i++)
        {
            WCHAR* arg = argv[i];
            if (arg[0] == L'-')
            {
                if (wcscmp(arg, L"-n") == 0 || wcscmp(arg, L"-N") == 0) i++;
                continue;
            }
            int len = sizeof(v4Server);
            WSAStringToAddressW(arg, AF_INET, nullptr, (sockaddr*)&v4Server, &len);
        }
    }

    if (v4Server.sin_addr.s_addr == 0)
    {
        printf("Bad or missing address.\n");
        ShowUsage();
        return 1;
    }

    if (v4Server.sin_port == 0)
        v4Server.sin_port = htons(x_DefaultPort);

    printf("transfer_file_sockets: %s mode, repeat=%llu (TCP)\n",
        bServer ? "SERVER" : "CLIENT", repeatCount);

    int result;
    if (bServer)
        result = RunServer(v4Server);
    else
        result = RunClient(v4Server, filePath, repeatCount);

    WSACleanup();
    return result;
}

