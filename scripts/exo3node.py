#!/usr/bin/env python3
"""exo three-node ring demo (v0.7.0 "Maxwell").

Topology
--------
QEMU 7.2 (Debian build) has NO mcast netdev and tcpip is single-interface,
so a true shared L2 segment is provided by a userspace hub on the host:
each node's virtio-net NIC is attached to a `-netdev dgram` UDP endpoint
(1 UDP datagram = 1 raw Ethernet frame, frame boundaries preserved — unlike
TCP socket pairs), and hub3.py below floods every frame to the other two
ports. UDP broadcast (discovery beacons) and unicast TCP (ring TENSOR/RESULT)
both cross the hub. All nodes are on one subnet 10.0.0.0/29.

  Node A: mac 52:54:00:00:00:0A, ip 10.0.0.1, hub port 12001 -> local 12101
  Node B: mac 52:54:00:00:00:0B, ip 10.0.0.2, hub port 12002 -> local 12102
  Node C: mac 52:54:00:00:00:0C, ip 10.0.0.3, hub port 12003 -> local 12103

Flow: setip -> exo init -> live discovery (no manual exopeer!) -> warm-up
model load -> `exoring auto` (identical ring on all nodes) -> `exoshard auto`
(RAM-weighted, equal RAM -> 10/10/10 for smollm-30) -> `exochat 8 What is
the capital of France?` on the orchestrator -> expect "Paris", zero
transport errors.

Usage: python3 exo3node.py [elf]
Logs:  /mnt/agents/work/logs/exo3node_{A,B,C,hub}.log
"""
import os, re, select, socket, subprocess, sys, threading, time

HOME = os.environ["HOME"]
SYSROOT = os.path.join(HOME, "sysroot")
os.environ["LD_LIBRARY_PATH"] = (f"{SYSROOT}/usr/lib/x86_64-linux-gnu:"
                                 f"{SYSROOT}/lib/x86_64-linux-gnu:" +
                                 os.environ.get("LD_LIBRARY_PATH", ""))
os.environ["QEMU_MODULE_DIR"] = f"{SYSROOT}/usr/lib/x86_64-linux-gnu/qemu"
QEMU = os.path.join(SYSROOT, "usr/bin/qemu-system-x86_64")
ELF = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("EMBODIOS_ELF", "kernel/embodios.elf")
LOGD = os.environ.get("EXO_LOGDIR", "logs")
os.makedirs(LOGD, exist_ok=True)

NODES = {
    "A": ("52:54:00:00:00:0A", "10.0.0.1", 12001, 12101),
    "B": ("52:54:00:00:00:0B", "10.0.0.2", 12002, 12102),
    "C": ("52:54:00:00:00:0C", "10.0.0.3", 12003, 12103),
}

# --------------------------------------------------------------------------
# Userspace L2 hub: flood every datagram (Ethernet frame) to the other ports
# --------------------------------------------------------------------------
class Hub:
    def __init__(self):
        self.log = open(os.path.join(LOGD, "exo3node_hub.log"), "wb")
        self.socks = {}       # name -> (socket, qemu_local_port)
        self.by_fd = {}       # socket -> name
        for name, (_, _, hport, lport) in NODES.items():
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.bind(("127.0.0.1", hport))
            s.setblocking(False)
            self.socks[name] = (s, lport)
            self.by_fd[s] = name
        self.stop = False
        self.frames = 0
        self.t = threading.Thread(target=self._run, daemon=True)
        self.t.start()

    def _run(self):
        # select() over all ports + FULL drain of each ready socket:
        # a naive round-robin recv (1 datagram/socket/cycle) backlogs
        # ARP/TCP bursts for seconds and breaks handshakes under TCG.
        while not self.stop:
            try:
                ready, _, _ = select.select(list(self.by_fd), [], [], 0.5)
            except (OSError, ValueError):
                break
            for s in ready:
                name = self.by_fd[s]
                while True:
                    try:
                        data = s.recv(65535)
                    except (BlockingIOError, InterruptedError):
                        break
                    except OSError:
                        break
                    self.frames += 1
                    for other, (os_, olport) in self.socks.items():
                        if other != name:
                            try:
                                os_.sendto(data, ("127.0.0.1", olport))
                            except OSError:
                                pass
            if self.frames:
                self.log.write(b"hub: %d frames total\n" % self.frames)
                self.log.flush()

    def close(self):
        self.stop = True
        for s, _ in self.socks.values():
            s.close()
        self.log.close()

