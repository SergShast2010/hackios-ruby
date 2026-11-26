#!/usr/bin/env python3
"""
HakiOS Network Block Device server (HNBD v1) for Redmi Note 12 Pro 5G (ruby) & QEMU.

Serves host disk images (APFS containers, raw volumes) over TCP (:10809) to the
XNU kernel driver `drivers/storage/hakios.nbd.c` (/dev/nbd0..1, /dev/rnbd0..1),
keeping the phone's UFS flash 100% read-only.

Examples:
  # Create deterministic test APFS image (with /etc/hello.txt, /data/test8m.bin, /bin/apfs_hello)
  # and a 1 MiB RW scratch disk:
  python3 tools/ruby_nbd.py --make-test-apfs out/v56/test_v56.apfs --make-test-rw out/v56/test_rw.img

  # Serve unit 0 (read-only APFS) and unit 1 (read-write scratch disk):
  python3 tools/ruby_nbd.py --unit0 out/v56/test_v56.apfs --unit1 out/v56/test_rw.img:rw
"""

import argparse
import errno
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zlib
from pathlib import Path

HNBD_HELLO_MAGIC = 0x44424E48  # "HNBD"
HNBD_REQ_MAGIC   = 0x31514552  # "REQ1"
HNBD_RSP_MAGIC   = 0x314C5052  # "RPL1"
HNBD_VERSION     = 1

HNBD_FLAG_RO     = 1 << 0

HNBD_OP_READ     = 1
HNBD_OP_WRITE    = 2
HNBD_OP_FLUSH    = 3

SECTOR_SIZE      = 512
MAX_XFER         = 256 * 1024

HELLO_REQ_FMT    = "<IHHII"     # 16 bytes
HELLO_RSP_FMT    = "<IIIIQ"     # 24 bytes
REQ_FMT          = "<IIQII"     # 24 bytes
RSP_FMT          = "<IIII"      # 16 bytes

MKAPFS_PY        = Path("/home/sergsh/hackios/hakios-xnu-kernel/tools/apfs/mkapfs.py")
XNU_ROOT         = Path("/home/sergsh/hackios/hakios-xnu-kernel")

APFS_HELLO_C = r'''
/* Minimal freestanding arm64 Mach-O test binary executed from network-mounted APFS (/mnt/bin/apfs_hello) */
typedef unsigned long u64;

static long sc3(long n, long a, long b, long c) {
    register long x16 __asm__("x16") = n;
    register long x0  __asm__("x0")  = a;
    register long x1  __asm__("x1")  = b;
    register long x2  __asm__("x2")  = c;
    __asm__ volatile("svc #0x80" : "+r"(x0) : "r"(x16), "r"(x1), "r"(x2) : "memory", "cc");
    return x0;
}

static void put(const char *s) {
    u64 n = 0; while (s[n]) n++;
    sc3(4, 1, (long)s, (long)n);
}

static void put_u64(u64 v) {
    char b[24]; int i = 0;
    if (!v) b[i++] = '0';
    else while (v) { b[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i--) sc3(4, 1, (long)&b[i], 1);
}

void start_main(void) __attribute__((noreturn, noinline));
__asm__(
    ".text\n"
    ".globl _start\n"
    ".p2align 2\n"
    "_start:\n"
    "\tmov x0, sp\n"
    "\tand x0, x0, #-16\n"
    "\tmov sp, x0\n"
    "\tb _start_main\n"
);
void start_main(void) {
    long pid = sc3(20, 0, 0, 0);
    long ppid = sc3(39, 0, 0, 0);
    put("APFS_EXEC_OK: hello from Mach-O binary on network APFS! pid=");
    put_u64((u64)pid);
    put(" ppid=");
    put_u64((u64)ppid);
    put("\n");
    sc3(1, 42, 0, 0);
    for (;;) {}
}
'''


