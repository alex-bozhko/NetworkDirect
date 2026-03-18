// share_screen.cpp : RDMA-based screen sharing application using NetworkDirect IND2.
//
// Usage:
//   share_screen.exe -s <ip>[:<port>]   Server mode (receive and display)
//   share_screen.exe -c <ip>[:<port>]   Client mode (capture and send)
//   -hack                               Skip NdResolveAddress (use same IP for src and dst)
//
// Default port: 54600

#include "framework.h"
#include "share_screen.h"

#include "ndcommon.h"
#include "ndtestutil.h"
#include <stdio.h>
#include <process.h>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
#define MAX_LOADSTRING   100
static const USHORT DEFAULT_PORT = 54600;
static const UINT   TIMER_ID    = 1;
static const UINT   TIMER_MS    = 33; // ~30 fps

#define RECV_CTXT  ((void*)0x1000)
#define SEND_CTXT  ((void*)0x2000)
#define WRITE_CTXT ((void*)0x4000)

static const WCHAR WINDOW_CLASS[] = L"ShareScreenClass";

// ---------------------------------------------------------------------------
// Wire protocol structures
// ---------------------------------------------------------------------------
#pragma pack(push, 1)
struct FrameInfo {
    UINT32 width;
    UINT32 height;
    UINT32 bpp; // always 32
};

struct PeerInfo {
    UINT32 remoteToken;
    UINT64 remoteAddress;
};
#pragma pack(pop)

// ---------------------------------------------------------------------------
// Debug helper
// ---------------------------------------------------------------------------
static void DbgPrint(const char* fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsprintf_s(buf, fmt, ap);
    va_end(ap);
    OutputDebugStringA(buf);
}

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
static HINSTANCE g_hInst;
static HWND      g_hWnd;

// Shared framebuffer (server renders from it, client writes into it before RDMA)
static BYTE*     g_pFramebuffer   = nullptr;
static UINT32    g_frameWidth     = 0;
static UINT32    g_frameHeight    = 0;
static SIZE_T    g_frameBytes     = 0;

static volatile BOOL g_bRunning   = TRUE;

// Mode
enum AppMode { MODE_NONE, MODE_SERVER, MODE_CLIENT };
static AppMode   g_mode           = MODE_NONE;

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static ATOM  RegisterWindowClass(HINSTANCE hInstance);
static HWND  CreateAppWindow(HINSTANCE hInstance, int w, int h, const WCHAR* title);
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
static unsigned __stdcall ServerNdThread(void* param);
static unsigned __stdcall ClientNdThread(void* param);

// ---------------------------------------------------------------------------
// Server ND implementation
// ---------------------------------------------------------------------------
class ScreenServer : public NdTestServerBase
{
public:
    void RunTest(
        _In_ const struct sockaddr_in& v4Src,
        _In_ DWORD /*queueDepth*/,
        _In_ DWORD /*nSge*/) override
    {
        // stub — we drive the test from ServerNdThread directly
        UNREFERENCED_PARAMETER(v4Src);
    }