# --------------------------------------------------------------------------
class Node:
    def __init__(self, name, args):
        self.name = name
        self.proc = subprocess.Popen(args, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     bufsize=0)
        self.buf = b""
        self.log = open(os.path.join(LOGD, f"exo3node_{name}.log"), "wb")
        self.lock = threading.Lock()
        self.t = threading.Thread(target=self._reader, daemon=True)
        self.t.start()

    def _reader(self):
        while True:
            chunk = self.proc.stdout.read(256)
            if not chunk:
                break
            self.log.write(chunk); self.log.flush()
            with self.lock:
                self.buf += chunk
                if len(self.buf) > 6_000_000:
                    self.buf = self.buf[-3_000_000:]

    def text(self):
        with self.lock:
            return self.buf.decode("utf-8", "replace")

    def wait(self, marker, timeout=120, since=0):
        t0 = time.time()
        while time.time() - t0 < timeout:
            txt = self.text()
            if marker in txt[since:]:
                return txt
            if self.proc.poll() is not None:
                raise RuntimeError(f"{self.name} exited rc={self.proc.returncode}")
            time.sleep(0.25)
        raise TimeoutError(f"{self.name}: '{marker}' not seen in {timeout}s")

    def send(self, cmd):
        self.proc.stdin.write(cmd.encode() + b"\n")
        self.proc.stdin.flush()

    def mark(self):
        return len(self.text())

def ring_order_of(txt):
    """Parse exoring-auto rows (' ring[i] <id> <ip:port> ram_free=...')
    -> ordered list of node ids."""
    order = []
    for line in txt.splitlines():
        m = re.match(r"\s*ring\[(\d+)\]\s+(\S+)\s+\S+:\d+\s+ram_free=", line)
        if m:
            order.append((int(m.group(1)), m.group(2)))
    return [nid for _, nid in sorted(order)]