def deterministic_test_bytes(nbytes: int) -> bytes:
    """Fast deterministic non-trivial pattern matching tools/ruby_net.py style."""
    # 64 KiB repeating blocks mixed with block index so every 4K page is unique
    # and incompressible enough to exercise multi-megabyte APFS reads.
    out = bytearray(nbytes)
    pos = 0
    blk = 0
    while pos < nbytes:
        chunk = min(65536, nbytes - pos)
        # xorshift32 per 4 bytes in pure python using 4 KiB seed + block xor
        seed = (0xA55A1234 ^ (blk * 0x9E3779B1)) & 0xFFFFFFFF
        buf = bytearray(chunk)
        # Build 256-byte tile and tile-xor with page offset
        s = seed
        tile = bytearray(256)
        for i in range(0, 256, 4):
            s ^= (s << 13) & 0xFFFFFFFF
            s ^= (s >> 17)
            s ^= (s << 5) & 0xFFFFFFFF
            struct.pack_into("<I", tile, i, s)
        for off in range(0, chunk, 256):
            take = min(256, chunk - off)
            xor_b = ((blk * 17) ^ (off >> 8)) & 0xFF
            if xor_b == 0:
                buf[off:off + take] = tile[:take]
            else:
                buf[off:off + take] = bytes(b ^ xor_b for b in tile[:take])
        out[pos:pos + chunk] = buf
        pos += chunk
        blk += 1
    return bytes(out)


def make_test_apfs(out_path: Path, size_mb: int = 8) -> dict:
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="ruby-v56-apfs-") as td:
        root = Path(td) / "root"
        (root / "etc").mkdir(parents=True)
        (root / "data").mkdir(parents=True)
        (root / "bin").mkdir(parents=True)

        hello_txt = (
            "HakiOS v0.56 Network APFS Volume on Redmi Note 12 Pro 5G (ruby)\n"
            "Served over USB CDC-ECM (~38 MB/s) via HNBD v1 with ZERO UFS writes.\n"
        ).encode("utf-8")
        (root / "etc" / "hello.txt").write_bytes(hello_txt)

        # Compressible text file (exercises APFS decmpfs zlib decompression)
        comp_txt = (b"RUBY-MT6877-APFS-DECMPFS-CHECK-LINE-0123456789ABCDEF\n" * 4096)
        (root / "data" / "compressed.txt").write_bytes(comp_txt)
        comp_crc = zlib.crc32(comp_txt) & 0xFFFFFFFF

        # Multi-megabyte pseudo-random binary payload (exercises sustained NBD + APFS reads)
        big_bytes = deterministic_test_bytes(size_mb * 1024 * 1024)
        (root / "data" / f"test{size_mb}m.bin").write_bytes(big_bytes)
        big_crc = zlib.crc32(big_bytes) & 0xFFFFFFFF

        # Compile minimal freestanding arm64 Mach-O binary (/bin/apfs_hello)
        src_c = Path(td) / "apfs_hello.c"
        bin_out = root / "bin" / "apfs_hello"
        src_c.write_text(APFS_HELLO_C)
        compile_cmd = (
            f"source {XNU_ROOT}/xnu-env.sh && "
            f"xnu-clang -arch arm64 -Os -static -nostdlib -ffreestanding "
            f"-fno-stack-protector -fno-builtin -Wl,-e,_start -Wl,-pagezero_size,0x100000000 "
            f"-o {bin_out} {src_c}"
        )
        subprocess.run(["bash", "-c", compile_cmd], check=True)
        os.chmod(bin_out, 0o755)

        # Build deterministic APFS container (-z enables decmpfs; --plain bin keeps Mach-O uncompressed)
        subprocess.run(
            [
                sys.executable, str(MKAPFS_PY),
                "-o", str(out_path),
                "-z", "--plain", "bin",
                "--time", "1683900000",
                "--slack", "16",
                f"RubyTest={root}",
            ],
            check=True,
        )

    apfs_size = out_path.stat().st_size
    info = {
        "path": str(out_path),
        "apfs_size": apfs_size,
        "hello_crc": zlib.crc32(hello_txt) & 0xFFFFFFFF,
        "compressed_size": len(comp_txt),
        "compressed_crc": comp_crc,
        "big_name": f"/mnt/data/test{size_mb}m.bin",
        "big_size": len(big_bytes),
        "big_crc": big_crc,
    }
    print(
        f"CREATED TEST APFS: {out_path} ({apfs_size} B, {apfs_size / (1024*1024):.2f} MiB)\n"
        f"  /mnt/etc/hello.txt       crc32=0x{info['hello_crc']:08x}\n"
        f"  /mnt/data/compressed.txt size={info['compressed_size']} crc32=0x{info['compressed_crc']:08x}\n"
        f"  {info['big_name']:24s} size={info['big_size']} crc32=0x{info['big_crc']:08x}\n"
        f"  /mnt/bin/apfs_hello      arm64 Mach-O (exits 42)"
    )
    return info


