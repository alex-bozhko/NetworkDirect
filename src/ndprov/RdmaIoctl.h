// Copyright (c) Microsoft Corporation. All rights reserved.

//
// Shared IOCTL definitions for the USB4 P2P RDMA device interface.
// Used by both the kernel-mode driver and user-mode applications.
//

#pragma once

// User-mode includers may not define NTSTATUS; bcrypt.h defines it the same way.
#ifndef _NTDEF_
typedef _Return_type_success_(
    return >= 0) LONG NTSTATUS;
#endif

// {F2E7CE00-5BD5-4E0C-B4A0-4A2D3FCCE7A3}
DEFINE_GUID(GUID_DEVINTERFACE_RDMA,
    0xF2E7CE00, 0x5BD5, 0x4E0C, 0xB4, 0xA0, 0x4A, 0x2D, 0x3F, 0xCC, 0xE7, 0xA3);

// {51B20BA2-E8B2-4FCF-8C13-5FB3B62665AC}, PID 2
DEFINE_DEVPROPKEY(DEVPKEY_RdmaAdapter_Luid,
    0x51b20ba2, 0xe8b2, 0x4fcf, 0x8c, 0x13, 0x5f, 0xb3, 0xb6, 0x26, 0x65, 0xac,
    2);

//
// Files opened on the RDMA interface:
//
// - <interface path>: an RDMA file. It owns completion queues and memory
//   regions, which other files reference.
// - <interface path>\overlapped: an overlapped file. Once bound to an RDMA
//   file, it arms that file's completion queues, so ND overlapped I/O can be
//   delivered through its own I/O completion port. The provider also completes
//   the connector and listener requests it carries out through it.
// - <interface path>\qp: a queue pair file. It owns one queue pair, created
//   from an RDMA file's resources once the drivers of both machines bind it to
//   a tunnel; closing the file destroys the queue pair and releases the tunnel.
//
// A bound overlapped file and a queue pair file keep their RDMA file open
// until they close.
//
#define RDMA_OVERLAPPED_FILE_SUFFIX L"\\overlapped"
#define RDMA_QUEUE_PAIR_FILE_SUFFIX L"\\qp"

#define RDMA_CTL_CODE(                                                                                                           \
    function,                                                                                                                    \
    method,                                                                                                                      \
    access)                                                                                                                      \
    CTL_CODE(                                                                                                                    \
        FILE_DEVICE_NETWORK,                                                                                                     \
        0x800 + (function),                                                                                                      \
        method,                                                                                                                  \
        access)

#define RDMA_READ_ACCESS       FILE_READ_ACCESS
#define RDMA_READ_WRITE_ACCESS (FILE_READ_ACCESS | FILE_WRITE_ACCESS)

//
// Sent on an RDMA file.
//

// Output: RDMA_QUERY_TUNNELS_OUTPUT.
#define IOCTL_RDMA_QUERY_TUNNELS                                                                                                 \
    RDMA_CTL_CODE(                                                                                                               \
        0x00,                                                                                                                    \
        METHOD_BUFFERED,                                                                                                         \
        RDMA_READ_ACCESS)
// Locks a provider-allocated completion queue ring. Must be sent from the
// process that opened the file. Input: RDMA_CREATE_COMPLETION_QUEUE_INPUT.
// Output: RDMA_CREATE_COMPLETION_QUEUE_OUTPUT.
#define IOCTL_RDMA_CREATE_COMPLETION_QUEUE                                                                                       \
    RDMA_CTL_CODE(                                                                                                               \
        0x03,                                                                                                                    \
        METHOD_BUFFERED,                                                                                                         \
        RDMA_READ_WRITE_ACCESS)
// Input: RDMA_DESTROY_COMPLETION_QUEUE_INPUT.
#define IOCTL_RDMA_DESTROY_COMPLETION_QUEUE                                                                                      \
    RDMA_CTL_CODE(                                                                                                               \
        0x04,                                                                                                                    \
        METHOD_BUFFERED,                                                                                                         \
        RDMA_READ_WRITE_ACCESS)
// Completes, through a completion queue, requests that the provider holds and
// never submitted to a queue pair. Input: RDMA_CANCEL_UNPOSTED_REQUESTS_INPUT.
#define IOCTL_RDMA_CANCEL_UNPOSTED_REQUESTS                                                                                      \
    RDMA_CTL_CODE(                                                                                                               \
        0x05,                                                                                                                    \
        METHOD_BUFFERED,                                                                                                         \
        RDMA_READ_WRITE_ACCESS)
