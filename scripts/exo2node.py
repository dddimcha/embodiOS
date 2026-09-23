#!/usr/bin/env python3
"""exo two-node ring demo over a QEMU -netdev socket pair.

Node A (orchestrator, ring[0]): -m 3072, socket listen :11234, ip 10.0.0.1
Node B (tail):                  -m 2048, socket connect,       ip 10.0.0.2
Single virtio-net NIC per node (tcpip stack supports one interface).
Trigger: `exochat 8 <prompt>` on A's serial (no host HTTP path needed).

Usage: python3 exo2node.py [elf] [prompt]
Logs: $EXO_LOGDIR (default ./logs)
"""
import os, re, subprocess, sys, threading, time

HOME = os.environ["HOME"]
SYSROOT = os.path.join(HOME, "sysroot")
os.environ["LD_LIBRARY_PATH"] = (f"{SYSROOT}/usr/lib/x86_64-linux-gnu:"
                                 f"{SYSROOT}/lib/x86_64-linux-gnu:" +
                                 os.environ.get("LD_LIBRARY_PATH", ""))
os.environ["QEMU_MODULE_DIR"] = f"{SYSROOT}/usr/lib/x86_64-linux-gnu/qemu"
QEMU = os.path.join(SYSROOT, "usr/bin/qemu-system-x86_64")
ELF = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("EMBODIOS_ELF", "kernel/embodios.elf")
PROMPT = sys.argv[2] if len(sys.argv) > 2 else "The capital of France is"
LOGD = os.environ.get("EXO_LOGDIR", "logs")
os.makedirs(LOGD, exist_ok=True)

class Node:
    def __init__(self, name, args):
        self.name = name
        self.proc = subprocess.Popen(args, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     bufsize=0)
        self.buf = b""
        self.log = open(os.path.join(LOGD, f"exo2node_{name}.log"), "wb")
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
                if len(self.buf) > 4_000_000:
                    self.buf = self.buf[-2_000_000:]

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

def main():
    base = [QEMU, "-kernel", ELF, "-display", "none", "-serial", "stdio",
            "-monitor", "none", "-smp", "1", "-no-reboot"]
    nodeA = Node("A", base + ["-m", "3072",
        "-netdev", "socket,id=p0,listen=:11234",
        "-device", "virtio-net-pci,netdev=p0,mac=52:54:00:00:00:0A"])
    time.sleep(2)
    nodeB = Node("B", base + ["-m", "2048",
        "-netdev", "socket,id=p0,connect=127.0.0.1:11234",
        "-device", "virtio-net-pci,netdev=p0,mac=52:54:00:00:00:0B"])
    try:
        print("[demo] waiting for shells...", flush=True)
        nodeA.wait("embodios>", 180); nodeB.wait("embodios>", 180)
        print("[demo] both shells up", flush=True)

        steps = [
            (nodeA, "setip 10.0.0.1 255.255.255.252 10.0.0.2", "IP set", 20),
            (nodeB, "setip 10.0.0.2 255.255.255.252 10.0.0.1", "IP set", 20),
            (nodeA, "exo nodeA 50051", "exo", 30),
            (nodeB, "exo nodeB 50051", "exo", 30),
            (nodeA, "exopeer nodeB 10.0.0.2 50051 2048", "exo", 20),
            (nodeB, "exopeer nodeA 10.0.0.1 50051 3072", "exo", 20),
        ]
        for node, cmd, marker, to in steps:
            m = node.mark(); node.send(cmd); node.wait(marker, to, since=m)
            print(f"[demo] {node.name}: {cmd} -> ok", flush=True)

        # warm-up: lazy-load the embedded model on BOTH nodes
        for node in (nodeA, nodeB):
            m = node.mark(); node.send("chat hi")
            node.wait("tok/s", 900, since=m)
            print(f"[demo] {node.name} model loaded", flush=True)

        # shard assignment on both (even split -> identical rings on both)
        for node in (nodeA, nodeB):
            m = node.mark(); node.send("exoshard smollm 30 even")
            txt = node.wait("ring[", 30, since=m)
            print(f"[demo] {node.name} shard:\n" +
                  "\n".join(l for l in txt[m:].splitlines() if "[EXO]" in l), flush=True)

        # ring generation on orchestrator A
        print("[demo] exochat on A (ring generation)...", flush=True)
        mA, mB = nodeA.mark(), nodeB.mark()
        nodeA.send(f"exochat 8 {PROMPT}")
        txtA = nodeA.wait("EXO]", 900, since=mA)  # any [exo]/[EXO] progress
        # wait until generation done: emit ends with done newline after '[EXO] ring generate'
        t0 = time.time()
        while time.time() - t0 < 900:
            tail = nodeA.text()[mA:]
            if re.search(r"\[exo\]|\[EXO\]", tail) and "embodios>" in tail:
                break
            time.sleep(1)
        outA = nodeA.text()[mA:]
        outB = nodeB.text()[mB:]
        print("=" * 60)
        print("[demo] NODE A output (orchestrator):")
        print(outA[-3000:])
        print("=" * 60)
        hops = [l for l in outB.splitlines() if "TENSOR" in l.upper() or "[EXO]" in l]
        print(f"[demo] NODE B ring activity lines: {len(hops)}")
        for l in hops[:20]:
            print("  B|", l)
        # control: same prompt through LOCAL full-model path on A
        mA2 = nodeA.mark()
        nodeA.send("chat " + PROMPT)
        try:
            nodeA.wait("tok/s", 600, since=mA2)
        except TimeoutError:
            pass
        ctrl = nodeA.text()[mA2:]
        lines = [l for l in ctrl.splitlines() if "tokens" in l or "you>" in l]
        print("[demo] CONTROL local chat on A: " + " | ".join(lines[-2:]), flush=True)
        for node in (nodeA, nodeB):
            m = node.mark(); node.send("tcpsockets"); time.sleep(3)
            print(f"[demo] {node.name} sockets:\n" + node.text()[m:], flush=True)
        for node in (nodeA, nodeB):
            m = node.mark(); node.send("net"); time.sleep(4)
            st = node.text()[m:]
            keep = [l for l in st.splitlines() if any(k in l for k in
                    ("Packets", "ARP", "TCP", "ICMP", "UDP", "Dropped", "Errors"))]
            print(f"[demo] {node.name} net stats: " + " | ".join(x.strip() for x in keep), flush=True)
        ok = ("ring generate" in outA or "ring" in outA.lower()) and len(hops) > 0
        print(f"[demo] RESULT: {'PASS' if ok else 'CHECK LOGS'}")
        return 0 if ok else 1
    finally:
        for n in (nodeA, nodeB):
            try: n.proc.kill()
            except Exception: pass
        print(f"[demo] logs: {LOGD}/exo2node_A.log, {LOGD}/exo2node_B.log")

if __name__ == "__main__":
    sys.exit(main())