def make_test_rw(out_path: Path, size_mb: int = 1) -> None:
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with open(out_path, "wb") as f:
        f.truncate(size_mb * 1024 * 1024)
    print(f"CREATED RW SCRATCH IMG: {out_path} ({size_mb} MiB)")


class UnitExport:
    """Single-file or virtual composite (concatenated RO segments) block export."""

    def __init__(self, unit_id: int, paths: list[Path], writable: bool):
        if not paths:
            raise ValueError(f"unit{unit_id}: empty path list")
        if writable and len(paths) > 1:
            raise ValueError(f"unit{unit_id}: composite export must be read-only")
        self.unit_id = unit_id
        self.paths = paths
        self.path = paths[0]
        self.label = "+".join(pt.name for pt in paths)
        self.writable = writable
        flags = os.O_RDWR if writable else os.O_RDONLY
        self.segments: list[tuple[int, int, int, Path]] = []  # (start, end, fd, path)
        off = 0
        for pt in paths:
            fd = os.open(str(pt), flags)
            sz = os.fstat(fd).st_size
            if sz < SECTOR_SIZE or (sz % SECTOR_SIZE) != 0:
                os.close(fd)
                self.close()
                raise ValueError(f"unit{unit_id} ({pt}): size {sz} must be a non-zero multiple of 512")
            self.segments.append((off, off + sz, fd, pt))
            off += sz
        self.size = off
        self.fd = self.segments[0][2]
        self.lock = threading.Lock()
        self.reads = 0
        self.writes = 0
        self.flushes = 0
        self.rbytes = 0
        self.wbytes = 0

    def pread(self, length: int, offset: int) -> bytes:
        if len(self.segments) == 1:
            return os.pread(self.fd, length, offset)
        out = bytearray()
        pos = offset
        rem = length
        for start, end, fd, _pt in self.segments:
            if pos >= end:
                continue
            if pos < start:
                break
            take = min(rem, end - pos)
            chunk = os.pread(fd, take, pos - start)
            if len(chunk) != take:
                return b""
            out.extend(chunk)
            pos += take
            rem -= take
            if rem == 0:
                break
        return bytes(out)

    def pwrite(self, payload: bytes, offset: int) -> int:
        return os.pwrite(self.fd, payload, offset)

    def fsync(self) -> None:
        if self.writable:
            os.fsync(self.fd)

    def close(self):
        for _start, _end, fd, _pt in getattr(self, "segments", []):
            try:
                os.close(fd)
            except OSError:
                pass


def recv_exact(sock: socket.socket, n: int) -> bytes:
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise EOFError("peer closed")
        buf.extend(chunk)
    return bytes(buf)


