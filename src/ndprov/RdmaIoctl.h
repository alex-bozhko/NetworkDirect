// Copyright (c) Microsoft Corporation. All rights reserved.

//
// Shared IOCTL definitions for the USB4 P2P RDMA device interface.
// Used by both the kernel-mode driver and user-mode applications.
//

#pragma once

// {F2E7CE00-5BD5-4E0C-B4A0-4A2D3FCCE7A3}
DEFINE_GUID(GUID_DEVINTERFACE_RDMA,
    0xF2E7CE00, 0x5BD5, 0x4E0C, 0xB4, 0xA0, 0x4A, 0x2D, 0x3F, 0xCC, 0xE7, 0xA3);

// {51B20BA2-E8B2-4FCF-8C13-5FB3B62665AC}, PID 2
DEFINE_DEVPROPKEY(DEVPKEY_RdmaAdapter_Luid,
    0x51b20ba2, 0xe8b2, 0x4fcf, 0x8c, 0x13, 0x5f, 0xb3, 0xb6, 0x26, 0x65, 0xac,
    2);

#define IOCTL_RDMA_CREATE_QUEUE_PAIR CTL_CODE(FILE_DEVICE_NETWORK, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_RDMA_DESTROY_QUEUE_PAIR CTL_CODE(FILE_DEVICE_NETWORK, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_RDMA_CREATE_MEMORY_REGION CTL_CODE(FILE_DEVICE_NETWORK, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_RDMA_DESTROY_MEMORY_REGION CTL_CODE(FILE_DEVICE_NETWORK, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_RDMA_SEND CTL_CODE(FILE_DEVICE_NETWORK, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_RDMA_RECEIVE CTL_CODE(FILE_DEVICE_NETWORK, 0x805, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_RDMA_CREATE_COMPLETION_QUEUE CTL_CODE(FILE_DEVICE_NETWORK, 0x806, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_RDMA_DESTROY_COMPLETION_QUEUE CTL_CODE(FILE_DEVICE_NETWORK, 0x807, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_RDMA_WRITE CTL_CODE(FILE_DEVICE_NETWORK, 0x808, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_RDMA_ARM_CQ CTL_CODE(FILE_DEVICE_NETWORK, 0x809, METHOD_BUFFERED, FILE_ANY_ACCESS)

// Sentinel value meaning "no completion queue specified".
#define RDMA_INVALID_ID 0xFFFFFFFF

typedef struct _RDMA_CREATE_QUEUE_PAIR_INPUT
{
    UINT32 TxCompletionQueueId;
    UINT32 RxCompletionQueueId;
} RDMA_CREATE_QUEUE_PAIR_INPUT;

typedef struct _RDMA_CREATE_QUEUE_PAIR_OUTPUT
{
    UINT32 QueuePairIndex;
} RDMA_CREATE_QUEUE_PAIR_OUTPUT;

typedef struct _RDMA_DESTROY_QUEUE_PAIR_INPUT
{
    UINT32 QueuePairIndex;
} RDMA_DESTROY_QUEUE_PAIR_INPUT;

typedef struct _RDMA_CREATE_MEMORY_REGION_INPUT
{
    UINT64 UserModeBuffer;
    UINT64 Size;
} RDMA_CREATE_MEMORY_REGION_INPUT;

typedef struct _RDMA_CREATE_MEMORY_REGION_OUTPUT
{
    UINT32 MemoryRegionKey;
} RDMA_CREATE_MEMORY_REGION_OUTPUT;

typedef struct _RDMA_DESTROY_MEMORY_REGION_INPUT
{
    UINT32 MemoryRegionKey;
} RDMA_DESTROY_MEMORY_REGION_INPUT;

typedef struct _RDMA_SEND_INPUT
{
    UINT32 QueuePairIndex;
    UINT32 MemoryRegionKey;
    UINT64 QueuePairContext;
    UINT64 RequestContext;
    UINT64 Address;
    UINT32 Length;
} RDMA_SEND_INPUT;

typedef struct _RDMA_RECEIVE_INPUT
{
    UINT32 QueuePairIndex;
    UINT32 MemoryRegionKey;
    UINT64 QueuePairContext;
    UINT64 RequestContext;
    UINT64 Address;
    UINT32 Length;
} RDMA_RECEIVE_INPUT;

