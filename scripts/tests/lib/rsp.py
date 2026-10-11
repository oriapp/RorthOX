"""Small GDB remote serial protocol client for QEMU's gdbstub, used by the tests in scripts/tests/.

QEMU is started with "-gdb unix:<socket>,server=on,wait=off" (and -S to stop it before the BIOS).
A test connects, sets execution breakpoints, reads and writes registers and memory, runs monitor
commands, then detaches so the guest runs on. Every operation has a timeout, and anything
unexpected (an error reply, an unsupported packet, a stop that isn't a breakpoint, QEMU going away)
raises RspError, so a test can't pass by accident.

Only the Python standard library is used.
"""

import re
import socket
import struct
import time

# The first 16 registers of QEMU's i386 target description, in "p"/"P" numbering.
REGS = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi",
        "eip", "eflags", "cs", "ss", "ds", "es", "fs", "gs"]

EFLAGS_DF = 0x400
EFLAGS_IF = 0x200


class RspError(Exception):
    pass


class Rsp:
    def __init__(self, path, timeout=30.0):
        deadline = time.monotonic() + timeout
        while True:
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                sock.connect(path)
                break
            except OSError:
                sock.close()
                if time.monotonic() > deadline:
                    raise RspError("could not connect to the gdbstub at %s" % path)
                time.sleep(0.1)
        self.sock = sock
        self.buf = b""
        self.op_timeout = 20.0
        self.breakpoints = set()
        # QEMU stops the guest when a debugger connects; "?" returns the stop reason.
        self.stop_reply = self.request("?")

    # --- packets ---------------------------------------------------------------------------

    def _read_byte(self, deadline):
        while not self.buf:
            left = deadline - time.monotonic()
            if left <= 0:
                raise TimeoutError
            self.sock.settimeout(left)
            try:
                data = self.sock.recv(65536)
            except socket.timeout:
                raise TimeoutError
            if not data:
                raise RspError("the gdbstub closed the connection (QEMU exited?)")
            self.buf += data
        b = self.buf[0]
        self.buf = self.buf[1:]
        return b

    def _send_raw(self, data):
        self.sock.sendall(data)

    def _send_packet(self, payload):
        body = payload.encode("latin-1")
        frame = b"$" + body + b"#" + ("%02x" % (sum(body) & 0xFF)).encode()
        deadline = time.monotonic() + self.op_timeout
        for _ in range(5):
            self._send_raw(frame)
            while True:
                try:
                    b = self._read_byte(deadline)
                except TimeoutError:
                    raise RspError("no acknowledgement for packet %r" % payload[:40])
                if b == ord("+"):
                    return
                if b == ord("-"):
                    break   # resend
                # Anything else before the ack is noise; keep reading.
        raise RspError("packet %r was refused five times" % payload[:40])

    def _read_packet(self, timeout):
        deadline = time.monotonic() + timeout
        while self._read_byte(deadline) != ord("$"):
            pass
        body = bytearray()
        while True:
            b = self._read_byte(deadline)
            if b == ord("#"):
                break
            body.append(b)
        csum = bytes([self._read_byte(deadline), self._read_byte(deadline)])
        if int(csum, 16) != sum(body) & 0xFF:
            self._send_raw(b"-")
            raise RspError("bad checksum from the gdbstub")
        self._send_raw(b"+")
        return self._decode(bytes(body))

    @staticmethod
    def _decode(body):
        out = bytearray()
        i = 0
        while i < len(body):
            c = body[i]
            if c == ord("}"):          # escaped byte
                out.append(body[i + 1] ^ 0x20)
                i += 2
            elif c == ord("*"):        # run-length encoding: repeat the last byte
                out.extend(out[-1:] * (body[i + 1] - 29))
                i += 2
            else:
                out.append(c)
                i += 1
        return out.decode("latin-1")

    def request(self, payload, timeout=None):
        self._send_packet(payload)
        return self._read_packet(self.op_timeout if timeout is None else timeout)

    def expect_ok(self, payload):
        reply = self.request(payload)
        if reply != "OK":
            raise RspError("%r: expected OK, got %r" % (payload[:40], reply))

    # --- registers and memory --------------------------------------------------------------

    def reg(self, name):
        reply = self.request("p%x" % REGS.index(name))
        if len(reply) < 8 or reply.startswith("E"):
            raise RspError("reading %s failed: %r" % (name, reply))
        return struct.unpack("<I", bytes.fromhex(reply[:8]))[0]

    def set_reg(self, name, value):
        self.expect_ok("P%x=%s" % (REGS.index(name), struct.pack("<I", value & 0xFFFFFFFF).hex()))

    def regs(self):
        return {name: self.reg(name) for name in REGS}

    def set_regs(self, values):
        for name in REGS:
            if name in values:
                self.set_reg(name, values[name])

    def read(self, addr, length):
        data = bytearray()
        while length > 0:
            n = min(length, 2048)
            reply = self.request("m%x,%x" % (addr, n))
            if reply.startswith("E") or len(reply) != 2 * n:
                raise RspError("reading %d bytes at %#x failed: %r" % (n, addr, reply[:20]))
            data += bytes.fromhex(reply)
            addr += n
            length -= n
        return bytes(data)

    def write(self, addr, data):
        while data:
            chunk = data[:1024]
            self.expect_ok("M%x,%x:%s" % (addr, len(chunk), chunk.hex()))
            addr += len(chunk)
            data = data[1024:]

    def read32(self, addr):
        return struct.unpack("<I", self.read(addr, 4))[0]

    def write32(self, addr, value):
        self.write(addr, struct.pack("<I", value & 0xFFFFFFFF))

    # --- execution -------------------------------------------------------------------------

    def add_breakpoint(self, addr):
        reply = self.request("Z1,%x,1" % addr)
        if reply != "OK":
            raise RspError("hardware breakpoint at %#x refused: %r" % (addr, reply))
        self.breakpoints.add(addr)

    def remove_breakpoint(self, addr):
        self.expect_ok("z1,%x,1" % addr)
        self.breakpoints.discard(addr)

    def _check_stop(self, reply):
        # "T05..." / "S05" is SIGTRAP: a breakpoint or a finished single step.
        if not re.match(r"^[ST]05", reply):
            raise RspError("unexpected stop reply %r" % reply)
        return reply

    def step(self):
        return self._check_stop(self.request("s"))

    def cont(self, timeout):
        """Continue until a breakpoint hits; returns the new eip."""
        eip = self.reg("eip")
        if eip in self.breakpoints:
            # QEMU would stop again on the same instruction: step past it first.
            self.remove_breakpoint(eip)
            self.step()
            self.add_breakpoint(eip)
        self._send_packet("c")
        try:
            self._check_stop(self._read_packet(timeout))
        except TimeoutError:
            self._send_raw(b"\x03")
            try:
                self._read_packet(5)
            except (TimeoutError, RspError):
                pass
            raise RspError("no breakpoint hit within %d s" % timeout)
        return self.reg("eip")

    def monitor(self, command):
        """Run a QEMU monitor command and return its output."""
        self._send_packet("qRcmd," + command.encode().hex())
        out = []
        while True:
            reply = self._read_packet(self.op_timeout)
            if reply == "OK":
                return "".join(out)
            if reply.startswith("O") and reply != "OK":
                out.append(bytes.fromhex(reply[1:]).decode("latin-1"))
            elif reply == "":
                raise RspError("monitor commands are not supported by this gdbstub")
            elif re.match(r"^E[0-9a-fA-F]{2}$", reply):
                raise RspError("monitor command %r failed: %s" % (command, reply))
            else:
                # Some QEMU versions send the output as a single hex payload.
                try:
                    out.append(bytes.fromhex(reply).decode("latin-1"))
                except ValueError:
                    raise RspError("unexpected monitor reply %r" % reply[:40])

    def control_registers(self):
        """CR0, CR2, CR3 and the IDT base/limit from the monitor's "info registers"."""
        text = self.monitor("info registers")
        out = {}
        for name in ("CR0", "CR2", "CR3", "CR4"):
            m = re.search(r"\b%s=([0-9a-fA-F]+)" % name, text)
            if not m:
                raise RspError("no %s in info registers" % name)
            out[name.lower()] = int(m.group(1), 16)
        m = re.search(r"^IDT=\s*([0-9a-fA-F]+)\s+([0-9a-fA-F]+)", text, re.M)
        if not m:
            raise RspError("no IDT line in info registers")
        out["idt_base"] = int(m.group(1), 16)
        out["idt_limit"] = int(m.group(2), 16)
        return out

    def detach(self):
        """Remove the breakpoints and let the guest run on without a debugger."""
        for addr in list(self.breakpoints):
            self.remove_breakpoint(addr)
        self.expect_ok("D")
        self.sock.close()