def handle_client(conn: socket.socket, addr, units: dict[int, UnitExport], quiet: bool):
    conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    try:
        raw = recv_exact(conn, struct.calcsize(HELLO_REQ_FMT))
        magic, version, unit_id, _flags, _res = struct.unpack(HELLO_REQ_FMT, raw)
        if magic != HNBD_HELLO_MAGIC or version != HNBD_VERSION or unit_id not in units:
            status = errno.ENOENT if unit_id not in units else errno.EINVAL
            conn.sendall(struct.pack(HELLO_RSP_FMT, HNBD_HELLO_MAGIC, status, 0, SECTOR_SIZE, 0))
            if not quiet:
                print(f"[nbd] rejected {addr}: magic=0x{magic:08x} ver={version} unit={unit_id}", flush=True)
            return

        u = units[unit_id]
        uflags = 0 if u.writable else HNBD_FLAG_RO
        conn.sendall(struct.pack(HELLO_RSP_FMT, HNBD_HELLO_MAGIC, 0, uflags, SECTOR_SIZE, u.size))
        if not quiet:
            print(
                f"[nbd] unit{unit_id} connected from {addr[0]}:{addr[1]} -> "
                f"{u.label} ({u.size // (1024*1024)} MiB, {'rw' if u.writable else 'ro'})",
                flush=True,
            )

        req_sz = struct.calcsize(REQ_FMT)
        while True:
            hdr = recv_exact(conn, req_sz)
            rmagic, op, offset, length, tag = struct.unpack(REQ_FMT, hdr)
            if rmagic != HNBD_REQ_MAGIC:
                raise ValueError(f"bad req magic 0x{rmagic:08x}")

            if op == HNBD_OP_READ:
                if length > MAX_XFER or (offset % SECTOR_SIZE) or (length % SECTOR_SIZE) or (offset + length > u.size):
                    conn.sendall(struct.pack(RSP_FMT, HNBD_RSP_MAGIC, errno.EINVAL, tag, 0))
                    continue
                data = u.pread(length, offset)
                if len(data) != length:
                    conn.sendall(struct.pack(RSP_FMT, HNBD_RSP_MAGIC, errno.EIO, tag, 0))
                    continue
                with u.lock:
                    u.reads += 1
                    u.rbytes += length
                conn.sendall(struct.pack(RSP_FMT, HNBD_RSP_MAGIC, 0, tag, length) + data)

            elif op == HNBD_OP_WRITE:
                payload = recv_exact(conn, length) if length > 0 else b""
                if not u.writable:
                    conn.sendall(struct.pack(RSP_FMT, HNBD_RSP_MAGIC, errno.EROFS, tag, 0))
                    continue
                if length > MAX_XFER or (offset % SECTOR_SIZE) or (length % SECTOR_SIZE) or (offset + length > u.size):
                    conn.sendall(struct.pack(RSP_FMT, HNBD_RSP_MAGIC, errno.EINVAL, tag, 0))
                    continue
                written = u.pwrite(payload, offset)
                if written != length:
                    conn.sendall(struct.pack(RSP_FMT, HNBD_RSP_MAGIC, errno.EIO, tag, 0))
                    continue
                with u.lock:
                    u.writes += 1
                    u.wbytes += length
                conn.sendall(struct.pack(RSP_FMT, HNBD_RSP_MAGIC, 0, tag, 0))

            elif op == HNBD_OP_FLUSH:
                u.fsync()
                with u.lock:
                    u.flushes += 1
                conn.sendall(struct.pack(RSP_FMT, HNBD_RSP_MAGIC, 0, tag, 0))

            else:
                conn.sendall(struct.pack(RSP_FMT, HNBD_RSP_MAGIC, errno.EINVAL, tag, 0))

    except (EOFError, ConnectionResetError, BrokenPipeError):
        if not quiet:
            print(f"[nbd] disconnected {addr[0]}:{addr[1]}", flush=True)
    except Exception as exc:
        if not quiet:
            print(f"[nbd] error {addr[0]}:{addr[1]}: {exc}", flush=True)
    finally:
        try:
            conn.close()
        except OSError:
            pass