    void Run(const struct sockaddr_in& v4Src)
    {
        DbgPrint("[SERVER] Initializing adapter...\n");
        NdTestBase::Init(v4Src);

        ND2_ADAPTER_INFO ai = {};
        NdTestBase::GetAdapterInfo(&ai);
        DbgPrint("[SERVER] Adapter: MaxCQDepth=%lu MaxRecvQ=%lu\n",
            ai.MaxCompletionQueueDepth, ai.MaxReceiveQueueDepth);

        DWORD cqDepth = min(ai.MaxCompletionQueueDepth, ai.MaxReceiveQueueDepth);
        cqDepth = min(cqDepth, (DWORD)64);

        NdTestBase::CreateCQ(cqDepth);
        NdTestBase::CreateConnector();
        NdTestBase::CreateQueuePair(cqDepth, 1);

        // Allocate a small control buffer for Send/Recv exchange
        const DWORD ctrlBufLen = 256;
        m_pCtrl = (char*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, ctrlBufLen);
        NdTestBase::CreateMR();
        NdTestBase::RegisterDataBuffer(m_pCtrl, ctrlBufLen,
            ND_MR_FLAG_ALLOW_LOCAL_WRITE);

        // Post receive for FrameInfo from client
        ND2_SGE sge = {};
        sge.Buffer = m_pCtrl;
        sge.BufferLength = ctrlBufLen;
        sge.MemoryRegionToken = m_pMr->GetLocalToken();
        NdTestBase::PostReceive(&sge, 1, RECV_CTXT);

        // Listen
        NdTestServerBase::CreateListener();
        NdTestServerBase::Listen(v4Src);
        DbgPrint("[SERVER] Listening on port %d...\n", ntohs(v4Src.sin_port));
        NdTestServerBase::GetConnectionRequest();
        NdTestServerBase::Accept(0, 0);
        DbgPrint("[SERVER] Client connected.\n");

        // Wait for FrameInfo
        WaitForCompletionAndCheckContext(RECV_CTXT);
        FrameInfo* pFI = reinterpret_cast<FrameInfo*>(m_pCtrl);
        g_frameWidth  = pFI->width;
        g_frameHeight = pFI->height;
        g_frameBytes  = (SIZE_T)g_frameWidth * g_frameHeight * 4;
        DbgPrint("[SERVER] Received FrameInfo: %ux%u bpp=%u (%zu bytes)\n",
            g_frameWidth, g_frameHeight, pFI->bpp, g_frameBytes);

        // Allocate framebuffer with VirtualAlloc for alignment
        g_pFramebuffer = (BYTE*)VirtualAlloc(nullptr, g_frameBytes,
            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!g_pFramebuffer) {
            DbgPrint("[SERVER] VirtualAlloc failed for framebuffer!\n");
            return;
        }

        // We need a second MR for the framebuffer (the first MR is for the ctrl buffer)
        HRESULT hr = m_pAdapter->CreateMemoryRegion(
            IID_IND2MemoryRegion, m_hAdapterFile,
            reinterpret_cast<VOID**>(&m_pFbMr));
        if (FAILED(hr)) {
            DbgPrint("[SERVER] CreateMemoryRegion for FB failed: 0x%08x\n", hr);
            return;
        }
        OVERLAPPED ov = {};
        ov.hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        hr = m_pFbMr->Register(g_pFramebuffer, (DWORD)g_frameBytes,
            ND_MR_FLAG_ALLOW_LOCAL_WRITE | ND_MR_FLAG_ALLOW_REMOTE_WRITE, &ov);
        if (hr == ND_PENDING) {
            hr = m_pFbMr->GetOverlappedResult(&ov, TRUE);
        }
        CloseHandle(ov.hEvent);
        if (FAILED(hr)) {
            DbgPrint("[SERVER] Register FB MR failed: 0x%08x\n", hr);
            return;
        }
        DbgPrint("[SERVER] Framebuffer MR registered: remoteToken=0x%x addr=0x%llx\n",
            m_pFbMr->GetRemoteToken(), (UINT64)g_pFramebuffer);

        // Send PeerInfo back to client
        PeerInfo* pPI = reinterpret_cast<PeerInfo*>(m_pCtrl);
        pPI->remoteToken   = m_pFbMr->GetRemoteToken();
        pPI->remoteAddress = reinterpret_cast<UINT64>(g_pFramebuffer);
        NdTestBase::Send(&sge, 1, 0, SEND_CTXT);
        WaitForCompletionAndCheckContext(SEND_CTXT);
        DbgPrint("[SERVER] PeerInfo sent.\n");

        // Post a receive for frame-ready notifications
        NdTestBase::PostReceive(&sge, 1, RECV_CTXT);

        // Signal main thread that window can be created
        SetEvent(m_hReady);

        // Block-wait for frame-ready notifications
        while (g_bRunning) {
            ND2_RESULT ndRes;
            WaitForCompletion(&ndRes, true);
            if (ndRes.Status != ND_SUCCESS) {
                if (ndRes.Status == ND_CANCELED)
                    break;
                DbgPrint("[SERVER] Completion error: 0x%08x\n", ndRes.Status);
                break;
            }
            if (ndRes.RequestContext == RECV_CTXT) {
                // Frame ready — invalidate window and repost receive
                if (g_hWnd) {
                    InvalidateRect(g_hWnd, nullptr, FALSE);
                }
                NdTestBase::PostReceive(&sge, 1, RECV_CTXT);
            }
        }

        // Cleanup
        DbgPrint("[SERVER] Shutting down ND...\n");
        if (m_pFbMr) {
            OVERLAPPED dov = {};
            dov.hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
            m_pFbMr->Deregister(&dov);
            m_pFbMr->GetOverlappedResult(&dov, TRUE);
            CloseHandle(dov.hEvent);
            m_pFbMr->Release();
            m_pFbMr = nullptr;
        }
        NdTestBase::Shutdown();
    }

