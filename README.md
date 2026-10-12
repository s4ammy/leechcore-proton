# LeechCore Proton Connector

This connector lets MemProcFS inspect the live memory of one 64-bit Windows
process running under Proton or Wine. It turns the process's Linux virtual
memory mappings into a small x86-64 memory image that MemProcFS can read.

This is a process-level view, not an emulated Windows kernel. Virtual memory
reads, address translation, page permissions, binary searches, and YARA
searches are useful. Kernel-backed features such as the registry, services,
drivers, handles, and a complete Windows process list are not available.
MemProcFS exposes the selected process as PID 1 named `unknown_process`.

## Quick check

The included fixture is the easiest way to confirm that everything works:

```bash
./example.sh
./benchmark.sh 1 256
```

`example.sh` prints a live hex dump from a temporary Windows process.
`benchmark.sh` measures reads through MemProcFS and LeechCore. Both scripts
build missing components, launch the fixture, locate its Linux host PID, and
clean up afterward. They should not normally need `sudo`.

Run `./benchmark.sh` without arguments for the full benchmark sweep. Set
`PROTON_BIN` or `WINE_BIN` if automatic runtime detection chooses the wrong
executable.

## Releases

Every push to `main` builds and tests the Linux x86-64 connector, then updates
the rolling `latest` prerelease on GitHub. It contains the standalone plugin,
an archive with installation notes, and SHA-256 checksums.

For a normal versioned release, push a tag beginning with `v`:

```bash
git tag v0.1.0
git push origin v0.1.0
```

The downloaded `leechcore_device_proton.so` must be placed beside the exact
`leechcore.so` loaded by MemProcFS, PCILeech, or another LeechCore client.

## Build

You need a C11 compiler, GNU make, and a Linux x86-64 build of LeechCore 2.23
or newer. MinGW-w64 is needed for the test fixture, and the standalone
MemProcFS frontend also needs the FUSE development package.

The upstream projects are local build dependencies and are intentionally not
stored in this repository. Clone the two required projects into
`references/` after checking out this project:

```bash
mkdir -p references
git clone https://github.com/ufrisk/LeechCore.git references/LeechCore
git clone https://github.com/ufrisk/MemProcFS.git references/MemProcFS
```

PCILeech and the official LeechCore plugins are useful for compatibility work,
but are not required to build this connector:

```bash
git clone https://github.com/ufrisk/pcileech.git references/PCILeech
git clone https://github.com/ufrisk/LeechCore-plugins.git references/LeechCore-plugins
```

Build against the local reference checkouts:

```bash
make -C references/LeechCore/leechcore leechcore
install -m 0755 \
    references/LeechCore/files/leechcore.so \
    references/MemProcFS/files/leechcore.so
make -C references/MemProcFS/vmm vmm

make
make test-unit
```

Build as your normal user. Root access may be useful when attaching to a real
target, but build artifacts should not be owned by root.

For an external LeechCore checkout:

```bash
make \
    LEECHCORE_DIR=/path/to/LeechCore \
    LEECHCORE_SO=/path/to/leechcore.so
```

## Are upstream patches required?

No patches are needed for LeechCore or standalone MemProcFS when using the
versions in `references/`.

- LeechCore already loads external device modules. A `proton://` device makes
  it load `leechcore_device_proton.so` and call `LcPluginCreate()`.
- MemProcFS already accepts a supplied x64 DTB. The options
  `-arch x64 -dtb 0x1000` create the generic process view used here.
- This project does not modify the reference LeechCore, MemProcFS, or PCILeech
  source trees.

The connector must be beside the exact `leechcore.so` loaded by the
application. If you use an older or different LeechCore build, rebuild the
connector against its headers and check that its external-device ABI is
compatible.

PCILeech can load the connector without patching LeechCore. Physical-memory
commands and page-table tools work with the synthetic DTB. For example:

```bash
pcileech pt_virt2phys \
    -device "proton://pid=${targetPid},ro=1" \
    -cr3 0x1000 \
    -0 0x140000000
```

One PCILeech caveat remains. Its process-aware virtual-memory commands do not
currently pass `-cr3` into their embedded MemProcFS initialization, so they
will not create `unknown_process` automatically. Supporting those particular
commands needs a small PCILeech change. Standalone MemProcFS does not have this
problem and is the recommended interface for process inspection.

## Use with standalone MemProcFS

### 1. Stage the connector