def parse_unit_spec(spec: str) -> tuple[list[Path], bool]:
    writable = False
    if spec.endswith(":rw"):
        spec, writable = spec[:-3], True
    elif spec.endswith(":ro"):
        spec, writable = spec[:-3], False
    paths = [Path(part) for part in spec.split("+") if part]
    return paths, writable


def main():
    ap = argparse.ArgumentParser(description="HakiOS HNBD v1 server for ruby & QEMU")
    ap.add_argument("--bind", default="0.0.0.0", help="Listen IP (default 0.0.0.0)")
    ap.add_argument("--port", type=int, default=10809, help="Listen TCP port (default 10809)")
    ap.add_argument("--unit0", help="Image(s) for /dev/nbd0 (join with '+' for composite RO, append :rw for RW)")
    ap.add_argument("--unit1", help="Image(s) for /dev/nbd1")
    ap.add_argument("--unit2", help="Image(s) for /dev/nbd2")
    ap.add_argument("--unit3", help="Image(s) for /dev/nbd3")
    ap.add_argument("--darwin", action="store_true",
                    help="Serve HakiOS Darwin 16.5 preset: unit0=rootfs+cryptex-os+cryptex-app, unit1=root.apfs")
    ap.add_argument("--var-hfs", type=Path, help="Optional persistent HFS+ var image for /dev/nbd2 (:rw)")
    ap.add_argument("--make-test-apfs", type=Path, help="Generate deterministic test APFS image at this path")
    ap.add_argument("--test-mb", type=int, default=8, help="Payload MiB inside test APFS (default 8)")
    ap.add_argument("--make-test-rw", type=Path, help="Generate 1 MiB RW scratch disk at this path")
    ap.add_argument("--quiet", action="store_true", help="Suppress per-connection log lines")
    args = ap.parse_args()

    if args.make_test_apfs:
        make_test_apfs(args.make_test_apfs, size_mb=args.test_mb)
    if args.make_test_rw:
        make_test_rw(args.make_test_rw, size_mb=1)

    if args.darwin:
        sysdir = XNU_ROOT / "sysimage"
        if not args.unit0:
            args.unit0 = f"{sysdir / 'rootfs.apfs'}+{sysdir / 'cryptex-os.dmg'}+{sysdir / 'cryptex-app.dmg'}:ro"
        if not args.unit1:
            args.unit1 = f"{XNU_ROOT / 'userland' / 'root.apfs'}:ro"
    if args.var_hfs and not args.unit2:
        args.unit2 = f"{args.var_hfs}:rw"

    raw_units = {0: args.unit0, 1: args.unit1, 2: args.unit2, 3: args.unit3}
    if not any(raw_units.values()):
        if args.make_test_apfs or args.make_test_rw:
            return
        ap.error("Specify --darwin or --unit0..3, or --make-test-apfs")

    units: dict[int, UnitExport] = {}
    for uid, spec in raw_units.items():
        if spec:
            paths, rw = parse_unit_spec(spec)
            units[uid] = UnitExport(uid, paths, rw)

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.bind, args.port))
    srv.listen(16)

    print(f"[nbd] HNBD v1 listening on {args.bind}:{args.port}", flush=True)
    for uid, u in sorted(units.items()):
        print(
            f"  unit{uid} (/dev/nbd{uid}): {u.label} "
            f"({u.size} B, {u.size / (1024*1024):.2f} MiB, {'RW' if u.writable else 'RO'})",
            flush=True,
        )

    try:
        while True:
            conn, addr = srv.accept()
            t = threading.Thread(target=handle_client, args=(conn, addr, units, args.quiet), daemon=True)
            t.start()
    except KeyboardInterrupt:
        print("\n[nbd] shutting down; totals:")
        for uid, u in sorted(units.items()):
            print(
                f"  unit{uid}: reads={u.reads} ({u.rbytes} B), "
                f"writes={u.writes} ({u.wbytes} B), flushes={u.flushes}"
            )
    finally:
        srv.close()
        for u in units.values():
            u.close()


if __name__ == "__main__":
    main()