// Locks a memory region. Must be sent from the process that opened the file.
// Input: RDMA_CREATE_MEMORY_REGION_INPUT. Output:
// RDMA_CREATE_MEMORY_REGION_OUTPUT.
#define IOCTL_RDMA_CREATE_MEMORY_REGION                                                                                          \
    RDMA_CTL_CODE(                                                                                                               \
        0x06,                                                                                                                    \
        METHOD_BUFFERED,                                                                                                         \
        RDMA_READ_WRITE_ACCESS)
// Input: RDMA_DESTROY_MEMORY_REGION_INPUT.
#define IOCTL_RDMA_DESTROY_MEMORY_REGION                                                                                         \
    RDMA_CTL_CODE(                                                                                                               \
        0x07,                                                                                                                    \
        METHOD_BUFFERED,                                                                                                         \
        RDMA_READ_WRITE_ACCESS)

//
// Sent on an RDMA file or a bound overlapped file.
//

// Pends until the completion queue has a completion of the requested type that
// the consumer has not claimed. Input: RDMA_ARM_COMPLETION_QUEUE_INPUT.
#define IOCTL_RDMA_ARM_COMPLETION_QUEUE                                                                                          \
    RDMA_CTL_CODE(                                                                                                               \
        0x08,                                                                                                                    \
        METHOD_BUFFERED,                                                                                                         \
        RDMA_READ_WRITE_ACCESS)

//
// Sent on an overlapped file.
//

// Binds the overlapped file to the RDMA file in the input, once.
// Input: RDMA_BIND_OVERLAPPED_FILE_INPUT.
#define IOCTL_RDMA_BIND_OVERLAPPED_FILE                                                                                          \
    RDMA_CTL_CODE(                                                                                                               \
        0x09,                                                                                                                    \
        METHOD_BUFFERED,                                                                                                         \
        RDMA_READ_WRITE_ACCESS)
// Completes, with the input's status, the application request whose OVERLAPPED
// it is sent with. The provider sends it once it has carried out a request
// itself, such as a connector or listener request, so the I/O manager reports
// that request like any other pended I/O on the file: it stores the status in
// the OVERLAPPED, signals its event and queues a completion packet.
// Input: RDMA_COMPLETE_OVERLAPPED_INPUT.
#define IOCTL_RDMA_COMPLETE_OVERLAPPED                                                                                           \
    RDMA_CTL_CODE(                                                                                                               \
        0x0E,                                                                                                                    \
        METHOD_BUFFERED,                                                                                                         \
        RDMA_READ_WRITE_ACCESS)

//
// Sent on a queue pair file.
//

// Creates the file's queue pair, once, on a tunnel that the drivers agree on.
// Both sides of a connection send it with the same connection token: the
// active side before it sends the connection request, and the passive side
// once it accepts it. The drivers then agree on a tunnel and start its rings.
// The request pends until both sides' rings run, or the creation fails; the
// passive side's fails after 5 seconds. Canceling the request doesn't undo
// the creation; closing the file does. Input: RDMA_CREATE_QUEUE_PAIR_INPUT.
// Output: RDMA_CREATE_QUEUE_PAIR_OUTPUT.
#define IOCTL_RDMA_CREATE_QUEUE_PAIR                                                                                             \
    RDMA_CTL_CODE(                                                                                                               \
        0x0A,                                                                                                                    \
        METHOD_BUFFERED,                                                                                                         \
        RDMA_READ_WRITE_ACCESS)
// Stops the queue pair for good: its rings stop, and its outstanding requests
// and those posted afterward complete with flush errors. The driver releases
// the tunnel, which retires the peer's queue pair as well. Optional input:
// RDMA_FLUSH_QUEUE_PAIR_INPUT.
#define IOCTL_RDMA_FLUSH_QUEUE_PAIR                                                                                              \
    RDMA_CTL_CODE(                                                                                                               \
        0x0B,                                                                                                                    \
        METHOD_BUFFERED,                                                                                                         \
        RDMA_READ_WRITE_ACCESS)
// Handled by fast I/O; completes through the send completion queue.
// Input: RDMA_SEND_INPUT.
#define IOCTL_RDMA_SEND                                                                                                          \
    RDMA_CTL_CODE(                                                                                                               \
        0x0C,                                                                                                                    \
        METHOD_NEITHER,                                                                                                          \
        RDMA_READ_WRITE_ACCESS)