# --- build outputs ----------------------------------------------------------------------------

def read_map(path):
    """Symbols and output sections from a GNU ld map file: ({name: addr}, {section: (addr, size)})."""
    symbols = {}
    sections = {}
    with open(path) as f:
        for line in f:
            m = re.match(r"^(\.\w+)\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)\s*$", line)
            if m:
                sections.setdefault(m.group(1), (int(m.group(2), 16), int(m.group(3), 16)))
                continue
            # "                0x00100000                _start" and "0x... __bss_start = ."
            m = re.match(r"^\s+0x([0-9a-fA-F]+)\s+([A-Za-z_]\w*)(\s*=.*)?\s*$", line)
            if m:
                symbols.setdefault(m.group(2), int(m.group(1), 16))
    return symbols, sections


def read_elf_symbols(path):
    """{name: value} from the symbol table of a 32-bit little-endian ELF file."""
    with open(path, "rb") as f:
        elf = f.read()
    if elf[:6] != b"\x7fELF\x01\x01":
        raise RspError("%s is not a 32-bit little-endian ELF file" % path)
    shoff, = struct.unpack_from("<I", elf, 0x20)
    shentsize, shnum = struct.unpack_from("<HH", elf, 0x2E)
    headers = [struct.unpack_from("<10I", elf, shoff + i * shentsize) for i in range(shnum)]
    symbols = {}
    for sh in headers:
        if sh[1] != 2:          # SHT_SYMTAB
            continue
        strtab = headers[sh[6]]
        for off in range(sh[4], sh[4] + sh[5], 16):
            name, value = struct.unpack_from("<II", elf, off)
            start = strtab[4] + name
            symbols[elf[start:elf.index(b"\0", start)].decode("latin-1")] = value
    return symbols


def read_config(*paths):
    """#define / %define values from build/config/rorthox-config.{h,inc}, numbers only."""
    values = {}
    for path in paths:
        with open(path) as f:
            for line in f:
                m = re.match(r"^[#%]define\s+(\w+)\s+(0x[0-9a-fA-F]+|\d+)\s*$", line)
                if m:
                    values[m.group(1)] = int(m.group(2), 0)
    return values


class Results:
    """One "PASS <id>" or "FAIL <id>: <detail>" line per assertion; the bash test checks the ids."""

    def __init__(self, path):
        self.path = path
        self.lines = []
        open(path, "w").close()

    def add(self, ident, ok, detail=""):
        line = ("PASS %s" % ident) if ok else ("FAIL %s: %s" % (ident, detail))
        self.lines.append(line)
        with open(self.path, "a") as f:
            f.write(line + "\n")
        print("  %s %s%s" % ("ok  " if ok else "FAIL", ident, "" if ok else ": " + detail), flush=True)
        return ok