    HANDLE m_hReady = nullptr;

private:
    char* m_pCtrl = nullptr;
    IND2MemoryRegion* m_pFbMr = nullptr;
};

// ---------------------------------------------------------------------------
// Client ND implementation
// ---------------------------------------------------------------------------
class ScreenClient : public NdTestClientBase
{
public:
    void RunTest(
        _In_ const struct sockaddr_in& v4Src,
        _In_ const struct sockaddr_in& v4Dst,
        _In_ DWORD /*queueDepth*/,
        _In_ DWORD /*nSge*/) override
    {
        // stub
        UNREFERENCED_PARAMETER(v4Src);
        UNREFERENCED_PARAMETER(v4Dst);
    }

    void Run(const struct sockaddr_in& v4Src, const struct sockaddr_in& v4Dst)
    {
        DbgPrint("[CLIENT] Initializing adapter...\n");
        NdTestBase::Init(v4Src);

        ND2_ADAPTER_INFO ai = {};
        NdTestBase::GetAdapterInfo(&ai);
        DWORD cqDepth = min(ai.MaxCompletionQueueDepth, ai.MaxInitiatorQueueDepth);
        cqDepth = min(cqDepth, (DWORD)64);

        NdTestBase::CreateCQ(cqDepth);
        NdTestBase::CreateConnector();
        NdTestBase::CreateQueuePair(cqDepth, 1);

        // Ctrl buffer for Send/Recv
        const DWORD ctrlBufLen = 256;
        m_pCtrl = (char*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, ctrlBufLen);
        NdTestBase::CreateMR();
        NdTestBase::RegisterDataBuffer(m_pCtrl, ctrlBufLen,
            ND_MR_FLAG_ALLOW_LOCAL_WRITE);

        // Post receive for PeerInfo from server
        ND2_SGE ctrlSge = {};
        ctrlSge.Buffer = m_pCtrl;
        ctrlSge.BufferLength = ctrlBufLen;
        ctrlSge.MemoryRegionToken = m_pMr->GetLocalToken();
        NdTestBase::PostReceive(&ctrlSge, 1, RECV_CTXT);

        // Connect
        DbgPrint("[CLIENT] Connecting...\n");
        NdTestClientBase::Connect(v4Src, v4Dst, 0, 0);
        NdTestClientBase::CompleteConnect();
        DbgPrint("[CLIENT] Connected.\n");

        // Capture screen dimensions
        g_frameWidth  = (UINT32)GetSystemMetrics(SM_CXSCREEN);
        g_frameHeight = (UINT32)GetSystemMetrics(SM_CYSCREEN);
        g_frameBytes  = (SIZE_T)g_frameWidth * g_frameHeight * 4;
        DbgPrint("[CLIENT] Screen: %ux%u (%zu bytes)\n",
            g_frameWidth, g_frameHeight, g_frameBytes);

        // Send FrameInfo
        FrameInfo* pFI = reinterpret_cast<FrameInfo*>(m_pCtrl);
        pFI->width  = g_frameWidth;
        pFI->height = g_frameHeight;
        pFI->bpp    = 32;
        NdTestBase::Send(&ctrlSge, 1, 0, SEND_CTXT);
        WaitForCompletionAndCheckContext(SEND_CTXT);
        DbgPrint("[CLIENT] FrameInfo sent.\n");

        // Wait for PeerInfo
        WaitForCompletionAndCheckContext(RECV_CTXT);
        PeerInfo* pPI = reinterpret_cast<PeerInfo*>(m_pCtrl);
        m_remoteToken   = pPI->remoteToken;
        m_remoteAddress = pPI->remoteAddress;
        DbgPrint("[CLIENT] PeerInfo received: token=0x%x addr=0x%llx\n",
            m_remoteToken, m_remoteAddress);

        // Allocate local capture buffer
        m_pCapture = (BYTE*)VirtualAlloc(nullptr, g_frameBytes,
            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!m_pCapture) {
            DbgPrint("[CLIENT] VirtualAlloc for capture buffer failed!\n");
            return;
        }

        // Create a second MR for the capture buffer
        HRESULT hr = m_pAdapter->CreateMemoryRegion(
            IID_IND2MemoryRegion, m_hAdapterFile,
            reinterpret_cast<VOID**>(&m_pCapMr));
        if (FAILED(hr)) {
            DbgPrint("[CLIENT] CreateMR for capture failed: 0x%08x\n", hr);
            return;
        }
        OVERLAPPED ov = {};
        ov.hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        hr = m_pCapMr->Register(m_pCapture, (DWORD)g_frameBytes,
            ND_MR_FLAG_ALLOW_LOCAL_WRITE, &ov);
        if (hr == ND_PENDING) {
            hr = m_pCapMr->GetOverlappedResult(&ov, TRUE);
        }
        CloseHandle(ov.hEvent);
        if (FAILED(hr)) {
            DbgPrint("[CLIENT] Register capture MR failed: 0x%08x\n", hr);
            return;
        }
        DbgPrint("[CLIENT] Capture MR registered: localToken=0x%x\n",
            m_pCapMr->GetLocalToken());

        // Signal main thread — ready to show status window
        SetEvent(m_hReady);

        // Prepare screen capture resources
        HDC hdcScreen = GetDC(nullptr);
        HDC hdcMem = CreateCompatibleDC(hdcScreen);

        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth       = (LONG)g_frameWidth;
        bmi.bmiHeader.biHeight      = -(LONG)g_frameHeight; // top-down for capture
        bmi.bmiHeader.biPlanes      = 1;
        bmi.bmiHeader.biBitCount    = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* pDibBits = nullptr;
        HBITMAP hDib = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, &pDibBits, nullptr, 0);
        HGDIOBJ hOld = SelectObject(hdcMem, hDib);

