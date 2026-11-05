# NetworkDirect Test Suite — Detailed Reference

This document provides a comprehensive overview of all example and unit test projects in the NetworkDirect solution, including usage instructions, protocol details, and measurement methodology.

> **Important:** All tests run against a real NetworkDirect provider, such as ndprov over a USB4 peer-to-peer link; there are no mocks or stubs. All tests accept IPv4 addresses only.

---

## Table of Contents

- [Examples](#examples)
  - [ndadapterinfo — Adapter Capabilities Query](#ndadapterinfo--adapter-capabilities-query)
  - [ndcat — Address Catalog Validation](#ndcat--address-catalog-validation)
  - [ndmrlat — Memory Registration Latency](#ndmrlat--memory-registration-latency)
  - [ndmrrate — Memory Registration Throughput](#ndmrrate--memory-registration-throughput)
  - [ndping — Send/Recv Latency (Unidirectional)](#ndping--sendrecv-latency-unidirectional)
  - [ndlat — Send/Recv Round-Trip Latency](#ndlat--sendrecv-round-trip-latency)
  - [ndtput — Send/Recv Throughput](#ndtput--sendrecv-throughput)
- [Unit Tests](#unit-tests)
  - [ndcq — Completion Queue Validation](#ndcq--completion-queue-validation)
  - [ndconn — Connection Scalability](#ndconn--connection-scalability)
  - [ndmval — Memory Buffer Overflow Validation](#ndmval--memory-buffer-overflow-validation)
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

### ndlat — Send/Recv Round-Trip Latency

**Purpose:** Measures send/receive round-trip latency at a fixed message size and reports a percentile distribution. The client defines the workload and sends it to the server over a control exchange, so the server needs no workload options.

**Usage:**
```
Server: ndlat -s [-1] <ip>[:port]
Client: ndlat -c [-d] [-b] [-r] [-i N] [-w N] [-z bytes] [-u sec] [-o us] <ip>[:port]
Options:
  -s            - Start as server
  -1            - Server exits after one client
  -c            - Start as client
  -d            - Bidirectional (both flows share one QP)
  -b            - Blocking I/O (default: polling)
  -r            - Realtime thread priority
  -i <N>        - Measured iterations
  -w <N>        - Warm-up iterations
  -z <bytes>    - Message size
  -u <sec>      - Periodic statistics interval
  -o <us>       - Report per-phase timing of exchanges slower than this RTT
  -h            - Help
```

**Defaults:**
| Parameter | Value |
|-----------|-------|
| Port | 54324 |
| Iterations | 1,000 |
| Warm-up iterations | 0 |
| Message size | 64 bytes |
| Max message size | 4 MB |
| Completion mode | Polling |

**What it measures:**
- RTT min / average / P50 / P90 / P99 / P99.9 / max / standard deviation (µs)
- Exchange rate and data rate (request + echo bytes)
- Optional per-cycle statistics (`-u`) and outlier phase breakdown (`-o`)

**Requires:** Server + Client.

**Examples:**
```
ndlat -s 192.168.1.10
ndlat -c -z 4096 -i 100000 -w 1000 192.168.1.10
```

---

### ndtput — Send/Recv Throughput

**Purpose:** Measures streaming send/receive throughput over one or more queue pairs. The client defines the workload; the server mirrors it. Each run ends with a Last/Ack handshake so both sides agree on the final byte counts.

**Usage:**
```
Server: ndtput -s [-1] <ip>[:port]
Client: ndtput -c [-z bytes] [-q depth] [-t sec | -i msgs] [-w warmup] [-n qps] [-T] [-d] [-b] [-I] [-r] [-u sec] <ip>[:port]
Options:
  -s            - Start as server
  -1            - Server exits after one client
  -c            - Start as client
  -z <bytes>    - Message size
  -q <depth>    - Outstanding sends per QP
  -t <sec>      - Measured streaming time
  -i <msgs>     - Measured messages per QP and direction (instead of -t)
  -w <warmup>   - Warm-up (seconds with -t, messages with -i)
  -n <qps>      - Queue pairs (one connection and CQ each)
  -T            - One thread per QP
  -d            - Bidirectional on every QP
  -b            - Blocking I/O (default: polling)
  -I            - Blocking mode via an I/O completion port (not with -T)
  -r            - Realtime priority
  -u <sec>      - Periodic statistics interval
  -h            - Help
```

**Defaults:**
| Parameter | Value |
|-----------|-------|
| Port | 54326 |
| Message size | 64 KB |
| Max message size | 4 MB |
| Queue depth | 16 (max 256) |
| Duration | 5 s |
| Warm-up | 1 s |
| Queue pairs | 1 (max 3) |
| Completion mode | Polling |

**What it measures:**
- TX/RX throughput per QP and in total (MB/s and Gb/s are decimal units)
- Message rate
- CPU time, as a percentage of one core
- A machine-parsable `RESULT role=... dir=... MBps=... Gbps=...` line

**Requires:** Server + Client.

**Examples:**
```
ndtput -s 192.168.1.10
ndtput -c -z 1048576 -q 32 -t 10 -n 2 -d 192.168.1.10
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
  - **Client:** Expects `ND_SUCCESS` or `STATUS_DATA_ERROR`, depending on whether the provider reports the receiver-side overflow back to the sender
  - **Server:** Expects `ND_BUFFER_OVERFLOW`

**Pass criteria:** Both sides report the expected error codes.

**Requires:** Server + Client.

**Examples:**
```
ndmval -s 192.168.1.10:54331
ndmval -c 192.168.1.10:54331
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

**Purpose:** A suite of 13 targeted test scenarios covering error handling, edge cases, and stress conditions across the NetworkDirect API surface.

**Usage:**
```
ndmemorytest [options] <ip>[:<port>]
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
| `NdLargePrivateData` | Oversized connection private data |
| `NdLargeQPDepth` | Maximum queue pair depth limits |
| `NdMRInvalidBuffer` | Invalid buffer passed to memory region |
| `NdQpMax` | Queue pair creation at maximum limits |
| `NdReceiveFlushQP` | Receive flushing on QP state change |
| `NdSendNoReceive` | Send without matching posted receive |
| `NdReceiveConnClosed` | Receive on a closed connection |

**Running a test:** `-t` is required; each run executes one test.

```
# Server
ndmemorytest -s -t NdLargePrivateData 192.168.1.10:54321

# Client
ndmemorytest -c -t NdLargePrivateData 192.168.1.10:54321
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
| ndping | 54324 |
| ndmval | 54331 |
| ndlat | 54324 (same as ndping) |
| ndtput | 54326 |

Tools that share a default port (ndconn and ndmemorytest, ndping and ndlat) cannot run at the same time on the same address unless you pass a different port.
