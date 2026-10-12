#!/usr/bin/env python3
import argparse
import ctypes
import errno
import os
import struct
import sys
import time

try:
    import memprocfs
except ImportError as exc:
    print(f"SKIP: memprocfs Python module is unavailable: {exc}", file=sys.stderr)
    raise SystemExit(77)

MARKER = b"MEMPROCFS_PROTON_TEST_V1"
SEQUENCE_OFFSET = 64
SELF_OFFSET = 72
COMMAND_OFFSET = 80
ACK_OFFSET = 88
BOUNDARY_OFFSET = 4096 - 32
BOUNDARY_SIZE = 64
ACK_XOR = 0xA55AA55ADEADBEEF


class IOVec(ctypes.Structure):
    _fields_ = [("base", ctypes.c_void_p), ("length", ctypes.c_size_t)]


def arguments():
    parser = argparse.ArgumentParser()
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--address", type=lambda value: int(value, 0), required=True)
    parser.add_argument("--size", type=lambda value: int(value, 0), required=True)
    parser.add_argument("--ro", type=lambda value: int(value, 0), required=True)
    parser.add_argument("--rw", type=lambda value: int(value, 0), required=True)
    parser.add_argument("--rx", type=lambda value: int(value, 0), required=True)
    parser.add_argument("--test-write", action="store_true")
    return parser.parse_args()


def open_vmm(pid, read_only):
    return memprocfs.Vmm([
        "-device", f"proton://pid={pid},ro={1 if read_only else 0}",
        "-arch", "x64",
        "-dtb", "0x1000",
        "-norefresh",
    ])


def preflight_process_access(pid, address):
    libc = ctypes.CDLL(None, use_errno=True)
    operation = libc.process_vm_readv
    operation.argtypes = [ctypes.c_int, ctypes.POINTER(IOVec), ctypes.c_ulong,
                          ctypes.POINTER(IOVec), ctypes.c_ulong, ctypes.c_ulong]
    operation.restype = ctypes.c_ssize_t
    buffer = ctypes.create_string_buffer(len(MARKER))
    local = IOVec(ctypes.cast(buffer, ctypes.c_void_p), len(MARKER))
    remote = IOVec(ctypes.c_void_p(address), len(MARKER))
    transferred = operation(pid, ctypes.byref(local), 1, ctypes.byref(remote), 1, 0)
    if transferred < 0 and ctypes.get_errno() in (errno.EPERM, errno.EACCES):
        print("SKIP: process_vm_readv permission denied. Check same-user/parent "
              "access, kernel.yama.ptrace_scope, CAP_SYS_PTRACE or root, Steam "
              "Flatpak/PID namespaces, and the Linux host PID.", file=sys.stderr)
        raise SystemExit(77)
    if transferred != len(MARKER) or buffer.raw != MARKER:
        code = ctypes.get_errno() if transferred < 0 else 0
        raise AssertionError(f"direct process_vm_readv preflight failed: "
                             f"result={transferred} errno={code}")


def read_exact(process, address, size):
    value = process.memory.read(address, size, memprocfs.FLAG_NOCACHE)
    assert len(value) == size, f"short read at 0x{address:x}: {len(value)} != {size}"
    return value


def pte_for(entries, address):
    for entry in entries:
        if entry["va"] <= address < entry["va"] + entry["size"]:
            return entry
    raise AssertionError(f"no PTE map entry covers 0x{address:x}")


def wait_for_exit(pid, timeout=10.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if not os.path.exists(f"/proc/{pid}"):
            return
        time.sleep(0.05)
    raise AssertionError(f"target PID {pid} did not exit")


def test_read_path(args):
    vmm = open_vmm(args.pid, True)
    process = vmm.process(1)

    assert read_exact(process, args.address, len(MARKER)) == MARKER
    self_pointer = struct.unpack("<Q", read_exact(process, args.address + SELF_OFFSET, 8))[0]
    assert self_pointer == args.address

    sequence_one = struct.unpack("<Q", read_exact(process, args.address + SEQUENCE_OFFSET, 8))[0]
    time.sleep(0.15)
    sequence_two = struct.unpack("<Q", read_exact(process, args.address + SEQUENCE_OFFSET, 8))[0]
    assert sequence_two > sequence_one, (sequence_one, sequence_two)

    expected_boundary = bytes((index * 37 + 11) & 0xFF for index in range(BOUNDARY_SIZE))
    assert read_exact(process, args.address + BOUNDARY_OFFSET, BOUNDARY_SIZE) == expected_boundary

    ptes = process.maps.pte(False)
    main_pte = pte_for(ptes, args.address)
    ro_pte = pte_for(ptes, args.ro)
    rw_pte = pte_for(ptes, args.rw)
    rx_pte = pte_for(ptes, args.rx)
    assert main_pte["flags"][2] == "w"
    assert ro_pte["flags"][2] == "-" and ro_pte["flags"][3] == "-"
    assert rw_pte["flags"][2] == "w"
    assert rx_pte["flags"][2] == "-" and rx_pte["flags"][3] == "x"

    translated = process.memory.virt2phys(args.address)
    assert translated != 0

    search = process.search(args.address & ~0xFFF,
                            (args.address + args.size + 0xFFF) & ~0xFFF,
                            memprocfs.FLAG_NOCACHE)
    search.strategy = "pte"
    search.add_search(MARKER, None, 1)
    results = search.result()
    assert any(result[0] == args.address for result in results), results

    try:
        unmapped = process.memory.read(0, 16, memprocfs.FLAG_NOCACHE)
    except RuntimeError:
        unmapped = b""
    assert len(unmapped) != 16

    try:
        process.memory.write(args.address + COMMAND_OFFSET, struct.pack("<Q", 1))
        raise AssertionError("read-only VMM unexpectedly accepted a write")
    except RuntimeError:
        pass

    vmm.close()
    os.kill(args.pid, 0)


def test_write_path(args):
    vmm = open_vmm(args.pid, False)
    process = vmm.process(1)
    nonce = (time.monotonic_ns() | 1) & 0xFFFFFFFFFFFFFFFF
    process.memory.write(args.address + COMMAND_OFFSET, struct.pack("<Q", nonce))
    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline:
        ack = struct.unpack("<Q", read_exact(process, args.address + ACK_OFFSET, 8))[0]
        if ack == (nonce ^ ACK_XOR):
            break
        time.sleep(0.05)
    else:
        raise AssertionError("fixture did not acknowledge the write")

    for memory, address, label in ((process.memory, args.ro, "read-only virtual page"),
                                   (vmm.memory, 0x1000, "synthetic PML4")):
        try:
            memory.write(address, b"12345678")
            raise AssertionError(f"write to {label} unexpectedly succeeded")
        except RuntimeError:
            pass
    vmm.close()


def test_target_exit(args):
    vmm = open_vmm(args.pid, True)
    process = vmm.process(1)
    assert read_exact(process, args.address, len(MARKER)) == MARKER
    os.kill(args.pid, 15)
    wait_for_exit(args.pid)
    try:
        value = process.memory.read(args.address, len(MARKER), memprocfs.FLAG_NOCACHE)
    except RuntimeError:
        value = b""
    assert value != MARKER
    vmm.close()


def main():
    args = arguments()
    preflight_process_access(args.pid, args.address)
    test_read_path(args)
    if args.test_write:
        test_write_path(args)
    test_target_exit(args)
    print("test_memprocfs_proton: PASS")


if __name__ == "__main__":
    main()
