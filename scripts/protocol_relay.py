#!/usr/bin/env python3
"""protocol_relay.py -- passive TCP relay with protocol logging.

The game (3DS, or any build) connects here; this connects to the real
server and copies both directions byte-for-byte, parsing what it
recognizes:

  * server -> client: the full packet table from protocol_probe, passive
    (it never injects a byte), logging the teleport, the PreChunk/
    MapChunk burst, chat, kick and the diffable summary at the end.
  * client -> server: the position stream (Packet10/11/12/13), the
    handshake/login usernames, chat and disconnects -- what the real
    client's post-login behavior looks like on the wire, which is the
    differential the standalone probe could not reproduce against
    servers that gate chunk data per session.

Usage:
    python scripts/protocol_relay.py --listen-port <local port> \
        --target <server address> --target-port <server port> [--out relay.txt]

Then point the game at this machine's LAN IP and --listen-port.
"""

import argparse
import socket
import sys
import threading
import time

from protocol_probe import (BufWire, NeedMore, Capture, parse_packet,
                            summarize)

# The client -> server ids worth parsing; anything else disables the
# client-side parse for the rest of the session (forwarding never stops).
C2S_POSITION_SAMPLE = 10      # log the first N positions fully
C2S_POSITION_EVERY = 100      # then log every Nth


class RelayCapture(Capture):
    """Capture plus thread-safe logging and client->server tallies."""

    def __init__(self, out_file):
        Capture.__init__(self, out_file)
        self.lock = threading.Lock()
        self.s2c_parse_off = False
        self.c2s_parse_off = False
        self.c2s_counts = {}
        self.c2s_username = None
        self.c2s_positions = 0
        self.c2s_pos_last = None       # (monotonic time, x, y, stance, z)
        self.c2s_pos_deltas = []       # seconds between position packets

    def log(self, text):
        with self.lock:
            Capture.log(self, text)

    def record_position(self, kind, x, y, stance, z):
        now = time.monotonic()
        if self.c2s_pos_last is not None:
            self.c2s_pos_deltas.append(now - self.c2s_pos_last[0])
        self.c2s_pos_last = (now, x, y, stance, z)
        self.c2s_positions += 1
        if (self.c2s_positions <= C2S_POSITION_SAMPLE or
                self.c2s_positions % C2S_POSITION_EVERY == 0):
            if self.c2s_pos_deltas:
                cadence = 1000.0 * sum(self.c2s_pos_deltas) / len(self.c2s_pos_deltas)
                suffix = " cadence~%.0fms" % cadence
            else:
                suffix = ""
            self.log("C->S pos#%d (%s) x=%.2f y=%.2f stance=%.2f z=%.2f%s"
                     % (self.c2s_positions, kind, x, y, stance, z, suffix))


def parse_c2s(bw, cap):
    """Consume one client->server packet. 'need' if the buffer is short,
    'off' if the id is unknown (parse disabled), 'closed' on a 255."""
    start = bw.pos
    pid = bw.u8()
    if pid == 0:  # KeepAlive
        bw.i32()
    elif pid == 1:  # Login
        proto = bw.i32()
        user = bw.string()
        terrain = bw.string()
        bw.i32(); bw.i32(); bw.u8(); bw.u8(); bw.u8()
        cap.c2s_username = user
        cap.log("C->S Login proto=%d user=%r terrain=%r" % (proto, user, terrain))
    elif pid == 2:  # Handshake
        user = bw.string()
        cap.c2s_username = user
        cap.log("C->S Handshake user=%r" % user)
    elif pid == 3:  # Chat
        message = bw.string()
        cap.log("C->S Chat %r" % (message[:80],))
    elif pid == 10:  # Flying
        bw.u8()
    elif pid == 11:  # PlayerPosition: x, y, stance, z, onGround
        x, y, stance, z = bw.f64(), bw.f64(), bw.f64(), bw.f64()
        bw.u8()
        cap.record_position("11", x, y, stance, z)
    elif pid == 12:  # PlayerLook
        bw.f32(); bw.f32(); bw.u8()
    elif pid == 13:  # PlayerLookMove: x, y, stance, z, yaw, pitch, onGround
        x, y, stance, z = bw.f64(), bw.f64(), bw.f64(), bw.f64()
        bw.f32(); bw.f32(); bw.u8()
        cap.record_position("13", x, y, stance, z)
    elif pid == 255:  # client-initiated disconnect
        reason = bw.string()
        cap.log("C->S Disconnect %r" % reason)
        return "closed"
    else:
        cap.c2s_parse_off = True
        cap.log("C->S unknown id %d -- client-side parsing disabled "
                "(forwarding continues)" % pid)
        return "off"
    cap.c2s_counts[pid] = cap.c2s_counts.get(pid, 0) + 1
    return "ok"