// Handled by fast I/O; completes through the receive completion queue.
// Input: RDMA_RECEIVE_INPUT.
#define IOCTL_RDMA_RECEIVE                                                                                                       \
    RDMA_CTL_CODE(                                                                                                               \
        0x0D,                                                                                                                    \
        METHOD_NEITHER,                                                                                                          \
        RDMA_READ_WRITE_ACCESS)
// Pends until both sides released the tunnel of the bound queue pair: the
// queue pair of either side was flushed, failed or closed, or the data path
// stopped. Closing the file cancels it. Output:
// RDMA_QUEUE_PAIR_RELEASE_OUTPUT.
#define IOCTL_RDMA_NOTIFY_QUEUE_PAIR_RELEASE                                                                                     \
    RDMA_CTL_CODE(                                                                                                               \
        0x0F,                                                                                                                    \
        METHOD_BUFFERED,                                                                                                         \
        RDMA_READ_WRITE_ACCESS)

// No completion queue, memory region or tunnel.
#define RDMA_INVALID_ID 0xFFFFFFFF
#define RDMA_MAX_COMPLETION_QUEUE_CAPACITY  4096
#define RDMA_MAX_COMPLETION_QUEUES_PER_FILE 64

// Sends a queue pair holds until they complete, and Receives it holds once
// connected.
#define RDMA_MAX_QUEUE_PAIR_REQUESTS 4096

// Largest memory region: the MDL mapping limit, 4 GiB less one page.
#define RDMA_MAX_MEMORY_REGION_SIZE 0xFFFFF000ull

// Memory regions must start on a page boundary, and Send and Receive buffers at
// a page-aligned offset within their memory region.
#define RDMA_PAGE_SIZE 0x1000

// Identifies a connection to the drivers of both sides. Never zero.
typedef struct _RDMA_CONNECTION_TOKEN
{
    UINT64 High;
    UINT64 Low;
} RDMA_CONNECTION_TOKEN;

typedef struct _RDMA_QUERY_TUNNELS_OUTPUT
{
    // Negotiated tunnels that both sides released.
    UINT32 AvailableTunnelMask;
    UINT32 NegotiatedTunnelMask;
    UINT32 OutOfCommissionTunnelMask;
} RDMA_QUERY_TUNNELS_OUTPUT;

typedef struct _RDMA_CREATE_COMPLETION_QUEUE_INPUT
{
    // Page-aligned ring of RDMA_CQ_RING_SIZE(Capacity) bytes, initialized as
    // described at RDMA_CQ_RING_HEADER.
    UINT64 RingAddress;
    UINT32 RingSize;
    // Power of two, 1 to RDMA_MAX_COMPLETION_QUEUE_CAPACITY.
    UINT32 Capacity;
} RDMA_CREATE_COMPLETION_QUEUE_INPUT;

typedef struct _RDMA_CREATE_COMPLETION_QUEUE_OUTPUT
{
    // File-local completion queue ID.
    UINT32 CompletionQueueId;
} RDMA_CREATE_COMPLETION_QUEUE_OUTPUT;

typedef struct _RDMA_DESTROY_COMPLETION_QUEUE_INPUT
{
    UINT32 CompletionQueueId;
} RDMA_DESTROY_COMPLETION_QUEUE_INPUT;

typedef enum _RDMA_COMPLETION_EVENT_TYPE
{
    RdmaCompletionEventTypeSend = 1,
    RdmaCompletionEventTypeReceive,

    // Errors. Flush errors complete canceled requests.
    RdmaCompletionEventTypeSendFlushError,
    RdmaCompletionEventTypeReceiveFlushError,
    // A posted Receive was too small for the incoming Send.
    RdmaCompletionEventTypeReceiveBufferOverflow,
    RdmaCompletionEventTypeLocalLengthError,
    RdmaCompletionEventTypeRemoteOperationError,
    RdmaCompletionEventTypeTransportError,
} RDMA_COMPLETION_EVENT_TYPE;

typedef struct _RDMA_CANCEL_UNPOSTED_REQUESTS_INPUT
{
    UINT64 QueuePairContext;
    UINT32 CompletionQueueId;
    // RdmaCompletionEventTypeSendFlushError or
    // RdmaCompletionEventTypeReceiveFlushError.
    UINT32 Event;
    UINT32 RequestCount;
    UINT64 RequestContexts[ANYSIZE_ARRAY];
} RDMA_CANCEL_UNPOSTED_REQUESTS_INPUT;

