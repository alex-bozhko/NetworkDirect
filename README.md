# NetworkDirect over USB4

This project lets Windows PCs that are connected directly by a USB4 cable talk to each other through RDMA verbs. Applications get zero-copy transfers, kernel-bypass I/O, and one-sided operations without a dedicated RDMA network adapter. The goal is a low-latency, high-throughput interconnect that lets AI frameworks split model inference and training across several ordinary PCs.

The project provides `ndprov.dll`, a NetworkDirect v2 (NDv2) provider that runs on top of the USB4 P2P networking drivers built into Windows. Applications use it through the standard NetworkDirect interfaces (`IND2Provider`, `IND2Adapter`, `IND2QueuePair`, and so on). The provider currently supports two-sided send and receive operations.

## Contents

| Path | Description |
|------|-------------|
| `src/ndprov` | `ndprov.dll`, the NDv2 provider. |
| `src/install` | `ndprov_install.exe`, which adds `ndprov.dll` to or removes it from the Winsock provider catalog. |
| `src/ndutil` | NetworkDirect headers and the helper library that applications use to find and load providers. |
| `src/examples` | `ndping`, `ndlat`, `ndtput`, and other test tools. |
| `docs` | NetworkDirect interface reference and [test tool reference](./docs/test-suite-reference.md). |

# Building

## Prerequisites