def pump_server_to_client(server, client, cap, bw):
    """Forward server->client bytes, parsing the protocol passively."""
    try:
        while True:
            data = server.recv(65536)
            if not data:
                break
            client.sendall(data)
            if cap.s2c_parse_off:
                continue
            bw.feed(data)
            while not cap.s2c_parse_off:
                start = bw.pos
                try:
                    parse_packet(bw, cap, None, None, None)  # passive mode
                    bw.compact()
                except NeedMore:
                    bw.pos = start
                    break
                except ValueError as exc:
                    cap.log("S->C WALK STOPPED: %s (forwarding continues)" % exc)
                    cap.s2c_parse_off = True
    except OSError:
        pass
    finally:
        try:
            client.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass


def pump_client_to_server(client, server, cap, bw):
    """Forward client->server bytes, parsing the position stream."""
    try:
        while True:
            data = client.recv(65536)
            if not data:
                break
            server.sendall(data)
            if cap.c2s_parse_off:
                continue
            bw.feed(data)
            while not cap.c2s_parse_off:
                start = bw.pos
                try:
                    result = parse_c2s(bw, cap)
                    bw.compact()
                    if result in ("off", "closed"):
                        if result == "closed":
                            return
                        break
                except NeedMore:
                    bw.pos = start
                    break
    except OSError:
        pass
    finally:
        try:
            server.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass


def main():
    parser = argparse.ArgumentParser(
        description="Passive protocol-logging relay between the game and a server.")
    parser.add_argument("--listen-port", type=int, default=25569)
    # No default target: the relay must never quietly assume a server, the
    # same way the probe takes its host as a positional argument.
    parser.add_argument("--target", required=True)
    parser.add_argument("--target-port", type=int, default=25565)
    parser.add_argument("--out", default=None, help="also write the log to this file")
    parser.add_argument("--verbose", action="store_true",
                        help="log every server->client packet")
    args = parser.parse_args()

    out_file = open(args.out, "a", encoding="utf-8") if args.out else None
    listen = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listen.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listen.bind(("0.0.0.0", args.listen_port))
    listen.listen(4)
    print("relay listening on 0.0.0.0:%d -> %s:%d"
          % (args.listen_port, args.target, args.target_port))
    print("point the game at THIS machine's LAN IP, port %d" % args.listen_port)
    sys.stdout.flush()

    while True:
        client, addr = listen.accept()
        cap = RelayCapture(out_file)
        cap.verbose = args.verbose
        cap.log("client %s:%d connected -- relaying to %s:%d"
                % (addr[0], addr[1], args.target, args.target_port))
        try:
            server = socket.create_connection((args.target, args.target_port),
                                              timeout=10.0)
        except OSError as exc:
            cap.log("cannot reach target: %s" % exc)
            client.close()
            continue
        bw_s = BufWire()   # server -> client
        bw_c = BufWire()   # client -> server
        t1 = threading.Thread(target=pump_server_to_client,
                               args=(server, client, cap, bw_s), daemon=True)
        t2 = threading.Thread(target=pump_client_to_server,
                              args=(client, server, cap, bw_c), daemon=True)
        t1.start()
        t2.start()
        t1.join()
        t2.join()
        client.close()
        server.close()
        elapsed = time.monotonic() - cap.start
        summarize(cap, elapsed, "closed")
        if cap.c2s_positions:
            deltas = [d for d in cap.c2s_pos_deltas if d > 0]
            if deltas:
                cadence = 1000.0 * sum(deltas) / len(deltas)
                cap.log("C->S position stream: %d packets, cadence avg "
                        "%.0f ms (min %.0f, max %.0f)"
                        % (cap.c2s_positions, cadence,
                           1000.0 * min(deltas), 1000.0 * max(deltas)))
            else:
                cap.log("C->S position stream: %d packets" % cap.c2s_positions)
        if cap.c2s_counts:
            cap.log("C->S packet counts: " + ", ".join(
                "%d:%d" % (pid, count)
                for pid, count in sorted(cap.c2s_counts.items())))
        if cap.c2s_username is not None:
            cap.log("client username: %r" % cap.c2s_username)
        cap.log("---- session end ----")
        if out_file is not None:
            out_file.flush()


if __name__ == "__main__":
    main()
