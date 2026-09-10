# NetworkDirect SPI

The NetworkDirect architecture provides application developers with a networking interface that enables zero-copy data transfers between applications, kernel-bypass I/O generation and completion processing, and one-sided data transfer operations. The NetworkDirect service provider interface (SPI) defines the interface that NetworkDirect providers implement to expose their hardware capabilities to applications.

Please find additional documentation in [docs/](./docs) folder.

NetworkDirect SDK is available in [Nuget](https://www.nuget.org/packages/networkdirect) also.

# Building

## Prerequisites

 - [Visial Studio 2017](https://docs.microsoft.com/visualstudio/install/install-visual-studio)

   Please make sure to select the following workloads during installation:
    - .NET desktop development (required for CBT/Nuget packages)
    - Desktop development with C++ 

 - [Windows SDK](https://developer.microsoft.com/windows/downloads/windows-10-sdk)
 - [Windows WDK](https://docs.microsoft.com/windows-hardware/drivers/download-the-wdk)
 
 Based on the installed VS/SDK/WDK versions, update _VCToolsVersion_ and _WindowsTargetPlatformVersion_ in Directory.Build.props
 
 Note that the build system uses [CommonBuildToolSet(CBT)](https://commonbuildtoolset.github.io/). You may need to unblock __CBT.core.dll__ (under .build/CBT) depending on your security configurations. Please refer to [CBT documentation](https://commonbuildtoolset.github.io/#/getting-started) for additional details.


 ## Build
 To build, open a __Native Tools Command Prompt for Visual Studio__ and  run ``msbuild`` from root folder.

## Experimental features in this branch

- **NetworkDirect v2 provider:** `ndprov.dll` implements the NDv2 provider interface to support RDMA over usb4 connection. It works in combination with usb4 drivers that are shipped separately.
- **Provider management:** `ndprov_install.exe` installs, lists, and removes `ndprov.dll` from the Winsock provider catalog.
- **Transfer samples:** `transfer_file.exe` transfers files over NetworkDirect, `transfer_file_sockets.exe` provides a TCP comparison, and `share_screen.exe` streams the desktop over NetworkDirect.
- **Test documentation:** [docs/test-suite-reference.md](./docs/test-suite-reference.md) describes the included examples and validation tools.

### Install `ndprov.dll`

Build the x64 provider and installer:

```bat
msbuild src\netdirect.sln /m /p:Configuration=Release /p:Platform=x64
```

Then open an **Administrator** command prompt and install the provider using an absolute DLL path:

```bat
out\Release-x64\ndprov_install\ndprov_install.exe install "C:\full\path\to\NetworkDirect\out\Release-x64\ndprov\ndprov.dll"
```

Keep `ndprov.dll` at the registered path. To list NetworkDirect providers or remove `ndprov.dll`, run:

```bat
out\Release-x64\ndprov_install\ndprov_install.exe dump
out\Release-x64\ndprov_install\ndprov_install.exe uninstall
```

### Run the transfer samples

Start the receiver first, then the sender:

```bat
transfer_file.exe -s <server-ip>
transfer_file.exe -c <server-ip> <file-path>
```

Use `transfer_file_sockets.exe` with the same `-s` and `-c` arguments for a TCP baseline. Run `share_screen.exe -s <server-ip>` on the display host and `share_screen.exe -c <server-ip>` on the capture host.

## Setting up a USB4 test bed for RDMA

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