- [Visual Studio 2022](https://visualstudio.microsoft.com/downloads/) with the **Desktop development with C++** workload.
- Windows SDK 10.0.26100 or later.
- The [NuGet CLI](https://learn.microsoft.com/nuget/install-nuget-client-tools), to restore packages from the command line.

## Build

`ndprov`, `ndlat`, and `ndtput` use the [Windows Implementation Library (WIL)](https://github.com/microsoft/wil) NuGet package. Restore it before building:

```bat
nuget restore src\netdirect.sln -PackagesDirectory packages
```

Then build from a **Developer Command Prompt for Visual Studio**:

```bat
msbuild src\netdirect.sln /m /p:Configuration=Release /p:Platform=x64
```

Binaries are written to `out\<Configuration>-<Platform>\<project>\`.

To stamp a version into the binaries, add `/p:NdVersion=<major>.<minor>.<patch>.<build>`. Without it, the binaries report version 1.0.0.1.

## Releases

Pushing a tag of the form `vMAJOR.MINOR.PATCH` (for example `v1.2.0`) runs the [Release workflow](.github/workflows/release.yml). It builds Release x64 and publishes a GitHub release containing:

- `NetworkDirect-USB4-<version>-x64.zip`: `ndprov.dll`, `ndprov_install.exe`, the tools in `tools\`, and the tests in `tests\`.
- `NetworkDirect-USB4-<version>-x64-symbols.zip`: the matching `.pdb` files.
- `SHA256SUMS.txt`: checksums for both archives.

A tag with a suffix, such as `v1.2.0-rc.1`, creates a prerelease. You can also run the workflow manually from the Actions tab; this uploads the archives as workflow artifacts without creating a release.

# Using the provider

## Install `ndprov.dll`

Open an **Administrator** command prompt and install the provider using an absolute DLL path:

```bat
out\Release-x64\ndprov_install\ndprov_install.exe install "C:\full\path\to\out\Release-x64\ndprov\ndprov.dll"
```

Keep `ndprov.dll` at the registered path. To list NetworkDirect providers or remove `ndprov.dll`, run:

```bat
out\Release-x64\ndprov_install\ndprov_install.exe dump
out\Release-x64\ndprov_install\ndprov_install.exe uninstall
```

## Collect `ndprov.dll` traces

`ndprov.dll` logs through a TraceLogging ETW provider named `Microsoft.NetworkDirect.NdProv` (GUID `{B71AF425-C11E-54C8-BD6E-20FFB13DCADB}`). Nothing is logged unless a trace session enables it. From an elevated prompt:

```bat
logman start ndprov -p "{B71AF425-C11E-54C8-BD6E-20FFB13DCADB}" 0xffffffffffffffff 0xff -o ndprov.etl -ets
rem ... run the workload ...
logman stop ndprov -ets
tracerpt ndprov.etl -o ndprov.xml -of XML -y
```

## Set up a USB4 test bed

Connecting two machines with a USB4 (Thunderbolt) cable creates a `USB4(TM) P2P Network Adapter` on each end. Before the tests will run reliably, check two things: the network profile and the IP addressing. Both cause failures that look like broken hardware but are not.

### 1. Check the network profile

A USB4 link has no gateway and no DNS, so Windows cannot identify it. It shows up as **"Unidentified network"**, and Windows puts every unidentified network in the **Public** profile.

```powershell
Get-NetConnectionProfile | Where-Object InterfaceAlias -like 'Ethernet ?'
Get-NetFirewallProfile | Select-Object Name, Enabled
```
So if inbound traffic fails, check the profile first. How you open it up is your choice: move the adapters to `Private` with `Set-NetConnectionProfile`, add rules scoped to those interfaces with `New-NetFirewallRule -InterfaceAlias`, or turn the firewall off on an isolated test machine. Whichever you pick, re-check after a reconnect.

### 2. Give every link its own subnet

Left alone, each adapter self-assigns a link-local address like `169.254.x.x` with a `/16` mask. Every adapter then claims the same `169.254.0.0/16` network.

That breaks `NdResolveAddress`, which ndprov.dll uses to identify the virtual miniport to initiate RDMA verbs from. It chooses the local address by looking up a route to the peer. When several adapters cover the same range the lookup is ambiguous, so Windows returns whichever interface has the lowest metric, not the one the cable is plugged into. Traffic leaves the wrong port and the connection fails.

The fix is to put each cable in its own subnet, so a peer address can match only one adapter. A `/30` is a good size: it holds exactly two usable addresses, which is all a point-to-point link needs.

Example with three machines and three cables:

```
LINK   MACHINE  ADAPTER     IP ADDRESS    MASK             PEER
----   -------  ----------  ------------  ---------------  -----
A      RDMA1    Ethernet 2  192.168.10.1  255.255.255.252  RDMA4
A      RDMA4    Ethernet 3  192.168.10.2  255.255.255.252  RDMA1
B      RDMA1    Ethernet 3  192.168.20.1  255.255.255.252  RDMA2
B      RDMA2    Ethernet 3  192.168.20.2  255.255.255.252  RDMA1
C      RDMA2    Ethernet 2  192.168.30.1  255.255.255.252  RDMA4
C      RDMA4    Ethernet 2  192.168.30.2  255.255.255.252  RDMA2
```

Run `ipconfig /all` on each machine first and match adapters to cables by MAC address. Interface names and indexes are not the same across machines.

Then, on each machine, for each USB4 adapter:

```powershell
$alias = 'Ethernet 2'          # the USB4 adapter
$ip    = '192.168.10.1'        # its address from your table

Set-NetIPInterface  -InterfaceAlias $alias -AddressFamily IPv4 -Dhcp Disabled
Remove-NetIPAddress -InterfaceAlias $alias -AddressFamily IPv4 -Confirm:$false -EA SilentlyContinue
Remove-NetRoute     -InterfaceAlias $alias -AddressFamily IPv4 -Confirm:$false -EA SilentlyContinue
New-NetIPAddress    -InterfaceAlias $alias -IPAddress $ip -PrefixLength 30
```

No gateway and no manual routes are needed. Each `/30` adds one on-link route, and the routes do not overlap.

### 3. Verify

`Find-NetRoute` resolves an address the same way `NdResolveAddress` does, so it tells you in advance what the test will pick:

```powershell
Find-NetRoute -RemoteIPAddress 192.168.10.2 | Select-Object InterfaceAlias, IPAddress
```

It must name one interface, and it must be the one holding the matching cable. 

Finally, run the test itself. `-s` starts the listener; `-c` takes **the server's** address, not the local one:

```bat
ndping.exe -s 192.168.10.1      REM on RDMA1
ndping.exe -c 192.168.10.1      REM on RDMA4
```

Pointing `-c` at the client's own address is an easy mistake and returns `0x8007274d` (connection refused).

# Contributing

This project welcomes contributions and suggestions.  Most contributions require you to agree to a
Contributor License Agreement (CLA) declaring that you have the right to, and actually do, grant us
the rights to use your contribution. For details, visit https://cla.microsoft.com.

When you submit a pull request, a CLA-bot will automatically determine whether you need to provide
a CLA and decorate the PR appropriately (e.g., label, comment). Simply follow the instructions
provided by the bot. You will only need to do this once across all repos using our CLA.

This project has adopted the [Microsoft Open Source Code of Conduct](https://opensource.microsoft.com/codeofconduct/).
For more information see the [Code of Conduct FAQ](https://opensource.microsoft.com/codeofconduct/faq/) or
contact [opencode@microsoft.com](mailto:opencode@microsoft.com) with any additional questions or comments.