typedef struct _RDMA_CREATE_MEMORY_REGION_INPUT
{
    // Page-aligned.
    UINT64 Address;
    // Page multiple, at most RDMA_MAX_MEMORY_REGION_SIZE.
    UINT64 Size;
} RDMA_CREATE_MEMORY_REGION_INPUT;

typedef struct _RDMA_CREATE_MEMORY_REGION_OUTPUT
{
    // File-local memory region key.
    UINT32 MemoryRegionKey;
} RDMA_CREATE_MEMORY_REGION_OUTPUT;

typedef struct _RDMA_DESTROY_MEMORY_REGION_INPUT
{
    UINT32 MemoryRegionKey;
} RDMA_DESTROY_MEMORY_REGION_INPUT;

typedef enum _RDMA_CQ_NOTIFY_TYPE
{
    RdmaCqNotifyErrors = 0,
    RdmaCqNotifyAny = 1,
    RdmaCqNotifySolicited = 2,
} RDMA_CQ_NOTIFY_TYPE;

typedef struct _RDMA_ARM_COMPLETION_QUEUE_INPUT
{
    UINT32 CompletionQueueId;
    // RDMA_CQ_NOTIFY_TYPE
    UINT32 Type;
} RDMA_ARM_COMPLETION_QUEUE_INPUT;

typedef struct _RDMA_BIND_OVERLAPPED_FILE_INPUT
{
    // Handle, in the calling process, of the RDMA file.
    UINT64 RdmaHandle;
} RDMA_BIND_OVERLAPPED_FILE_INPUT;

typedef struct _RDMA_COMPLETE_OVERLAPPED_INPUT
{
    // STATUS_PENDING and STATUS_MORE_PROCESSING_REQUIRED are rejected.
    NTSTATUS Status;
} RDMA_COMPLETE_OVERLAPPED_INPUT;

typedef enum _RDMA_CONNECTION_ROLE
{
    // Connects: creates its queue pair before sending the connection request,
    // then waits for the passive side.
    RdmaConnectionRoleActive = 0,
    // Accepts: its creation starts the drivers' agreement on the tunnel.
    RdmaConnectionRolePassive = 1,
} RDMA_CONNECTION_ROLE;

typedef struct _RDMA_CREATE_QUEUE_PAIR_INPUT
{
    // Handle, in the calling process, of the RDMA file that owns the
    // completion queues.
    UINT64 RdmaHandle;
    // Reported in every completion of the queue pair.
    UINT64 QueuePairContext;
    RDMA_CONNECTION_TOKEN ConnectionToken;
    UINT32 SendCompletionQueueId;
    UINT32 ReceiveCompletionQueueId;
    // RDMA_CONNECTION_ROLE
    UINT32 Role;
} RDMA_CREATE_QUEUE_PAIR_INPUT;

typedef struct _RDMA_CREATE_QUEUE_PAIR_OUTPUT
{
    // System-wide queue pair ID, for diagnostics. It also names the queue
    // pair's performance counter instance.
    UINT64 QueuePairId;
    UINT32 TunnelIndex;
} RDMA_CREATE_QUEUE_PAIR_OUTPUT;

// The release is a disconnection rather than an abort.
#define RDMA_FLUSH_FLAG_DISCONNECT 0x00000001

typedef struct _RDMA_FLUSH_QUEUE_PAIR_INPUT
{
    // RDMA_FLUSH_FLAG_*
    UINT32 Flags;
} RDMA_FLUSH_QUEUE_PAIR_INPUT;

typedef enum _RDMA_RELEASE_REASON
{
    // A flush with RDMA_FLUSH_FLAG_DISCONNECT.
    RdmaReleaseReasonDisconnect = 1,
    // Any other flush, a failure, or closing the queue pair file.
    RdmaReleaseReasonAbort = 2,
    // The data path stopped, such as when the link went down.
    RdmaReleaseReasonDataPath = 3,
} RDMA_RELEASE_REASON;

// The peer released the tunnel first.
#define RDMA_RELEASE_FLAG_PEER 0x00000001
// The peer didn't answer this side's release within 5 seconds, so the driver
// completed the release without it and took the tunnel out of commission.
#define RDMA_RELEASE_FLAG_UNANSWERED 0x00000002

typedef struct _RDMA_QUEUE_PAIR_RELEASE_OUTPUT
{
    // RDMA_RELEASE_REASON
    UINT32 Reason;
    // RDMA_RELEASE_FLAG_*
    UINT32 Flags;
} RDMA_QUEUE_PAIR_RELEASE_OUTPUT;

