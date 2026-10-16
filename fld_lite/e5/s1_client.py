#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-License-Identifier: Apache-2.0
#
# s1_client.py -- UDP architecture step 1 client ("UDP ping" of the FPGA). Runs as root inside a network namespace that
# owns the peer port (so traffic really crosses the cable). The FPGA endpoint (FLD-lite + udp_swap) answers on
# FPGA_IP:7777; Linux on the FPGA's own port owns FPGA_IP for ARP, ping and closed ports.
#   usage: s1_client.py <netdev in this netns> [fpga ip] [my ip] [port] [--mode full|nocsum|norxchk]
#   --mode nocsum   NEGATIVE CONTROL (host run with --no-csum): the FPGA still echoes but the NIC leaves the zeroed
#                   checksums alone -> the client kernel must drop every reply; tcpdump must show them, with bad cksum
#   --mode norxchk  NEGATIVE CONTROL (host run with --no-rxchk): datagrams with a bad UDP checksum now reach udp_swap
#                   and the NIC recomputes valid checksums on the reply -> they MUST be echoed
import os, socket, statistics, struct, subprocess, sys, time

MODE = "full"
if "--mode" in sys.argv:
    i = sys.argv.index("--mode")
    MODE = sys.argv[i + 1]
    del sys.argv[i:i + 2]
DEV = sys.argv[1]
FPGA_IP = sys.argv[2] if len(sys.argv) > 2 else "192.168.77.2"
MY_IP = sys.argv[3] if len(sys.argv) > 3 else "192.168.77.1"
PORT = int(sys.argv[4]) if len(sys.argv) > 4 else 7777
FPGA = (FPGA_IP, PORT)
results = []


def check(ok, msg):
    print(("  PASS  " if ok else "  FAIL  ") + msg, flush=True)
    results.append(ok)


def csum16(b, s=0):
    if len(b) & 1:
        b += b"\0"
    for i in range(0, len(b), 2):
        s += (b[i] << 8) | b[i + 1]
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return (~s) & 0xFFFF


def mac_of(dev):
    return bytes.fromhex(open(f"/sys/class/net/{dev}/address").read().strip().replace(":", ""))


def neigh_mac(ip):
    out = subprocess.run(["ip", "neigh", "show", ip], capture_output=True, text=True).stdout.split()
    return bytes.fromhex(out[out.index("lladdr") + 1].replace(":", "")) if "lladdr" in out else None