        // Notification byte buffer (reuse ctrl buffer area)
        char notifyByte = 1;

        // We need an MR for the notification send too — reuse m_pCtrl/m_pMr since
        // ctrl Send/Recv phase is done. Prepare the notify SGE.
        ND2_SGE notifySge = {};
        notifySge.Buffer = m_pCtrl;
        notifySge.BufferLength = 1;
        notifySge.MemoryRegionToken = m_pMr->GetLocalToken();

        // Capture loop
        LARGE_INTEGER freq, lastTime;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&lastTime);
        const double targetFrameUs = 33333.0; // ~30fps

        while (g_bRunning) {
            // Capture screen
            BitBlt(hdcMem, 0, 0, (int)g_frameWidth, (int)g_frameHeight,
                hdcScreen, 0, 0, SRCCOPY);

            // Copy DIB bits into our registered capture buffer.
            // The DIB is top-down but RDMA destination (server) expects bottom-up for
            // SetDIBitsToDevice with positive biHeight. Flip rows.
            const DWORD rowBytes = g_frameWidth * 4;
            for (UINT32 y = 0; y < g_frameHeight; y++) {
                const BYTE* srcRow = (const BYTE*)pDibBits + (SIZE_T)y * rowBytes;
                BYTE* dstRow = m_pCapture + (SIZE_T)(g_frameHeight - 1 - y) * rowBytes;
                memcpy(dstRow, srcRow, rowBytes);
            }

            // RDMA Write the capture buffer to server's framebuffer
            ND2_SGE writeSge = {};
            writeSge.Buffer = m_pCapture;
            writeSge.BufferLength = (DWORD)g_frameBytes;
            writeSge.MemoryRegionToken = m_pCapMr->GetLocalToken();
            NdTestBase::Write(&writeSge, 1, m_remoteAddress, m_remoteToken, 0, WRITE_CTXT);

            // Wait for write completion
            WaitForCompletionAndCheckContext(WRITE_CTXT);

            // Send frame-ready notification (1-byte send)
            m_pCtrl[0] = 1;
            NdTestBase::Send(&notifySge, 1, 0, SEND_CTXT);
            WaitForCompletionAndCheckContext(SEND_CTXT);

            // Frame rate limiter
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            double elapsedUs = (double)(now.QuadPart - lastTime.QuadPart) * 1000000.0 / (double)freq.QuadPart;
            if (elapsedUs < targetFrameUs) {
                DWORD sleepMs = (DWORD)((targetFrameUs - elapsedUs) / 1000.0);
                if (sleepMs > 0) Sleep(sleepMs);
            }
            QueryPerformanceCounter(&lastTime);
        }

        // Cleanup GDI
        SelectObject(hdcMem, hOld);
        DeleteObject(hDib);
        DeleteDC(hdcMem);
        ReleaseDC(nullptr, hdcScreen);

        // Cleanup ND
        DbgPrint("[CLIENT] Shutting down ND...\n");
        if (m_pCapMr) {
            OVERLAPPED dov = {};
            dov.hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
            m_pCapMr->Deregister(&dov);
            m_pCapMr->GetOverlappedResult(&dov, TRUE);
            CloseHandle(dov.hEvent);
            m_pCapMr->Release();
            m_pCapMr = nullptr;
        }
        if (m_pCapture) {
            VirtualFree(m_pCapture, 0, MEM_RELEASE);
            m_pCapture = nullptr;
        }
        NdTestBase::Shutdown();
    }

    HANDLE m_hReady = nullptr;