def main():
    hub = Hub()
    print("[demo] L2 hub up (udp 12001-12003 <-> 12101-12103)", flush=True)

    base = [QEMU, "-kernel", ELF, "-display", "none", "-serial", "stdio",
            "-monitor", "none", "-smp", "1", "-no-reboot"]
    nodes = {}
    for i, (name, (mac, _, hport, lport)) in enumerate(NODES.items()):
        netdev = (f"dgram,id=n0,local.type=inet,local.host=127.0.0.1,"
                  f"local.port={lport},remote.type=inet,"
                  f"remote.host=127.0.0.1,remote.port={hport}")
        nodes[name] = Node(name, base + ["-m", "2048",
            "-netdev", netdev,
            "-device", f"virtio-net-pci,netdev=n0,mac={mac}"])
        time.sleep(3)
    A, B, C = nodes["A"], nodes["B"], nodes["C"]
    ok = False
    try:
        print("[demo] waiting for shells...", flush=True)
        for n in nodes.values():
            n.wait("embodios>", 300)
        print("[demo] all shells up", flush=True)

        for name, n in nodes.items():
            ip = NODES[name][1]
            m = n.mark(); n.send(f"setip {ip} 255.255.255.248 10.0.0.1")
            n.wait("IP set", 30, since=m)
            m = n.mark(); n.send(f"exo node{name} 50051")
            n.wait("discovery started", 60, since=m)
            print(f"[demo] {name}: ip={ip}, exo up", flush=True)

        # live discovery: beacons every 2s — wait until every node sees 3
        print("[demo] waiting for live discovery (3 nodes everywhere)...", flush=True)
        for name, n in nodes.items():
            t0 = time.time()
            while time.time() - t0 < 120:
                m = n.mark(); n.send("exodiscover")
                n.wait("age(s)", 20, since=m)
                txt = n.text()[m:]
                if "exo discovery: 3 node(s)" in txt:
                    break
                time.sleep(4)
            else:
                raise TimeoutError(f"{name}: discovery never reached 3 nodes")
            lines = [l for l in n.text()[m:].splitlines() if re.match(r"\s+\d\s+\S", l)]
            print(f"[demo] {name} discovery table:\n" + "\n".join(lines), flush=True)

        # warm-up: lazy-load the embedded model on ALL nodes (parallel)
        marks_w = {}
        for name, n in nodes.items():
            marks_w[name] = n.mark(); n.send("chat hi")
        for name, n in nodes.items():
            n.wait("tok/s", 2400, since=marks_w[name])
            print(f"[demo] {name} model loaded", flush=True)

        # auto ring on all nodes — must be identical everywhere
        rings = {}
        for name, n in nodes.items():
            m = n.mark(); n.send("exoring auto")
            n.wait("ring closes", 30, since=m)
            txt = n.text()[m:]
            rings[name] = ring_order_of(txt)
            orch = re.search(r"ring\[0\] (\S+).*<orchestrator>", txt)
            print(f"[demo] {name} ring: {rings[name]} "
                  f"(orchestrator={orch.group(1) if orch else '?'})", flush=True)
        if not (rings["A"] == rings["B"] == rings["C"] and len(rings["A"]) == 3):
            print(f"[demo] RING MISMATCH: {rings}")
            return 1
        orch_id = rings["A"][0]            # e.g. 'nodeA'
        orch = nodes[orch_id[-1]]          # 'nodeA' -> 'A'
        print(f"[demo] identical ring on all nodes, orchestrator={orch_id}", flush=True)

        # auto shard (RAM-weighted; equal RAM -> 10/10/10)
        for name, n in nodes.items():
            m = n.mark(); n.send("exoshard auto")
            n.wait("local shard", 60, since=m)
            txt = n.text()[m:]
            rng = re.findall(r"ring\[\d\] node \S+ layers (\d+)\.\.(\d+) \((\d+)\)", txt)
            rng = re.findall(r"layers (\d+)\.\.(\d+) \((\d+)\)", txt)
            counts = [int(c) for _, _, c in rng]
            print(f"[demo] {name} shards: {rng}", flush=True)
            if counts != [10, 10, 10]:
                print(f"[demo] SHARD SPLIT NOT 10/10/10 on {name}: {counts}")
                return 1

        # ring generation on the orchestrator
        print(f"[demo] exochat on {orch_id} (ring generation)...", flush=True)
        marks = {name: n.mark() for name, n in nodes.items()}
        orch.send("exochat 8 What is the capital of France?")
        t0 = time.time()
        # Completion = a terminal marker printed by the generation path
        # ("ring generate done" / "chat failed" / "generation aborted")
        # FOLLOWED by a fresh shell prompt. Do NOT break on bare substrings
        # like "timeout" — retried transport messages (e.g. first-attempt
        # handshake timeout) are recoverable and the ring keeps going.
        def _gen_done(tail):
            for marker in ("ring generate done", "chat failed",
                           "generation aborted", "generation failed"):
                p = tail.find(marker)
                if p >= 0 and "embodios>" in tail[p:]:
                    return True
            return False
        while time.time() - t0 < 5400:
            tail = orch.text()[marks[orch_id[-1]]:]
            if _gen_done(tail):
                break
            if orch.proc.poll() is not None:
                raise RuntimeError("orchestrator exited")
            time.sleep(5)
        out_orch = orch.text()[marks[orch_id[-1]]:]
        print("=" * 60)
        print(f"[demo] ORCHESTRATOR ({orch_id}) output:")
        print(out_orch[-2500:])
        print("=" * 60)
        for name, n in nodes.items():
            if n is orch:
                continue
            hops = [l for l in n.text()[marks[name]:].splitlines()
                    if "forward_shard" in l or "RESULT" in l or "TENSOR" in l]
            print(f"[demo] {name} ring activity lines: {len(hops)}")
            for l in hops[:6]:
                print(f"  {name}|", l)

        # verdict
        paris = "Paris" in out_orch or "paris" in out_orch
        errs = []
        for name, n in nodes.items():
            tail = n.text()[marks[name]:]
            for bad in ("DEGRADED", "RESULT timeout", "TENSOR send failed",
                        "transport: write fail", "chat failed", "triple", "PANIC"):
                if bad in tail:
                    errs.append(f"{name}:{bad}")
        ok = paris and not errs
        print(f"[demo] Paris={'yes' if paris else 'NO'} "
              f"transport_errors={errs or 'none'}")
        print(f"[demo] RESULT: {'PASS' if ok else 'FAIL'}")
        return 0 if ok else 1
    finally:
        for n in nodes.values():
            try: n.proc.kill()
            except Exception: pass
        hub.close()
        print(f"[demo] logs: {LOGD}/exo3node_{{A,B,C,hub}}.log")

if __name__ == "__main__":
    sys.exit(main())