def raw_frame(dmac, smac, sport, payload, bad_udp=False, ip_opt=False):
    opt = b"\x01\x01\x01\x00" if ip_opt else b""
    ihl = 20 + len(opt)
    ulen = 8 + len(payload)
    src, dst = socket.inet_aton(MY_IP), socket.inet_aton(FPGA_IP)
    ip = struct.pack("!BBHHHBBH4s4s", 0x40 | (ihl // 4), 0, ihl + ulen, 0x1234, 0x4000, 64, 17, 0, src, dst) + opt
    ip = ip[:10] + struct.pack("!H", csum16(ip)) + ip[12:]
    udp = struct.pack("!HHHH", sport, PORT, ulen, 0) + payload
    c = csum16(src + dst + struct.pack("!BBH", 0, 17, ulen) + udp) or 0xFFFF
    if bad_udp:
        c ^= 0x00FF
    udp = udp[:6] + struct.pack("!H", c) + udp[8:]
    return dmac + smac + b"\x08\x00" + ip + udp


s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind((MY_IP, 0))
s.settimeout(0.5)
myport = s.getsockname()[1]
print(f"client {MY_IP}:{myport} on {DEV} -> FPGA {FPGA_IP}:{PORT}, mode {MODE}", flush=True)


def finish():
    print(f"S1-CLIENT {'PASS' if all(results) else 'FAIL'} ({results.count(False)} failed of {len(results)}, mode {MODE})", flush=True)
    sys.exit(0)


if MODE == "nocsum":
    tcp = subprocess.Popen(["timeout", "8", "tcpdump", "-i", DEV, "-nn", "-vv", "-c", "6", "udp and src port", str(PORT)],
                           stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    time.sleep(1.5)
    got = 0
    for n in [10, 300, 1200, 10, 300, 1200]:
        s.sendto(os.urandom(n), FPGA)
        try:
            s.recvfrom(4096)
            got += 1
        except socket.timeout:
            pass
    out = tcp.communicate()[0]
    seen = out.count(f"{FPGA_IP}.{PORT} >")
    check(seen >= 1, f"control: the FPGA still sent its replies ({seen} seen by tcpdump on the cable side)")
    check(out.count("bad cksum") >= 1, f"control: replies carry a bad IP checksum without cs_flags ({out.count('bad cksum')} 'bad cksum' in tcpdump)")
    check(got == 0, f"control: client kernel received {got}/6 replies (expected 0: dropped for the bad checksum)")
    finish()

# 1. Linux side of the split stack: ping (ARP + ICMP answered by Linux on the FPGA's port)
r = subprocess.run(["ping", "-c", "3", "-W", "1", "-i", "0.2", FPGA_IP], capture_output=True, text=True)
check(r.returncode == 0, f"ping {FPGA_IP} answered by Linux (ARP + ICMP): " + (r.stdout.strip().splitlines()[-1] if r.stdout else r.stderr.strip()))

# 2. UDP echo from the FPGA at sizes across the inline/pointer boundary (fld_tx inline cap 222 B frame)
for n in [1, 18, 64, 100, 180, 222, 223, 512, 1000, 1472]:
    ok, rtts = 0, []
    for i in range(50):
        p = os.urandom(n)
        t = time.perf_counter()
        s.sendto(p, FPGA)
        try:
            d, a = s.recvfrom(4096)
            if d == p and a == FPGA:
                ok += 1
                rtts.append((time.perf_counter() - t) * 1e6)
        except socket.timeout:
            pass
    check(ok == 50, f"echo {n:4d} B payload: {ok}/50 byte-exact from {FPGA[0]}:{FPGA[1]}"
          + (f", RTT median {statistics.median(rtts):.1f} us (Python client; kernel UDP both ways)" if rtts else ""))

# 3. reply checksums on the wire (the FPGA zeroed both fields; the NIC must have filled them)
tcp = subprocess.Popen(["timeout", "6", "tcpdump", "-i", DEV, "-nn", "-vv", "-c", "6", "udp and src port", str(PORT)],
                       stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
time.sleep(1.5)
for n in [10, 300, 1200, 10, 300, 1200]:
    s.sendto(os.urandom(n), FPGA)
    try:
        s.recvfrom(4096)
    except socket.timeout:
        pass
out = tcp.communicate()[0]
good_udp = out.count("udp sum ok")
bad = out.count("bad cksum") + out.count("bad udp cksum")
check(good_udp == 6 and bad == 0, f"tcpdump on the cable side: {good_udp}/6 replies with 'udp sum ok', {bad} bad IP/UDP checksums")

# 4. closed port: Linux (not the FPGA) answers with ICMP port unreachable
c = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
c.settimeout(1.0)
c.connect((FPGA_IP, PORT + 1))
c.send(b"closed?")
refused = False
try:
    c.recv(100)
except ConnectionRefusedError:
    refused = True
except socket.timeout:
    pass
check(refused, f"port {PORT + 1} (no steering rule) -> ICMP port unreachable from Linux")

# 5. frames the FPGA must NOT echo (dropped by fld_rx from the CQE bits), with a good raw frame as positive control
dmac = neigh_mac(FPGA_IP)
smac = mac_of(DEV)
if dmac is None:
    check(False, "no ARP entry for the FPGA IP -- raw injection skipped")
else:
    rs = socket.socket(socket.AF_PACKET, socket.SOCK_RAW)
    rs.bind((DEV, 0))

    def inject(label, expect_echo, **kw):
        marker = os.urandom(16)
        rs.send(raw_frame(dmac, smac, myport, marker, **kw))
        got = False
        try:
            while True:
                d, a = s.recvfrom(4096)
                if d == marker:
                    got = True
                    break
        except socket.timeout:
            pass
        return got

    check(inject("good", True), "raw-injected GOOD datagram is echoed (positive control for the injection path)")
    if MODE == "norxchk":
        n_echo = sum(inject("bad udp", True, bad_udp=True) for i in range(5))
        check(n_echo == 5, f"control: with rxchk OFF, {n_echo}/5 bad-UDP-checksum datagrams ARE echoed (the drop in the main run is fld_rx's)")
        finish()
    for i in range(5):
        e1 = inject("bad udp", False, bad_udp=True)
        e2 = inject("ip options", False, ip_opt=True)
        if e1 or e2:
            break
    check(not e1, "datagram with a BAD UDP checksum is NOT echoed (fld_rx drops it from CQE L4_OK) x5")
    check(not e2, "datagram with IP OPTIONS is NOT echoed (fld_rx drops it from the CQE options flag) x5")

print(f"S1-CLIENT {'PASS' if all(results) else 'FAIL'} ({results.count(False)} failed of {len(results)})", flush=True)
