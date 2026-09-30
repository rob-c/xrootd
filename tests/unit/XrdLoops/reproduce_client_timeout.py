#!/usr/bin/env python3
"""Exercise an ordinary xrdfs against a deliberately incomplete local peer.

No XRootD library is imported, replaced, patched, or mocked. The script starts
an IPv4 loopback TCP listener, performs the real handshake, protocol and login
exchanges, then sends well-formed response headers with controlled body timing.
The installed xrdfs executable supplies the client, sockets and event loops.

Example before/after (pin LD_LIBRARY_PATH to the matching build on Linux):
  python3 reproduce_client_timeout.py --client /path/to/upstream/bin/xrdfs \
      --scenario partial-drip --expect hang
  python3 reproduce_client_timeout.py --client /path/to/fixed/bin/xrdfs \
      --scenario partial-drip --expect timeout
  python3 reproduce_client_timeout.py --client /path/to/fixed/bin/xrdfs \
      --scenario fragmented --expect success

User scenario: a storage server, proxy, or failing network starts a reply and
keeps TCP alive without finishing the operation. A configured two-second
request deadline must still terminate the operation. 'hang' means the external
eight-second watchdog killed it; this is a bounded observation, not a claim
that the script proved an infinite execution. Exact-upstream results belong in
the accompanying evidence document; the script never infers them from a mock.
"""

import argparse
import json
import os
import pathlib
import socket
import struct
import subprocess
import threading
import time


PAYLOAD = (b"sentinel deadline regression\n" * 160)[:4096]
STAT = f"1 {len(PAYLOAD)} 48 0".encode()


def frame(sid, status, body=b"", declared=None):
    return sid + struct.pack("!HI", status, len(body) if declared is None else declared) + body


def receive(sock, count):
    result = bytearray()
    while len(result) < count:
        block = sock.recv(count - len(result))
        if not block:
            raise EOFError
        result.extend(block)
    return bytes(result)