private:
    char*  m_pCtrl       = nullptr;
    BYTE*  m_pCapture    = nullptr;
    IND2MemoryRegion* m_pCapMr = nullptr;
    UINT32 m_remoteToken   = 0;
    UINT64 m_remoteAddress = 0;
};

// ---------------------------------------------------------------------------
// Thread context
// ---------------------------------------------------------------------------
struct ThreadCtx {
    sockaddr_in addr;
    sockaddr_in srcAddr;
    HANDLE      hReady;
};

static ScreenServer* g_pServer = nullptr;
static ScreenClient* g_pClient = nullptr;

static unsigned __stdcall ServerNdThread(void* param)
{
    ThreadCtx* ctx = (ThreadCtx*)param;
    g_pServer = new ScreenServer();
    g_pServer->m_hReady = ctx->hReady;
    g_pServer->Run(ctx->addr);
    delete g_pServer;
    g_pServer = nullptr;
    return 0;
}

static unsigned __stdcall ClientNdThread(void* param)
{
    ThreadCtx* ctx = (ThreadCtx*)param;
    g_pClient = new ScreenClient();
    g_pClient->m_hReady = ctx->hReady;
    g_pClient->Run(ctx->srcAddr, ctx->addr);
    delete g_pClient;
    g_pClient = nullptr;
    return 0;
}

