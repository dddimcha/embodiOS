#!/usr/bin/env python3
"""exo failure-handling kill test (v0.7.0 "Maxwell").

2-node ring over the userspace L2 hub (QEMU -netdev dgram endpoints —
stateless, so a restarted node rejoins by just sending again; a QEMU
`socket listen` netdev re-accepts a reconnected peer TX-only and silently
drops its RX, which would break the re-join half of this test).
LIVE discovery is used (no exopeer pinning).
Mid-generation, node B (ring tail) is SIGKILLed. Node A (orchestrator)
must:
  1. hit the RESULT timeout, abort generation with a console error,
  2. mark the ring DEGRADED (visible in `exo` status),
  3. stay alive and responsive (no hang, no triple-fault),
  4. expire B from discovery (EXO_NODE_TIMEOUT_MS=120 s) and rebalance
     to a 1-node ring,
  5. rediscover a RESTARTED B and reform a 2-node ring (re-join path).

Usage: python3 exo2node_kill.py [elf]
Logs:  /mnt/agents/work/logs/exo2node_kill_{A,B,B2,hub}.log
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

# L2 hub: A <-> B over dgram endpoints (same design as exo3node.py)
HUB_PORTS = {"A": (12011, 12111), "B": (12012, 12112)}  # name -> (hub, qemu)

class Hub:
    def __init__(self):
        self.log = open(os.path.join(LOGD, "exo2node_kill_hub.log"), "wb")
        self.socks = {}
        self.by_fd = {}
        for name, (hp, lp) in HUB_PORTS.items():
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.bind(("127.0.0.1", hp))
            s.setblocking(False)
            self.socks[name] = (s, lp)
            self.by_fd[s] = name
        self.stop = False
        self.t = threading.Thread(target=self._run, daemon=True)
        self.t.start()

    def _run(self):
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
                    except (BlockingIOError, InterruptedError, OSError):
                        break
                    for other, (os_, olp) in self.socks.items():
                        if other != name:
                            try:
                                os_.sendto(data, ("127.0.0.1", olp))
                            except OSError:
                                pass

    def close(self):
        self.stop = True
        for s, _ in self.socks.values():
            s.close()
        self.log.close()

def netdev_for(name):
    hp, lp = HUB_PORTS[name]
    return (f"dgram,id=n0,local.type=inet,local.host=127.0.0.1,"
            f"local.port={lp},remote.type=inet,remote.host=127.0.0.1,"
            f"remote.port={hp}")

class Node:
    def __init__(self, name, args):
        self.name = name
        self.proc = subprocess.Popen(args, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     bufsize=0)
        self.buf = b""
        self.log = open(os.path.join(LOGD, f"exo2node_kill_{name}.log"), "wb")
        self.lock = threading.Lock()
        self.t = threading.Thread(target=self._reader, daemon=True)
        self.t.start()

    def _reader(self):
        while True:
            try:
                chunk = self.proc.stdout.read(256)
            except Exception:
                break
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

def nodeB_args():
    base = [QEMU, "-kernel", ELF, "-display", "none", "-serial", "stdio",
            "-monitor", "none", "-smp", "1", "-no-reboot"]
    return base + ["-m", "2048",
        "-netdev", netdev_for("B"),
        "-device", "virtio-net-pci,netdev=n0,mac=52:54:00:00:00:0B"]

def main():
    hub = Hub()
    base = [QEMU, "-kernel", ELF, "-display", "none", "-serial", "stdio",
            "-monitor", "none", "-smp", "1", "-no-reboot"]
    A = Node("A", base + ["-m", "2048",
        "-netdev", netdev_for("A"),
        "-device", "virtio-net-pci,netdev=n0,mac=52:54:00:00:00:0A"])
    time.sleep(2)
    B = Node("B", nodeB_args())
    ok = False
    try:
        print("[kill] waiting for shells...", flush=True)
        A.wait("embodios>", 300); B.wait("embodios>", 300)

        for n, ip, gw in ((A, "10.0.0.1", "10.0.0.2"), (B, "10.0.0.2", "10.0.0.1")):
            m = n.mark(); n.send(f"setip {ip} 255.255.255.252 {gw}")
            n.wait("IP set", 30, since=m)
        for n, nid in ((A, "nodeA"), (B, "nodeB")):
            m = n.mark(); n.send(f"exo {nid} 50051")
            n.wait("discovery started", 60, since=m)
        print("[kill] exo up on both; waiting for live discovery...", flush=True)

        for n in (A, B):
            t0 = time.time()
            while time.time() - t0 < 120:
                m = n.mark(); n.send("exodiscover")
                n.wait("age(s)", 20, since=m)
                if "exo discovery: 2 node(s)" in n.text()[m:]:
                    break
                time.sleep(4)
            else:
                raise TimeoutError(f"{n.name}: discovery never reached 2 nodes")
        print("[kill] live discovery OK (2 nodes each)", flush=True)

        mA_w, mB_w = A.mark(), B.mark()
        A.send("chat hi"); B.send("chat hi")
        for n, mw in ((A, mA_w), (B, mB_w)):
            n.wait("tok/s", 2400, since=mw)
            print(f"[kill] {n.name} model loaded", flush=True)

        for n in (A, B):
            m = n.mark(); n.send("exoshard auto")
            n.wait("local shard", 60, since=m)
            rng = re.findall(r"layers (\d+)\.\.(\d+) \((\d+)\)", n.text()[m:])
            print(f"[kill] {n.name} shards: {rng}", flush=True)

        # start long ring generation on A, kill B mid-generation
        mA, mB = A.mark(), B.mark()
        A.send("exochat 64 Tell me a very long story about the ocean")
        B.wait("forward_shard", 1800, since=mB)   # B is working on ring tensors
        mB2 = B.mark()
        B.wait("forward_shard", 600, since=mB2)   # 2nd hop: definitely mid-generation
        print("[kill] ring generation in progress — killing node B NOW", flush=True)
        B.proc.kill()
        time.sleep(1)
        if B.proc.poll() is None:
            raise RuntimeError("node B failed to die")

        # A must abort cleanly within RESULT timeout (300 s, TCG-scaled) + margin
        print("[kill] waiting for A's clean abort (<=420s)...", flush=True)
        t0 = time.time()
        aborted = False
        while time.time() - t0 < 420:
            tail = A.text()[mA:]
            if "RESULT timeout" in tail and "embodios>" in tail:
                aborted = True
                break
            if A.proc.poll() is not None:
                raise RuntimeError(f"node A died rc={A.proc.returncode}")
            time.sleep(2)
        tail = A.text()[mA:]
        degraded = "DEGRADED" in tail
        print(f"[kill] A aborted={aborted} degraded={degraded}", flush=True)
        if not aborted:
            print("[kill] FAIL: A did not abort cleanly"); return 1

        # A must be responsive
        m = A.mark(); A.send("uptime"); A.wait("up ", 30, since=m)
        m = A.mark(); A.send("exo"); A.wait("exo node:", 30, since=m)
        print("[kill] A responsive after abort:", flush=True)
        print("\n".join(A.text()[m:].splitlines()[:12]), flush=True)

        # discovery must expire B (EXO_NODE_TIMEOUT_MS=120 s, TCG-scaled)
        # and rebalance to a 1-node ring
        print("[kill] waiting for B expiry + rebalance (<=240s)...", flush=True)
        t0 = time.time()
        expired = False
        while time.time() - t0 < 240:
            if "timed out" in A.text()[mA:]:
                expired = True
                break
            time.sleep(2)
        m = A.mark(); A.send("exodiscover"); A.wait("age(s)", 20, since=m)
        one = "exo discovery: 1 node(s)" in A.text()[m:]
        print(f"[kill] B expired={expired}, A table back to 1 node={one}", flush=True)

        # re-join: restart B, A must rediscover it and reform the ring
        print("[kill] restarting node B (re-join test)...", flush=True)
        B2 = Node("B2", nodeB_args())
        B2.wait("embodios>", 300)
        m = B2.mark(); B2.send("setip 10.0.0.2 255.255.255.252 10.0.0.1")
        B2.wait("IP set", 30, since=m)
        m = B2.mark(); B2.send("exo nodeB 50051")
        B2.wait("discovery started", 60, since=m)
        t0 = time.time()
        rejoined = False
        while time.time() - t0 < 120:
            m = A.mark(); A.send("exodiscover")
            A.wait("age(s)", 20, since=m)
            if "exo discovery: 2 node(s)" in A.text()[m:]:
                rejoined = True
                break
            time.sleep(4)
        ringtxt = [l for l in A.text().splitlines()
                   if "rebalanc" in l or "ring[0]" in l or "ring[1]" in l]
        print(f"[kill] B re-joined={rejoined}; A ring rebalance lines:", flush=True)
        for l in ringtxt[-6:]:
            print("  A|", l, flush=True)

        panic = any(x in A.text() for x in ("PANIC", "triple", "GENERAL PROTECTION"))
        ok = aborted and degraded and expired and one and rejoined and not panic
        print(f"[kill] RESULT: {'PASS' if ok else 'FAIL'} "
              f"(abort={aborted} degraded={degraded} expired={expired} "
              f"rejoined={rejoined} panic={panic})")
        B2.proc.kill()
        return 0 if ok else 1
    finally:
        for n in (A, B):
            try: n.proc.kill()
            except Exception: pass
        hub.close()
        print(f"[kill] logs: {LOGD}/exo2node_kill_*.log")

if __name__ == "__main__":
    sys.exit(main())