#define RDMA_SEND_FLAG_SOLICITED 0x00000001
// A Send that succeeds posts no completion; one that fails still does.
#define RDMA_SEND_FLAG_SILENT_SUCCESS 0x00000002

typedef struct _RDMA_SEND_INPUT
{
    UINT64 RequestContext;
    UINT64 Address;
    // RDMA_INVALID_ID for an empty Send without a buffer; Address and Length
    // must then be 0.
    UINT32 MemoryRegionKey;
    UINT32 Length;
    // RDMA_SEND_FLAG_*
    UINT32 Flags;
} RDMA_SEND_INPUT;

typedef struct _RDMA_RECEIVE_INPUT
{
    UINT64 RequestContext;
    UINT64 Address;
    UINT32 MemoryRegionKey;
    UINT32 Length;
} RDMA_RECEIVE_INPUT;

typedef enum _RDMA_COMPLETION_QUEUE_STATE
{
    RdmaCompletionQueueStateActive,
    // A completion found its ring slot still unconsumed. The queue and its
    // queue pairs are unusable; completions already in the ring stay valid.
    RdmaCompletionQueueStateOverflow,
    // The owning file was cleaned up; the kernel no longer writes the ring.
    RdmaCompletionQueueStateClosed,
} RDMA_COMPLETION_QUEUE_STATE;

#define RDMA_CACHE_LINE_SIZE 64

// RDMA_CQ_ENTRY.Flags
#define RDMA_CQ_ENTRY_FLAG_SOLICITED 0x0001
#define RDMA_CQ_ENTRY_FLAG_ERROR     0x0002

//
// Completion queue ring, allocated by the provider and locked by the driver.
//
// The ring is a bounded multi-consumer queue with one sequence number per
// slot. Positions are free-running 32-bit counters; slot = position & (Capacity - 1).
//
// - Initially Head is 0 and Entries[i].Sequence is i.
// - The driver writes the completion for position p into its slot only if the
//   slot's Sequence is p, then stores Sequence = p + 1 with release semantics.
//   A slot with any other Sequence overflows the queue.
// - A consumer at Head h reads the slot's Sequence with acquire semantics. If
//   it is h + 1 the slot holds a completion; consumers claim positions by
//   advancing Head with compare-exchange, copy the claimed entries, then store
//   Sequence = position + Capacity with release semantics to return each slot.
//
// The driver never trusts ring contents for anything but its own full check
// and notification decisions; a corrupt ring only affects its own process.
//
typedef struct _RDMA_CQ_ENTRY
{
    UINT64 QueuePairContext;
    UINT64 RequestContext;
    UINT32 BytesTransferred;
    // RDMA_COMPLETION_EVENT_TYPE
    UINT16 Event;
    UINT16 Flags;
    volatile ULONG Sequence;
    UINT32 Reserved;
} RDMA_CQ_ENTRY;

typedef struct _RDMA_CQ_RING_HEADER
{
    // Consumer-owned: next position to claim.
    DECLSPEC_ALIGN(
        RDMA_CACHE_LINE_SIZE)
    volatile ULONG Head;

    // Driver-owned. Informational copies for user-mode notification
    // decisions; the driver keeps its own state.
    DECLSPEC_ALIGN(
        RDMA_CACHE_LINE_SIZE)
    volatile LONG State;
    // Position after the last solicited or error completion.
    volatile ULONG SolicitedTail;
    // Position after the last error completion.
    volatile ULONG ErrorTail;
} RDMA_CQ_RING_HEADER;

#define RDMA_CQ_RING_ENTRIES_OFFSET sizeof(RDMA_CQ_RING_HEADER)
#define RDMA_CQ_RING_SIZE(capacity)                                                                                              \
    ((RDMA_CQ_RING_ENTRIES_OFFSET + (static_cast<SIZE_T>(capacity) * sizeof(RDMA_CQ_ENTRY)) + RDMA_PAGE_SIZE - 1) &              \
     ~static_cast<SIZE_T>(RDMA_PAGE_SIZE - 1))

static_assert(
    sizeof(RDMA_CQ_ENTRY) == 32,
    "Two completion queue entries per cache line");
static_assert(
    sizeof(RDMA_CQ_RING_HEADER) == 2 * RDMA_CACHE_LINE_SIZE,
    "Completion queue entries start on a cache line");