LeechCore searches for device modules in the directory containing the
`leechcore.so` it loaded. For the included checkout:

```bash
runtimeDir="$PWD/references/MemProcFS/files"

make install-dev LEECHCORE_INSTALL_DIR="$runtimeDir"
make -C references/MemProcFS/memprocfs memprocfs
```

That directory should now contain `memprocfs`, `vmm.so`, `leechcore.so`,
and `leechcore_device_proton.so`.

### 2. Find the Linux host PID

Use the Linux PID of the process that maps the Windows executable. Do not use
a Wine-internal PID, `wineserver`, or the Proton launcher.

```bash
pgrep -af '\.exe'

targetPid=12345
ps -p "$targetPid" -o pid,ppid,cmd
grep -i 'TargetApplication.exe' "/proc/$targetPid/maps"
```

The final command should show mappings for the application you want to inspect.

### 3. Mount it

```bash
runtimeDir="$PWD/references/MemProcFS/files"
mkdir -p "$PWD/mnt"

sudo "$runtimeDir/memprocfs" \
    -device "proton://pid=${targetPid},ro=1" \
    -arch x64 \
    -dtb 0x1000 \
    -norefresh \
    -mount "$PWD/mnt"
```

`0x1000` is the fixed address of the connector's synthetic page tables.
`-norefresh` disables kernel-oriented refresh work that is not useful for
this process-only view.

The target is available under `pid/1`:

```bash
ls "$PWD/mnt/pid/1"
xxd -s 0x140000000 -l 64 "$PWD/mnt/pid/1/memory.vmem"
```

Offsets in `memory.vmem` are target virtual addresses. Replace the example
address with one that is mapped in your process.

Unmount with the FUSE command provided by your distribution:

```bash
sudo fusermount -u "$PWD/mnt"
# Or: sudo fusermount3 -u "$PWD/mnt"
```

## Permissions

The connector uses `process_vm_readv()` and, when enabled,
`process_vm_writev()`. Linux applies the same ptrace security rules used for
debuggers. Wine may reparent the Windows process, so
`kernel.yama.ptrace_scope=1` can block an unrelated same-user reader.
Running MemProcFS with `sudo` is the simplest development setup.

If attachment fails, confirm that:

- The PID owns the executable mappings you expect.
- MemProcFS can see `/proc/<pid>/maps` in the same PID namespace.
- A Steam Flatpak or other sandbox is not hiding the process.
- Root or the MemProcFS process has `CAP_SYS_PTRACE` when required.
- Anti-cheat or another security layer is not blocking inspection.

The test scripts preload a permission helper only inside their disposable
Proton or Wine prefix. They do not change the global ptrace setting or apply
that helper to normal applications. This connector is intended for software
you are allowed to inspect.

## Connector options

```text
proton://pid=<linux-host-pid>,ro=<0|1>
```

- `pid` is required and is a positive decimal Linux PID.
- `ro` defaults to `1`, which is the recommended mode.
- `ro=0` permits writes only to mappings that were writable when the
  connector opened. Synthetic page tables and read-only mappings stay
  protected.

The connector snapshots `/proc/<pid>/maps` when it opens but reads memory
live. Restart MemProcFS if the target later creates mappings you need. Removed
mappings fail cleanly, and process start-time checks detect PID reuse.

## Examples and tests

The C example translates an address, shows its PTE permissions, and prints a
live hex dump:

```bash
make example
./build/proton_example <linux-host-pid> <virtual-address> [byte-count]
```

The benchmark measures end-to-end virtual reads, including MemProcFS
translation and LeechCore scatter dispatch:

```bash
make benchmark
./build/proton_benchmark \
    <linux-host-pid> <page-aligned-address> <page-aligned-size> \
    [seconds] [batch-pages]
```

Unit tests do not need Proton, Wine, root, or ptrace access:

```bash
make test-unit
make test-sanitized
```

The integration test launches the Windows fixture:

```bash
make fixture
PROTON_BIN=/path/to/proton make test-integration
```

Set `PROTON_WRITE_TEST=1` to include opt-in write checks. `WINE_BIN` is
accepted as a development fallback.

## Limitations

- One 64-bit Proton or Wine process per connector instance.
- Four-level x86-64 paging and lower canonical addresses only.
- The selected process appears as PID 1 named `unknown_process`.
- Reads are live, not a consistent whole-process snapshot.
- Mapping names are not included in PTE descriptions.
- 32-bit processes and new WoW64 layouts are not supported.
