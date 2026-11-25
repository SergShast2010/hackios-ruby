#!/usr/bin/env python3
"""Host-side client for HakiOS ruby TCP console (:2323) and USB QMU DMA benchmarks (:2324 TX / :2325 RX)."""
import argparse
import select
import socket
import sys
import time
import zlib

CHUNK = 65536


def make_pattern_chunk() -> bytes:
    return bytes((((i * 131) ^ ((i >> 8) * 17)) + 0x5A) & 0xFF for i in range(CHUNK))


def run_cmd(host: str, port: int, cmd: str, timeout: float = 10.0) -> str:
    with socket.create_connection((host, port), timeout=timeout) as s:
        s.settimeout(timeout)
        # Drain initial banner up to first prompt
        buf = b""
        while b"hakios> " not in buf:
            d = s.recv(4096)
            if not d:
                break
            buf += d
        s.sendall((cmd.strip() + "\n").encode("utf-8"))
        out = b""
        if cmd.strip() in ("return", "quit", "exit", "bye"):
            while True:
                try:
                    d = s.recv(4096)
                except socket.timeout:
                    break
                if not d:
                    break
                out += d
            return out.decode("utf-8", errors="replace")
        while b"hakios> " not in out:
            d = s.recv(4096)
            if not d:
                break
            out += d
        text = out.decode("utf-8", errors="replace")
        if text.endswith("hakios> "):
            text = text[:-len("hakios> ")]
        return text


def run_shell(host: str, port: int) -> None:
    print(f"Connecting to {host}:{port} ... (Ctrl+C or 'quit' to disconnect, 'return' to exit to UEFI)")
    with socket.create_connection((host, port), timeout=10.0) as s:
        s.setblocking(False)
        while True:
            rlist, _, _ = select.select([s, sys.stdin], [], [])
            if s in rlist:
                data = s.recv(4096)
                if not data:
                    print("\n[connection closed]")
                    return
                sys.stdout.write(data.decode("utf-8", errors="replace"))
                sys.stdout.flush()
            if sys.stdin in rlist:
                line = sys.stdin.readline()
                if not line:
                    return
                s.sendall(line.encode("utf-8"))


def run_rxbench(host: str, port: int, mb: int, timeout: float = 60.0) -> bool:
    pat = make_pattern_chunk()
    blocks = mb * 16
    total = blocks * CHUNK
    expected_crc = 0
    for _ in range(blocks):
        expected_crc = zlib.crc32(pat, expected_crc)
    expected_crc &= 0xFFFFFFFF

    print(f"=== RX bench (host -> phone): {mb} MiB ({total} bytes) to {host}:{port} ===")
    t0 = time.monotonic()
    with socket.create_connection((host, port), timeout=10.0) as s:
        s.settimeout(timeout)
        for _ in range(blocks):
            s.sendall(pat)
        s.shutdown(socket.SHUT_WR)
        reply = b""
        while True:
            d = s.recv(4096)
            if not d:
                break
            reply += d
    dt = max(time.monotonic() - t0, 1e-6)
    txt = reply.decode("utf-8", errors="replace").strip()
    print(f"Phone reply : {txt}")
    print(f"Host wall   : {total / (1048576.0 * dt):.2f} MB/s ({dt:.3f} s), expected crc32=0x{expected_crc:08x}")
    ok = f"bytes={total}" in txt and f"crc32=0x{expected_crc:08x}" in txt
    print(f"RX VERIFY   : {'PASS (BIT-EXACT CRC32 MATCH)' if ok else 'FAIL!'}")
    return ok


def run_txbench(host: str, port: int, mb: int, timeout: float = 60.0) -> bool:
    pat = make_pattern_chunk()
    blocks = mb * 16
    total = blocks * CHUNK
    expected_crc = 0
    for _ in range(blocks):
        expected_crc = zlib.crc32(pat, expected_crc)
    expected_crc &= 0xFFFFFFFF

    print(f"=== TX bench (phone -> host): {mb} MiB ({total} bytes) from {host}:{port} ===")
    t0 = time.monotonic()
    got = 0
    crc = 0
    with socket.create_connection((host, port), timeout=10.0) as s:
        s.settimeout(timeout)
        s.sendall(f"{mb}\n".encode("ascii"))
        while True:
            d = s.recv(CHUNK)
            if not d:
                break
            got += len(d)
            crc = zlib.crc32(d, crc)
    dt = max(time.monotonic() - t0, 1e-6)
    crc &= 0xFFFFFFFF
    ok = (got == total and crc == expected_crc)
    print(f"Host recv   : {got} / {total} bytes in {dt:.3f} s ({got / (1048576.0 * dt):.2f} MB/s)")
    print(f"CRC32       : got=0x{crc:08x} expected=0x{expected_crc:08x}")
    print(f"TX VERIFY   : {'PASS (BIT-EXACT CRC32 MATCH)' if ok else 'FAIL!'}")
    return ok