typedef struct _RDMA_WRITE_INPUT
{
    UINT32 QueuePairIndex;
    UINT32 MemoryRegionKey;
    UINT64 QueuePairContext;
    UINT64 RequestContext;
    UINT64 Address;
    UINT32 Length;
    UINT32 RemoteMemoryRegionKey;
    UINT64 RemoteOffset;
} RDMA_WRITE_INPUT;

typedef struct _RDMA_CREATE_COMPLETION_QUEUE_INPUT
{
    UINT32 Capacity;
} RDMA_CREATE_COMPLETION_QUEUE_INPUT;

typedef struct _RDMA_CREATE_COMPLETION_QUEUE_OUTPUT
{
    UINT32 CompletionQueueId;
    UINT64 SharedBufferAddress;
    UINT32 SharedBufferCapacity;
} RDMA_CREATE_COMPLETION_QUEUE_OUTPUT;

typedef struct _RDMA_DESTROY_COMPLETION_QUEUE_INPUT
{
    UINT32 CompletionQueueId;
} RDMA_DESTROY_COMPLETION_QUEUE_INPUT;

typedef struct _RDMA_ARM_CQ_INPUT
{
    UINT32 CompletionQueueId;
} RDMA_ARM_CQ_INPUT;

typedef enum _RDMA_COMPLETION_EVENT_TYPE
{
    // Send-side completions
    RdmaCompletionEventTypeSend = 1,
    RdmaCompletionEventTypeSendWithInvalidate,
    RdmaCompletionEventTypeRdmaWrite,
    RdmaCompletionEventTypeRdmaWriteWithImmediate,
    RdmaCompletionEventTypeRdmaRead,
    RdmaCompletionEventTypeLocalInvalidate,
    RdmaCompletionEventTypeBindMemoryWindow,
    RdmaCompletionEventTypeFastRegistration,
    RdmaCompletionEventTypeAtomicCompareAndSwap,
    RdmaCompletionEventTypeAtomicFetchAndAdd,

    // Receive-side completions
    RdmaCompletionEventTypeReceive,
    RdmaCompletionEventTypeReceiveWithImmediate,
    RdmaCompletionEventTypeReceiveWithInvalidate,

    // Error completions
    RdmaCompletionEventTypeLocalLengthError,
    RdmaCompletionEventTypeLocalProtectionError,
    RdmaCompletionEventTypeRemoteAccessError,
    RdmaCompletionEventTypeRemoteOperationError,
    RdmaCompletionEventTypeRetryExceeded,
    RdmaCompletionEventTypeRnrRetryExceeded,
    RdmaCompletionEventTypeTransportError,
    RdmaCompletionEventTypeFlushError,
    RdmaCompletionEventTypeMemoryWindowBindError,
    RdmaCompletionEventTypeBadResponseError,
} RDMA_COMPLETION_EVENT_TYPE;

//
// Shared memory layout for a completion queue ring buffer mapped between
// kernel and user mode. The kernel writes entries and advances PostIndex.
// User mode reads entries and advances DrainIndex.
//
typedef struct _RDMA_COMPLETION_ENTRY
{
    RDMA_COMPLETION_EVENT_TYPE Event;
    UINT32 BytesTransferred;
    UINT64 QueuePairContext;
    UINT64 RequestContext;
} RDMA_COMPLETION_ENTRY;

// Cache line size used to separate producer and consumer indices to
// prevent false sharing between kernel (PostIndex) and user mode (DrainIndex).
#define RDMA_CACHE_LINE_SIZE 64

typedef struct _RDMA_SHARED_COMPLETION_QUEUE
{
    // Kernel-written index: aligned to its own cache line.
    DECLSPEC_ALIGN(RDMA_CACHE_LINE_SIZE) volatile ULONG PostIndex;

    // User-mode-written index: aligned to its own cache line.
    DECLSPEC_ALIGN(RDMA_CACHE_LINE_SIZE) volatile ULONG DrainIndex;

    // Read-only metadata set once during initialization.
    UINT32 Capacity;
    UINT32 CapacityMask;
    RDMA_COMPLETION_ENTRY Entries[1];
} RDMA_SHARED_COMPLETION_QUEUE;
