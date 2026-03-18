# NetworkDirect Test Suite — Detailed Reference

This document provides a comprehensive overview of all example and unit test projects in the NetworkDirect solution, including usage instructions, protocol details, and measurement methodology.

> **Important:** All tests require real RDMA-capable hardware (no mocks/stubs). All tests accept IPv4 addresses only.

---

## Table of Contents

- [Examples](#examples)
  - [ndadapterinfo — Adapter Capabilities Query](#ndadapterinfo--adapter-capabilities-query)
  - [ndcat — Address Catalog Validation](#ndcat--address-catalog-validation)
  - [ndmrlat — Memory Registration Latency](#ndmrlat--memory-registration-latency)
  - [ndmrrate — Memory Registration Throughput](#ndmrrate--memory-registration-throughput)
  - [ndping — Send/Recv Latency (Unidirectional)](#ndping--sendrecv-latency-unidirectional)
  - [ndpingpong — Send/Recv Latency (Bidirectional)](#ndpingpong--sendrecv-latency-bidirectional)
  - [ndrping — RDMA Read/Write Latency (Unidirectional)](#ndrping--rdma-readwrite-latency-unidirectional)
  - [ndrpingpong — RDMA Write Latency (Bidirectional)](#ndrpingpong--rdma-write-latency-bidirectional)
- [Unit Tests](#unit-tests)
  - [ndcq — Completion Queue Validation](#ndcq--completion-queue-validation)
  - [ndconn — Connection Scalability](#ndconn--connection-scalability)
  - [ndmval — Memory Buffer Overflow Validation](#ndmval--memory-buffer-overflow-validation)
  - [ndmw — Memory Window RDMA Validation](#ndmw--memory-window-rdma-validation)
  - [ndmpic — MS-MPI Connection Emulation](#ndmpic--ms-mpi-connection-emulation)
  - [ndmemorytest — Comprehensive Stress/Error Test Suite](#ndmemorytest--comprehensive-stresserror-test-suite)
- [Shared Libraries](#shared-libraries)
  - [ndutil — Core NetworkDirect Framework](#ndutil--core-networkdirect-framework)
  - [ndtestutil — Test Utility Library](#ndtestutil--test-utility-library)

---

## Examples

### ndadapterinfo — Adapter Capabilities Query

**Purpose:** Diagnostic tool that queries and displays detailed adapter capabilities for a given NetworkDirect-capable IP address.

**Usage:**
```
ndadapterinfo [options] <IPv4 Address>
Options:
  -l,--logFile <logFile>   Log output to a given file
  -h,--help                Show this message
```

**What it does:**
1. Initializes Winsock and NetworkDirect (`NdStartup`)
2. Validates the input address is ND-capable via `NdCheckAddress()`
3. Opens the adapter via `NdOpenAdapter()` with `IID_IND2Adapter`
4. Queries `ND2_ADAPTER_INFO` and prints all fields

**Reported information:**
- VendorId, DeviceId, AdapterId
- MaxRegistrationSize, MaxWindowSize
- MaxInitiatorSge, MaxReceiveSge, MaxReadSge
- MaxTransferLength, MaxInlineDataSize
- MaxInboundReadLimit, MaxOutboundReadLimit
- MaxReceiveQueueDepth, MaxInitiatorQueueDepth, MaxSharedReceiveQueueDepth
- MaxCompletionQueueDepth
- InlineRequestThreshold, LargeRequestThreshold
- MaxCallerData, MaxCalleeData
- AdapterFlags (IN_ORDER_DMA, CQ_INTERRUPT_MODERATION, MULTI_ENGINE, CQ_RESIZE, LOOPBACK)

**Requires:** Single node, no server/client pair. No network traffic.

**Example:**
```
ndadapterinfo 192.168.1.10
```

---

### ndcat — Address Catalog Validation

**Purpose:** Validates that all addresses returned by `NdQueryAddressList()` are valid NetworkDirect addresses.

**Usage:**
```
ndcat [options] <IPv4 Address>
Options:
  -l,--logFile <logFile>   Log output to a given file
  -h,--help                Show this message
```

**What it does:**
1. Calls `NdQueryAddressList()` to enumerate all ND addresses in the catalog
2. Iterates through each address and calls `NdCheckAddress()` to verify validity
3. Prints "Validation of full Addresslist passed" on success

**Requires:** Single node. No network traffic.

**Example:**
```
ndcat 192.168.1.10
```

---

### ndmrlat — Memory Registration Latency

**Purpose:** Measures the latency of memory registration and deregistration operations across varying buffer sizes.

**Usage:**
```
ndmrlat [options] <IPv4 Address>
Options:
  -l,--logFile <logFile>   Log output to a given file
  -h,--help                Show this message
```

**Defaults:**
| Parameter | Value |
|-----------|-------|
| Max buffer size | 4 MB |
| Iterations per size | 10,000 |

**What it measures:**
For each buffer size (1 byte → 4 MB, doubling each step):
- **Register call time:** Time from `Register()` call to `GetOverlappedResult()` start
- **Register total time:** Including `GetOverlappedResult()` completion wait
- **Deregister call time / total time:** Same breakdown for deregistration
- **CPU utilization** during each phase

Tests both **event-driven** and **IOCP** completion modes.

**Output format:**
```
         Register (usec)    Deregister (usec)       CPU
     Size        call       total      call       total
        1      <time>     <time>     <time>     <time>  <cpu%>
        2      <time>     <time>     <time>     <time>  <cpu%>
      ...
  4194304      <time>     <time>     <time>     <time>  <cpu%>
```

**Requires:** Single node. No network traffic — purely local memory operations.

**Example:**
```
ndmrlat 192.168.1.10
```

---

### ndmrrate — Memory Registration Throughput

**Purpose:** Measures memory registration/deregistration throughput (operations per second) with multi-threaded parallelism.

**Usage:**
```
ndmrrate [options] <IPv4 Address>
Options:
  -t,--threads <numThreads>   Number of threads (default: 2)
  -l,--logFile <logFile>      Log output to a given file
  -h,--help                   Show this message
```

**Defaults:**
| Parameter | Value |
|-----------|-------|
| Threads | 2 |
| Max buffer size | 4 MB |
| Max registrations | 10,000 |
| Min registrations (largest buffer) | 2,000 |

**What it measures:**
For each buffer size (1 byte → 4 MB):
- Register and deregister latency in microseconds across all threads
- CPU utilization
- Thread scalability (run with different `-t` values to compare)

Uses IOCP-based completions and barrier synchronization across threads.

**Output format:**
```
                        Register            Deregister
     Size     Iter      usec        CPU     usec         CPU
       1     10000       <x>       <x>      <x>        <x>
     ...
 4194304      2000       <x>       <x>      <x>        <x>
```

**Requires:** Single node. No network traffic.

**Examples:**
```
ndmrrate 192.168.1.10
ndmrrate -t 8 192.168.1.10
```

---

### ndping — Send/Recv Latency (Unidirectional)

**Purpose:** Measures unidirectional send/receive latency and throughput. Client sends messages to server; server acknowledges with credit updates for flow control.

**Usage:**
```
ndping [options] <ip>[:<port>]
Options:
  -s            - Start as server (listen on IP/Port)
  -c            - Start as client (connect to server IP/Port)
  -b            - Blocking I/O (wait for CQ notification)
  -p            - Polling I/O (poll on the CQ) (default)
  -n <nSge>     - Number of scatter/gather entries per transfer (default: 1)
  -q <pipeline> - Pipeline limit (default: 128)
  -l <logFile>  - Log output to a file
```

**Defaults:**
| Parameter | Value |
|-----------|-------|
| Port | 54324 |
| Max transfer size | 4 MB |
| Max iterations | 500,000 |
| Pipeline depth | 128 |
| Completion mode | Polling |

**Protocol:**
1. **Server** listens and accepts; advertises queue depth via connection private data
2. **Client** connects; retrieves queue depth; begins sending
3. **Flow control:** Credit-based — server sends 1-byte credit update every `queue_depth/2` receives
4. **Termination:** Client sends 0-byte SYNC message; server ACKs; both shut down

**What it measures:**
For each message size (1 byte → 4 MB):
- Latency (µs per message)
- CPU utilization
- Throughput (Bytes/sec)

**Output format:**
```
      Size       Iter    Latency       CPU    Bytes/Sec
         1       <n>      <x>us       <x>%   <x> MB/s
       ...
 4194304       <n>      <x>us       <x>%   <x> MB/s
```

**Requires:** Server + Client on two endpoints.

**Examples:**
```
# Server
ndping -s -p 192.168.1.10:54324

# Client
ndping -c -p 192.168.1.10:54324

# With blocking I/O and 4 SGEs
ndping -s -b -n 4 192.168.1.10:54324
ndping -c -b -n 4 192.168.1.10:54324
```

---

### ndpingpong — Send/Recv Latency (Bidirectional)

**Purpose:** Measures round-trip send/receive latency using a strict ping-pong pattern — client sends, server echoes back, measuring full round-trip time.

**Usage:**
```
ndpingpong [options] <ip>[:<port>]
Options:
  -s            - Start as server
  -c            - Start as client
  -b            - Blocking I/O
  -p            - Polling I/O (default)
  -n <nSge>     - SGE count (default: 1)
  -q <pipeline> - Pipeline limit
  -l <logFile>  - Log output
```

**Defaults:**
| Parameter | Value |
|-----------|-------|
| Port | 54325 |
| Max transfer size | 4 MB |
| Max iterations | 100,000 |
| Completion mode | Polling |

**Protocol:**
1. **Client** sends message → waits for send completion → waits for receive (pong)
2. **Server** receives message → sends echo → waits for send completion
3. Strictly sequential — one message in flight at a time (no pipelining)
4. Reported latency = round-trip time / 2

**What it measures:**
- One-way latency (half of measured round-trip)
- CPU utilization
- Throughput (2× for bidirectional)

**Requires:** Server + Client.

**Examples:**
```
ndpingpong -s -p 192.168.1.10:54325
ndpingpong -c -p 192.168.1.10:54325
```

---

### ndrping — RDMA Read/Write Latency (Unidirectional)

**Purpose:** Measures one-sided RDMA Read or Write latency. The server exposes a memory region; the client performs RDMA operations directly without server involvement in the data path.

**Usage:**
```
ndrping [options] <ip>[:<port>]
Options:
  -s            - Start as server
  -c            - Start as client
  -b            - Blocking I/O
  -p            - Polling I/O (default)
  -w            - Use RDMA Write (default)
  -r            - Use RDMA Read
  -n <nSge>     - SGE count (default: 1)
  -q <pipeline> - Pipeline limit (default: 128)
  -l <logFile>  - Log output
```

**Defaults:**
| Parameter | Value |
|-----------|-------|
| Port | 54326 |
| Max transfer size | 4 MB |
| Max iterations | 500,000 |
| RDMA operation | Write |
| Pipeline depth | 128 |
| Completion mode | Polling |

**Protocol:**
1. **Server** registers buffer with appropriate RDMA permissions, creates memory window, binds buffer, sends PeerInfo (remote token + address) to client, then waits passively
2. **Client** receives PeerInfo, then issues RDMA Read or Write operations in a pipelined loop using credit-based flow control
3. **Termination:** Client sends 0-byte message; server receives and shuts down

**Adapter requirement:** Must support `ND_ADAPTER_FLAG_IN_ORDER_DMA_SUPPORTED`

**RDMA Read vs Write:**
- **Write (default):** Client pushes data to server's buffer. Simpler, no server-side read limits.
- **Read (`-r`):** Client pulls data from server's buffer. Queue depth limited by server's `MaxOutboundReadLimit`.

**What it measures:**
- Per-operation RDMA latency (µs)
- CPU utilization
- Throughput (Bytes/sec)

**Requires:** Server + Client.

**Examples:**
```
# RDMA Write test
ndrping -s -p 192.168.1.10:54326
ndrping -c -p -w 192.168.1.10:54326

# RDMA Read test
ndrping -s -p 192.168.1.10:54326
ndrping -c -p -r 192.168.1.10:54326
```

---

### ndrpingpong — RDMA Write Latency (Bidirectional)

**Purpose:** Measures bidirectional RDMA Write ping-pong latency. Both sides perform one-sided RDMA writes to each other's memory, synchronizing via memory content polling.

**Usage:**
```
ndrpingpong [options] <ip>[:<port>]
Options:
  -s            - Start as server
  -c            - Start as client
  -b            - Blocking I/O
  -p            - Polling I/O (default)
  -n <nSge>     - SGE count (default: 1)
  -q <pipeline> - Pipeline limit
  -l <logFile>  - Log output
```

**Defaults:**
| Parameter | Value |
|-----------|-------|
| Port | 54327 |
| Max transfer size | 4 MB |
| Max iterations | 100,000 |
| Completion mode | Polling |

**Protocol:**
1. Both sides register buffers with `ALLOW_LOCAL_WRITE | ALLOW_REMOTE_WRITE`
2. Exchange PeerInfo (remote token + address) via send/receive
3. **Ping-pong loop:**
   - Client writes value `'X'` to server's buffer via RDMA Write
   - Server busy-polls its buffer for client's value, then writes `'Y'` back via RDMA Write
   - Client busy-polls its buffer for server's value
   - Repeat
4. Synchronization is entirely through memory content (no explicit control messages)

**Adapter requirement:** Must support `ND_ADAPTER_FLAG_IN_ORDER_DMA_SUPPORTED`

**What it measures:**
- Per-direction RDMA Write latency (round-trip / 2)
- CPU utilization
- Bidirectional throughput

**Requires:** Server + Client.

**Examples:**
```
ndrpingpong -s -p 192.168.1.10:54327
ndrpingpong -c -p 192.168.1.10:54327
```

---

## Unit Tests

### ndcq — Completion Queue Validation

**Purpose:** Validates Completion Queue (CQ) notification and cancellation behavior.

**Usage:**
```
ndcq [options] <IPv4 Address>
Options:
  -l,--logFile <logFile>   Log output to a given file
  -h,--help                Show this message
```

**Test scenarios:**
1. **In-order notify/cancel:** Tests CQ notification followed by cancellation in expected order
2. **Out-of-order notify/cancel:** Tests CQ cancellation before notification completes
3. **Notification interleaving:** Tests rapid notify/cancel sequences

**Pass criteria:** All operations complete without error; notifications and cancellations behave according to the ND specification.

**Requires:** Single node — **no server/client pair needed.**

**Example:**
```
ndcq 192.168.1.10
```

---

### ndconn — Connection Scalability

**Purpose:** Tests connection creation and teardown scalability across multiple threads.

**Usage:**
```
ndconn [options] <ip>[:<port>]
Options:
  -s              - Start as server
  -c              - Start as client
  -t <numThreads> - Number of threads (default: 2)
  -l <logFile>    - Log output
```

**Default port:** 54321

**What it tests:** Creates multiple simultaneous ND connections across configurable number of threads. Validates that the framework handles concurrent connection setup and teardown without errors.

**Pass criteria:** All connections established and torn down cleanly across all threads.

**Requires:** Server + Client.

**Examples:**
```
ndconn -s -t 4 192.168.1.10:54321
ndconn -c -t 4 192.168.1.10:54321
```

---

### ndmval — Memory Buffer Overflow Validation

**Purpose:** Validates error handling when a receive buffer is smaller than the incoming message.

**Usage:**
```
ndmval [options] <ip>[:<port>]
Options:
  -s            - Start as server
  -c            - Start as client
  -l <logFile>  - Log output
```

**Default port:** 54331

**What it tests:**
- Client sends 1024 bytes
- Server posts a receive with only a 1023-byte buffer
- Validates error codes:
  - **Client:** Expects `ND_SUCCESS` (iWARP) or `ND_REMOTE_ERROR` (InfiniBand)
  - **Server:** Expects `ND_BUFFER_OVERFLOW`

**Pass criteria:** Both sides report the expected error codes for their transport type.

**Requires:** Server + Client.

**Examples:**
```
ndmval -s 192.168.1.10:54331
ndmval -c 192.168.1.10:54331
```

---

### ndmw — Memory Window RDMA Validation

**Purpose:** Validates RDMA operations through Memory Windows on a 64 MB buffer.

**Usage:**
```
ndmw [options] <ip>[:<port>]
Options:
  -s            - Start as server
  -c            - Start as client
  -l <logFile>  - Log output
```

**Default port:** 54323

**What it tests:**
1. Server binds a Memory Window to a 64 MB registered buffer
2. Exchanges remote access tokens with client
3. Client performs RDMA Read and Write operations at various offsets
4. Validates data integrity using bitwise NOT and reverse-order transformations

**Pass criteria:** All RDMA operations succeed and buffer contents match expected transformations.

**Requires:** Server + Client.

**Examples:**
```
ndmw -s 192.168.1.10:54323
ndmw -c 192.168.1.10:54323
```

---

### ndmpic — MS-MPI Connection Emulation

**Purpose:** Emulates MS-MPI's rank-based connection establishment patterns to validate complex multi-connection scenarios.

**Usage:**
```
ndmpic [options] <local ip> <remote ip>
Options:
  -s            - Start as server (starts ranks 2 & 3)
  -c            - Start as client (starts ranks 0 & 1)
  -l <logFile>  - Log output
```

**Default port:** 54322

**What it tests:**
Simulates 4 MPI ranks across 2 processes:
- Client process hosts ranks 0 and 1
- Server process hosts ranks 2 and 3

Connection patterns tested:
1. **Active-passive:** Rank 0 → Rank 1, Rank 3 → Rank 2
2. **Active-active:** Rank 0 ↔ Rank 3, Rank 1 ↔ Rank 2
3. **Close message exchange** and acknowledgment protocol
4. **Multi-rank teardown** sequence

**Pass criteria:** All connection patterns establish and tear down without deadlock or error.

**Requires:** Server + Client (on two separate machines or IPs).

**Examples:**
```
ndmpic -s 192.168.1.20 192.168.1.10
ndmpic -c 192.168.1.10 192.168.1.20
```

---

### ndmemorytest — Comprehensive Stress/Error Test Suite

**Purpose:** A suite of 19 targeted test scenarios covering error handling, edge cases, and stress conditions across the NetworkDirect API surface.

**Usage:**
```
ndtest [options] <ip>[:<port>]
Options:
  -s            - Start as server
  -c            - Start as client
  -b            - Blocking I/O
  -p            - Polling I/O (default)
  -n <nSge>     - SGE count (default: 1)
  -q <pipeline> - Pipeline limit
  -l <logFile>  - Log output
  -t <testName> - Run specific test (see list below)
```

**Default port:** 54321

**Available tests:**

| Test Name | What It Validates |
|-----------|-------------------|
| `NdConnClose` | Connection close behavior |
| `NdConnListenClosing` | Listener closing during pending accept |
| `NdConnReject` | Connection rejection handling |
| `NdDualConnection` | Two simultaneous connections |
| `NdDualListen` | Two simultaneous listeners |
| `NdInvalidIP` | Invalid IP address error paths |
| `NdInvalidRead` | Invalid RDMA Read operations |
| `NdInvalidWrite` | Invalid RDMA Write operations |
| `NdLargePrivateData` | Oversized connection private data |
| `NdLargeQPDepth` | Maximum queue pair depth limits |
| `NdMRDeregister` | Memory region deregistration while in use |
| `NdMRInvalidBuffer` | Invalid buffer passed to memory region |
| `NdOverRead` | RDMA Read beyond registered bounds |
| `NdOverWrite` | RDMA Write beyond registered bounds |
| `NdQpMax` | Queue pair creation at maximum limits |
| `NdReceiveFlushQP` | Receive flushing on QP state change |
| `NdSendNoReceive` | Send without matching posted receive |
| `NdReceiveConnClosed` | Receive on a closed connection |
| `NdWriteViolation` | Write to memory without permission |

**Running all tests:** Omit `-t` to run all 19 tests sequentially.

**Running a specific test:**
```
# Server
ndtest -s -t NdWriteViolation 192.168.1.10:54321

# Client
ndtest -c -t NdWriteViolation 192.168.1.10:54321
```

**Pass criteria:** Each test validates that the API returns expected error codes and handles the error condition gracefully without crashes or hangs.

**Requires:** Server + Client (for all tests).

---

## Shared Libraries

### ndutil — Core NetworkDirect Framework

Static library providing the core ND runtime:
- **Provider discovery:** Enumerates Winsock catalog via `WSCEnumProtocols()`, filters for ND providers by service flags and version
- **Provider loading:** Dynamically loads provider DLLs via `DllGetClassObject`
- **Address management:** Maintains lists of ND v1 and v2 addresses (`m_NdAddrList`, `m_NdV1AddrList`)
- **Adapter lifecycle:** `NdStartup()`, `NdOpenAdapter()`, `NdCleanup()`
- **Message compiler output:** Generates `ndstatus.h` from `ndstatus.mc`

### ndtestutil — Test Utility Library

Static library providing base classes for test infrastructure:
- **`NdTestBase`** — Wraps adapter, CQ, QP, MR, and connector creation
- **`NdTestServerBase`** — Adds listener functionality
- **`NdTestClientBase`** — Client-side connection helpers
- All methods call real IND2 interfaces directly (no mocking)

---

## Quick Reference: Default Ports

| Tool | Port |
|------|------|
| ndconn | 54321 |
| ndmemorytest | 54321 |
| ndmpic | 54322 |
| ndmw | 54323 |
| ndping | 54324 |
| ndpingpong | 54325 |
| ndrping | 54326 |
| ndrpingpong | 54327 |
| ndmval | 54331 |