// ---------------------------------------------------------------------------
// Command-line parsing
// ---------------------------------------------------------------------------
static bool ParseCmdLine(
    LPWSTR lpCmdLine,
    AppMode& mode,
    sockaddr_in& addr,
    bool& hack)
{
    mode = MODE_NONE;
    hack = false;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(DEFAULT_PORT);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(lpCmdLine, &argc);
    if (!argv) return false;

    WCHAR ipStr[256] = {};
    for (int i = 0; i < argc; i++) {
        if (_wcsicmp(argv[i], L"-s") == 0) {
            mode = MODE_SERVER;
            if (i + 1 < argc) {
                wcscpy_s(ipStr, argv[++i]);
            }
        }
        else if (_wcsicmp(argv[i], L"-c") == 0) {
            mode = MODE_CLIENT;
            if (i + 1 < argc) {
                wcscpy_s(ipStr, argv[++i]);
            }
        }
        else if (_wcsicmp(argv[i], L"-hack") == 0) {
            hack = true;
        }
    }
    LocalFree(argv);

    if (mode == MODE_NONE || ipStr[0] == 0) {
        MessageBoxW(nullptr,
            L"Usage:\n  share_screen.exe -s <ip>[:<port>]   (server)\n"
            L"  share_screen.exe -c <ip>[:<port>]   (client)\n"
            L"  -hack  skip NdResolveAddress",
            L"Share Screen", MB_OK | MB_ICONINFORMATION);
        return false;
    }

    // Parse ip:port
    int len = sizeof(addr);
    if (WSAStringToAddressW(ipStr, AF_INET, nullptr,
        reinterpret_cast<sockaddr*>(&addr), &len) != 0)
    {
        // Try without port
        WCHAR withPort[280];
        swprintf_s(withPort, L"%s:%d", ipStr, DEFAULT_PORT);
        len = sizeof(addr);
        if (WSAStringToAddressW(withPort, AF_INET, nullptr,
            reinterpret_cast<sockaddr*>(&addr), &len) != 0)
        {
            MessageBoxW(nullptr, L"Invalid IP address.", L"Error", MB_OK | MB_ICONERROR);
            return false;
        }
    }
    if (addr.sin_port == 0) {
        addr.sin_port = htons(DEFAULT_PORT);
    }

    return true;
}

// ---------------------------------------------------------------------------
// Window class registration
// ---------------------------------------------------------------------------
static ATOM RegisterWindowClass(HINSTANCE hInstance)
{
    WNDCLASSEXW wcex = {};
    wcex.cbSize        = sizeof(WNDCLASSEX);
    wcex.style         = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc   = WndProc;
    wcex.hInstance     = hInstance;
    wcex.hIcon         = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_SHARESCREEN));
    wcex.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wcex.lpszClassName = WINDOW_CLASS;
    wcex.hIconSm       = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_SMALL));
    return RegisterClassExW(&wcex);
}

// ---------------------------------------------------------------------------
// Create window
// ---------------------------------------------------------------------------
static HWND CreateAppWindow(HINSTANCE hInstance, int w, int h, const WCHAR* title)
{
    RECT rc = { 0, 0, w, h };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hWnd = CreateWindowW(WINDOW_CLASS, title, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, hInstance, nullptr);
    return hWnd;
}