def run_nbdtest(host: str, port: int, rw_unit: bool = True) -> bool:
    """Full end-to-end verification of HNBD v1 + APFS mount + decmpfs + 8 MiB CRC32 + Mach-O execve + RW."""
    expected_comp_crc = "0x7e8b97b5"
    expected_big_crc = "0x344c2cb9"

    def step(cmd: str) -> str:
        out = run_cmd(host, port, cmd, timeout=30.0)
        print(f"=== {cmd} ===\n{out.rstrip()}\n", flush=True)
        return out

    st = ""
    for _ in range(16):
        st = run_cmd(host, port, "nbd", timeout=10.0)
        if "nbd0: connected" in st:
            break
        time.sleep(0.5)
    if "nbd0: connected" not in st:
        st = step("nbd connect 0")
    else:
        print(f"=== nbd ===\n{st.rstrip()}\n", flush=True)
    if "nbd0: connected" not in st:
        print("FAIL: nbd0 not connected")
        return False

    ok = True
    out = step("mount /dev/nbd0 /mnt")
    ok &= ("MOUNT OK" in out)

    out = step("ls /mnt")
    ok &= ("etc" in out and "data" in out and "bin" in out)

    out = step("cat /mnt/etc/hello.txt")
    ok &= ("HakiOS v0.56 Network APFS Volume" in out)

    out = step("crc /mnt/data/compressed.txt")
    ok &= ("CRC OK" in out and expected_comp_crc in out)

    out = step("crc /mnt/data/test8m.bin")
    ok &= ("CRC OK" in out and expected_big_crc in out)

    out = step("run /mnt/bin/apfs_hello")
    ok &= ("APFS_EXEC_OK" in out and "code=42" in out)

    out = step("umount /mnt")
    ok &= ("UMOUNT OK" in out)

    if rw_unit:
        out = step("nbd connect 1")
        if "nbd1: connected" in out and " rw" in out:
            out = step("rawwrite /dev/rnbd1 7 12345")
            ok &= ("RAWWRITE OK" in out)
            step("nbd disconnect 1")
        else:
            print("NOTE: unit1 RW not served, skipping rawwrite")

    step("nbd")
    print(f"NBD+APFS E2E VERIFY: {'PASS (ALL CHECKS BIT-EXACT)' if ok else 'FAIL!'}")
    return ok


def main() -> int:
    ap = argparse.ArgumentParser(description="HakiOS ruby TCP console & USB DMA benchmark client")
    ap.add_argument("--host", default="192.168.7.2", help="Target IP (default 192.168.7.2)")
    ap.add_argument("--port", type=int, default=2323, help="Console port (default 2323)")
    sub = ap.add_subparsers(dest="mode", required=True)

    sub.add_parser("shell", help="Interactive TCP console")
    p_cmd = sub.add_parser("cmd", help="Run a single command and print output")
    p_cmd.add_argument("command", nargs="+", help="Command text")
    p_b = sub.add_parser("bench", help="Run bit-exact RX + TX DMA benchmarks")
    p_b.add_argument("--mb", type=int, default=16, help="Transfer size in MiB (default 16)")
    p_rx = sub.add_parser("rxbench", help="Run RX (host -> phone) benchmark only")
    p_rx.add_argument("--mb", type=int, default=16)
    p_tx = sub.add_parser("txbench", help="Run TX (phone -> host) benchmark only")
    p_tx.add_argument("--mb", type=int, default=16)
    p_nbd = sub.add_parser("nbdtest", help="Run full NBD + APFS + CRC32 + Mach-O execve + RW test")
    p_nbd.add_argument("--no-rw", action="store_true", help="Skip unit1 RW sector test")

    a = ap.parse_args()
    if a.mode == "shell":
        run_shell(a.host, a.port)
        return 0
    if a.mode == "cmd":
        print(run_cmd(a.host, a.port, " ".join(a.command)), end="")
        return 0
    if a.mode == "rxbench":
        return 0 if run_rxbench(a.host, 2325, a.mb) else 1
    if a.mode == "txbench":
        return 0 if run_txbench(a.host, 2324, a.mb) else 1
    if a.mode == "nbdtest":
        return 0 if run_nbdtest(a.host, a.port, rw_unit=not a.no_rw) else 1
    if a.mode == "bench":
        ok1 = run_rxbench(a.host, 2325, a.mb)
        ok2 = run_txbench(a.host, 2324, a.mb)
        print("=== Phone USB counters after bench ===")
        print(run_cmd(a.host, a.port, "usb"), end="")
        return 0 if (ok1 and ok2) else 1
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