class Peer:
    def __init__(self, scenario, operation, deadline):
        self.scenario = scenario
        self.operation = operation
        self.deadline = deadline
        self.stop = threading.Event()
        self.transcript = []
        self.errors = []
        self.sockets = []
        self.workers = []
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen()
        self.listener.settimeout(0.2)
        self.port = self.listener.getsockname()[1]

    def send_response(self, sock, sid, body):
        scenario = self.scenario
        self.transcript.append({"response": scenario, "bytes": len(body)})
        if scenario == "clean":
            sock.sendall(frame(sid, 0, body))
        elif scenario == "fragmented":
            # Real short TCP reads across every header byte and several body
            # boundaries, followed by the final frame on the same connection.
            first = frame(sid, 4000, body[:11])
            for value in first:
                sock.sendall(bytes([value]))
                if self.stop.wait(0.005):
                    return
            sock.sendall(frame(sid, 0, body[11:]))
        elif scenario == "partial-flood":
            # Complete oksofar frames never finish the request. Reading whole
            # chunks must not reset its original deadline.
            while not self.stop.is_set():
                sock.sendall(frame(sid, 4000, b""))
                self.stop.wait(0.01)
        elif scenario == "late-final":
            # The header arrives on time; the final body arrives after expiry.
            sock.sendall(frame(sid, 0, body[:1], len(body)))
            if not self.stop.wait(self.deadline + 1):
                sock.sendall(body[1:])
        else:
            status = 0 if scenario == "final-stall" else 4000
            declared = max(len(body), 4096) if scenario == "partial-drip" else len(body)
            sock.sendall(frame(sid, status, body[:1], declared))
            if scenario == "partial-drip":
                # These bytes reset inactivity, but cannot complete this frame
                # before the watchdog. A low stream timeout cannot substitute
                # for enforcing the user's request deadline.
                while not self.stop.wait(0.2):
                    sock.sendall(body[1:2])
            else:
                self.stop.wait()

    def connection(self, sock):
        try:
            sock.settimeout(10)
            receive(sock, 20)
            sock.sendall(frame(b"\0\0", 0, struct.pack("!II", 0x310, 1)))
            while not self.stop.is_set():
                request = receive(sock, 24)
                sid = request[:2]
                code = struct.unpack("!H", request[2:4])[0]
                length = struct.unpack("!I", request[20:24])[0]
                if length > 1024 * 1024:
                    raise ValueError("unexpected client request length")
                receive(sock, length)
                self.transcript.append({"request": code})
                if code == 3006:  # kXR_protocol, pre-TLS and pre-page-I/O
                    sock.sendall(frame(sid, 0, struct.pack("!II", 0x310, 1)))
                elif code == 3007:  # kXR_login, no authentication requested
                    sock.sendall(frame(sid, 0, b"\0" * 16))
                elif code == 3010:  # kXR_open, handle + compression + stat
                    sock.sendall(frame(sid, 0, b"file" + b"\0" * 8 + STAT))
                elif code == 3017:  # kXR_stat
                    info = b"1 0 18 0" if self.operation == "list" else STAT
                    sock.sendall(frame(sid, 0, info))
                elif code == 3013:  # kXR_read
                    offset = struct.unpack("!Q", request[8:16])[0]
                    size = struct.unpack("!I", request[16:20])[0]
                    self.send_response(sock, sid, PAYLOAD[offset:offset + size])
                elif code == 3004:  # kXR_dirlist
                    self.send_response(sock, sid, b"alpha\nbeta\ngamma\n")
                elif code == 3001:  # kXR_query config used by copy setup
                    sock.sendall(frame(sid, 0, b"1\n"))
                elif code in (3003, 3011, 3023):  # close, ping, endsess
                    sock.sendall(frame(sid, 0))
                else:
                    raise ValueError(f"unsupported client request {code}")
        except (EOFError, ConnectionError, OSError):
            pass  # The expected timeout closes this TCP connection.
        except Exception as error:
            self.errors.append(repr(error))
        finally:
            sock.close()

    def run(self):
        while not self.stop.is_set():
            try:
                sock, _ = self.listener.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            self.sockets.append(sock)
            worker = threading.Thread(target=self.connection, args=(sock,), daemon=True)
            self.workers.append(worker)
            worker.start()

    def close(self):
        self.stop.set()
        self.listener.close()
        for sock in self.sockets:
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        for worker in self.workers:
            worker.join(timeout=1)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--client", type=pathlib.Path, required=True, help="absolute xrdfs executable")
    parser.add_argument("--scenario", choices=("clean", "fragmented", "partial-stall", "partial-drip", "partial-flood", "final-stall", "late-final"), default="partial-drip")
    parser.add_argument("--operation", choices=("read", "list"), default="read")
    parser.add_argument("--deadline", type=int, default=2)
    parser.add_argument("--watchdog", type=int, default=8)
    parser.add_argument("--expect", choices=("success", "timeout", "hang", "crash"))
    args = parser.parse_args()
    peer = Peer(args.scenario, args.operation, args.deadline)
    server = threading.Thread(target=peer.run, daemon=True)
    server.start()
    command = [str(args.client.resolve()), f"root://127.0.0.1:{peer.port}", "cat" if args.operation == "read" else "ls", "/fixture"]
    env = dict(os.environ, XRD_REQUESTTIMEOUT=str(args.deadline), XRD_TIMEOUTRESOLUTION="1", XRD_STREAMTIMEOUT="60", XRD_CONNECTIONWINDOW="10", XRD_CONNECTIONRETRY="1", XRD_CPCHUNKSIZE="4096", XRD_CPPARALLELCHUNKS="1")
    start = time.monotonic()
    try:
        child = subprocess.Popen(command, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            stdout, stderr = child.communicate(timeout=args.watchdog)
            outcome = "success" if child.returncode == 0 else "timeout"
            if child.returncode < 0 or b"AddressSanitizer" in stderr:
                outcome = "crash"
            elif child.returncode and b"expired" not in stderr.lower() and b"timeout" not in stderr.lower():
                outcome = "error"
        except subprocess.TimeoutExpired:
            child.kill()
            stdout, stderr = child.communicate()
            outcome = "hang"
    finally:
        peer.close()
        server.join(timeout=1)
    elapsed = time.monotonic() - start
    # A successful control must return exact data, not merely exit without an
    # error. xrdfs ls prefixes each entry with the requested directory.
    expected = PAYLOAD if args.operation == "read" else b"/fixture/alpha\n/fixture/beta\n/fixture/gamma\n"
    if outcome == "success" and stdout != expected:
        peer.errors.append(f"wrong stdout: expected {len(expected)} bytes, got {stdout[:100]!r}")
    result = dict(client=str(args.client.resolve()), scenario=args.scenario, operation=args.operation, deadline=args.deadline, watchdog=args.watchdog, outcome=outcome, returncode=child.returncode, elapsed=round(elapsed, 3), stdout_bytes=len(stdout), stderr=stderr.decode(errors="replace"), peer_errors=peer.errors, transcript=peer.transcript[:30])
    print(json.dumps(result, indent=2))
    if peer.errors or not any("response" in item for item in peer.transcript):
        return 2
    return int(args.expect is not None and args.expect != outcome)


if __name__ == "__main__":
    raise SystemExit(main())