// ---------------------------------------------------------------------------
// WndProc
// ---------------------------------------------------------------------------
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        if (g_pFramebuffer && g_frameWidth > 0 && g_frameHeight > 0)
        {
            BITMAPINFO bmi = {};
            bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth       = (LONG)g_frameWidth;
            bmi.bmiHeader.biHeight      = (LONG)g_frameHeight; // bottom-up
            bmi.bmiHeader.biPlanes      = 1;
            bmi.bmiHeader.biBitCount    = 32;
            bmi.bmiHeader.biCompression = BI_RGB;

            SetDIBitsToDevice(hdc,
                0, 0,
                g_frameWidth, g_frameHeight,
                0, 0,
                0, g_frameHeight,
                g_pFramebuffer,
                &bmi,
                DIB_RGB_COLORS);
        }
        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_TIMER:
        if (wParam == TIMER_ID) {
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        return 0;

    case WM_SIZE:
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;

    case WM_DESTROY:
        KillTimer(hWnd, TIMER_ID);
        g_bRunning = FALSE;
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
int APIENTRY wWinMain(
    _In_ HINSTANCE     hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR        lpCmdLine,
    _In_ int           nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(nCmdShow);
    g_hInst = hInstance;

    // Initialize Winsock
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        MessageBoxW(nullptr, L"WSAStartup failed.", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    // Parse command line
    AppMode mode;
    sockaddr_in addr;
    bool hack = false;
    if (!ParseCmdLine(lpCmdLine, mode, addr, hack)) {
        WSACleanup();
        return 0;
    }
    g_mode = mode;

    // Initialize NetworkDirect
    HRESULT hr = NdStartup();
    if (FAILED(hr)) {
        DbgPrint("NdStartup failed: 0x%08x\n", hr);
        MessageBoxW(nullptr, L"NdStartup failed.", L"Error", MB_OK | MB_ICONERROR);
        WSACleanup();
        return 1;
    }

    RegisterWindowClass(hInstance);

    ThreadCtx ctx = {};
    ctx.addr = addr;
    ctx.hReady = CreateEvent(nullptr, TRUE, FALSE, nullptr);

    if (mode == MODE_SERVER)
    {
        // Resolve source address for server (same as listen address)
        ctx.srcAddr = addr;

        // Start ND thread — it will signal hReady when dimensions are known
        HANDLE hThread = (HANDLE)_beginthreadex(nullptr, 0, ServerNdThread, &ctx, 0, nullptr);

        // Wait for ND setup and dimension exchange
        WaitForSingleObject(ctx.hReady, INFINITE);

        // Create display window sized to incoming frame
        g_hWnd = CreateAppWindow(hInstance,
            (int)g_frameWidth, (int)g_frameHeight,
            L"Share Screen - Server");
        if (!g_hWnd) {
            DbgPrint("[SERVER] CreateWindow failed.\n");
            g_bRunning = FALSE;
            WaitForSingleObject(hThread, INFINITE);
            CloseHandle(hThread);
            CloseHandle(ctx.hReady);
            NdCleanup();
            WSACleanup();
            return 1;
        }
        ShowWindow(g_hWnd, SW_SHOW);
        UpdateWindow(g_hWnd);
        SetTimer(g_hWnd, TIMER_ID, TIMER_MS, nullptr);

        // Message loop
        MSG msg;
        while (GetMessage(&msg, nullptr, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        g_bRunning = FALSE;
        WaitForSingleObject(hThread, 5000);
        CloseHandle(hThread);

        // Free framebuffer
        if (g_pFramebuffer) {
            VirtualFree(g_pFramebuffer, 0, MEM_RELEASE);
            g_pFramebuffer = nullptr;
        }
    }
    else // MODE_CLIENT
    {
        // Resolve local address
        if (hack) {
            ctx.srcAddr = addr;
            DbgPrint("[CLIENT] -hack mode: using same IP for src and dst.\n");
        } else {
            SIZE_T srcLen = sizeof(ctx.srcAddr);
            hr = NdResolveAddress(  
                reinterpret_cast<const sockaddr*>(&addr), sizeof(addr),
                reinterpret_cast<sockaddr*>(&ctx.srcAddr), &srcLen);
            if (FAILED(hr)) {
                DbgPrint("[CLIENT] NdResolveAddress failed: 0x%08x\n", hr);
                MessageBoxW(nullptr, L"NdResolveAddress failed.", L"Error", MB_OK | MB_ICONERROR);
                CloseHandle(ctx.hReady);
                NdCleanup();
                WSACleanup();
                return 1;
            }
        }

        // Start ND thread
        HANDLE hThread = (HANDLE)_beginthreadex(nullptr, 0, ClientNdThread, &ctx, 0, nullptr);

        // Wait for setup
        WaitForSingleObject(ctx.hReady, INFINITE);

        // Show a small status window
        g_hWnd = CreateAppWindow(hInstance, 300, 100, L"Share Screen - Sharing...");
        if (g_hWnd) {
            ShowWindow(g_hWnd, SW_SHOW);
            UpdateWindow(g_hWnd);

            MSG msg;
            while (GetMessage(&msg, nullptr, 0, 0)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
        }

        g_bRunning = FALSE;
        WaitForSingleObject(hThread, 5000);
        CloseHandle(hThread);
    }

    CloseHandle(ctx.hReady);
    NdCleanup();
    WSACleanup();
    return 0;
}
