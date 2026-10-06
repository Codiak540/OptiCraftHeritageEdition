#!/usr/bin/env python3
"""protocol_probe.py -- login/chunk stream logger for the OptiCraft wire protocol.

Connects, handshakes and logs exactly like the game client does (the wire
protocol this port speaks: streamed packets with no length prefix,
big-endian primitives, short-count + UTF-16BE strings, protocol version 8),
then logs every packet the server sends, with timestamps.

Why this exists: the 3DS deferred-chunk pipeline (rolled back 2026-09-29,
see src/3ds/tuning/DsWorldTuning.h) streamed fine on vanilla servers but
never showed chunks on CraftBukkit. The deferred design parks arrivals
beyond a 7x7 window (PLATFORM_CHUNK_UNLOAD_RADIUS 3) in a 256-entry /
3 MiB compressed map, so the failure has to live in the shape of the burst:
arrival order versus the teleport, view distance, includeInitialize flags,
or total bytes. This probe measures exactly those, so a vanilla and a
CraftBukkit capture can be diffed against the pipeline's assumptions.

Read-only from the server's perspective: it only ever sends the login
sequence, keepalive echoes and one position echo after the teleport, then
disconnects. No movement, no block interaction, no chat.

Usage:
    python scripts/protocol_probe.py <host> <port> [--user Probe]
                                      [--seconds 12] [--out capture.log]

Packet ids and layouts are transcribed from this port's own
Packet.cpp::initPacketMappings and each Packet*::readPacketData -- the ids
below are DECIMAL, matching that table (50 = PreChunk, 51 = MapChunk).
"""

import argparse
import socket
import struct
import sys
import time

# The client sends 29 -- hardcoded at both Packet1Login construction sites
# (NetClientHandler.cpp), the 1.2.5 protocol number this port's wire speaks.
PROTOCOL_VERSION = 29

# The deferred pipeline's knobs this capture gets diffed against
# (DsWorldTuning.h, inert while PLATFORM_MP_DEFERRED_CHUNKS is 0).
KEEP_RADIUS = 3                          # PLATFORM_CHUNK_UNLOAD_RADIUS
MAX_DEFERRED_ENTRIES = 256               # PLATFORM_MP_MAX_DEFERRED_CHUNKS
MAX_DEFERRED_BYTES = 3 * 1024 * 1024     # PLATFORM_MP_COMPRESSED_CHUNK_CACHE_BYTES

CHUNK_LOG_EVERY = 25                     # progress line cadence


class Wire:
    """Buffered reader over a socket with big-endian primitives."""

    def __init__(self, sock):
        self.sock = sock
        self.buf = bytearray()
        self.pos = 0
        self.eof = False

    def _fill(self, want):
        while len(self.buf) - self.pos < want:
            if self.eof:
                raise EOFError("connection closed mid-packet")
            chunk = self.sock.recv(65536)
            if not chunk:
                self.eof = True
                raise EOFError("connection closed by server")
            self.buf.extend(chunk)

    def take(self, n):
        self._fill(n)
        data = bytes(self.buf[self.pos:self.pos + n])
        self.pos += n
        return data

    def compact(self):
        if self.pos == len(self.buf):
            self.buf = bytearray()
            self.pos = 0
        elif self.pos > 65536:
            del self.buf[:self.pos]
            self.pos = 0

    def u8(self):
        return self.take(1)[0]

    def i16(self):
        return struct.unpack(">h", self.take(2))[0]

    def u16(self):
        return struct.unpack(">H", self.take(2))[0]

    def i32(self):
        return struct.unpack(">i", self.take(4))[0]

    def i64(self):
        return struct.unpack(">q", self.take(8))[0]

    def f32(self):
        return struct.unpack(">f", self.take(4))[0]

    def f64(self):
        return struct.unpack(">d", self.take(8))[0]

    def string(self):
        # Packet::readString: short char count + UTF-16BE code units.
        count = self.u16()
        units = struct.unpack(">%dH" % count, self.take(2 * count))
        return "".join(chr(u) for u in units)

    def item_stack(self):
        # Packet::readItemStack: short id; if >= 0, byte count + short
        # damage follow.
        item_id = self.i16()
        if item_id < 0:
            return None
        return (item_id, self.u8(), self.i16())

    def watchables(self):
        # DataWatcher::readWatchableObjects: tag = type << 5 | id, until
        # 0x7F. Types: 0 byte, 1 short, 2 int, 3 float, 4 string.
        count = 0
        while True:
            tag = self.u8()
            if tag == 0x7F:
                return count
            kind = tag >> 5
            if kind == 0:
                self.u8()
            elif kind == 1:
                self.i16()
            elif kind == 2:
                self.i32()
            elif kind == 3:
                self.f32()
            elif kind == 4:
                self.string()
            else:
                raise ValueError("unknown watchable type %d" % kind)
            count += 1


class NeedMore(Exception):
    """Raised by BufWire when the fed buffer cannot satisfy a read yet."""


class BufWire(Wire):
    """A socket-less Wire over a growable buffer: the passive mode
    protocol_relay uses to parse a forwarding stream. Rewind `pos` to the
    last packet boundary on NeedMore and feed more bytes."""

    def __init__(self):
        self.sock = None
        self.buf = bytearray()
        self.pos = 0
        self.eof = False

    def feed(self, data):
        self.buf.extend(data)

    def _fill(self, want):
        if len(self.buf) - self.pos < want:
            raise NeedMore()


def pack_string(text):
    return struct.pack(">H", len(text)) + text.encode("utf-16-be")


def send_handshake(sock, user):
    # Packet2Handshake::writePacketData: the username, nothing else.
    sock.sendall(b"\x02" + pack_string(user))


def send_login(sock, user, protocol):
    # Packet1Login::writePacketData (client -> server): protocol version,
    # username, terrain type (empty), server mode, dimension, difficulty,
    # world height, max players.
    sock.sendall(b"\x01" +
                  struct.pack(">i", protocol) + pack_string(user) +
                  pack_string("") +
                  struct.pack(">ii", 0, 0) + b"\x00\x00\x00")


def send_keepalive(sock, value):
    sock.sendall(b"\x00" + struct.pack(">i", value))


def send_chat(sock, message):
    # Packet3Chat::writePacketData: the message string.
    sock.sendall(b"\x03" + pack_string(message))


def send_flying(sock):
    # Packet10Flying::writePacketData: just onGround. The real client's idle
    # stream is mostly these -- 321 of them against 4 Packet13s in the relay
    # capture of a working session.
    sock.sendall(b"\x0a\x01")


def send_position(sock, x, y, stance, z):
    # Packet13PlayerLookMove (client -> server): the position echo after a
    # teleport, so server plugins that wait for the client to acknowledge
    # can finish their login pipeline. Yaw/pitch zero, on the ground.
    sock.sendall(b"\x0d" + struct.pack(">dddd", x, y, stance, z) +
                 struct.pack(">ff", 0.0, 0.0) + b"\x01")


class Capture:
    def __init__(self, out_file):
        self.start = time.monotonic()
        self.out = out_file
        self.counts = {}
        self.offset = 0          # cumulative stream bytes consumed
        self.verbose = False
        self.map_chunks = {}      # (x, z) -> total compressed bytes
        self.map_chunk_init = {}  # (x, z) -> [includeInitialize flags seen]
        self.prechunks = {}       # (x, z) -> last mode (True = load)
        self.first_chunk_at = None
        self.first_prechunk_at = None
        self.teleport_at = None
        self.teleport_pos = None
        self.total_payload = 0
        self.chunk_packets = 0
        self.kick_reason = None
        self.logged_lines = 0

    def log(self, text):
        line = "[%7.3fs] %s" % (time.monotonic() - self.start, text)
        print(line)
        if self.out is not None:
            self.out.write(line + "\n")
            self.out.flush()

    def record(self, pid):
        self.counts[pid] = self.counts.get(pid, 0) + 1

    def chebyshev_from_teleport(self, chunk_x, chunk_z):
        if self.teleport_pos is None:
            return None
        px, pz = self.teleport_pos
        return max(abs(chunk_x - int(px // 16)), abs(chunk_z - int(pz // 16)))


def parse_packet(wire, cap, sock, user, state):
    """Read one packet off the stream. Raises on desync."""
    packet_start = wire.pos
    stream_start = cap.offset
    pid = wire.u8()
    cap.record(pid)

    if pid == 0:  # KeepAlive: echo it so the server keeps us connected.
        value = wire.i32()
        if sock is not None:
            send_keepalive(sock, value)

    elif pid == 1:  # Login response
        proto = wire.i32()
        server_user = wire.string()
        terrain = wire.string()
        server_mode = wire.i32()
        dimension = wire.i32()
        difficulty = wire.u8()
        world_height = wire.u8()
        max_players = wire.u8()
        cap.log("LOGIN proto=%d user=%r terrain=%r mode=%d dim=%d diff=%d "
                "h=%d max=%d" % (proto, server_user, terrain, server_mode,
                                 dimension, difficulty, world_height,
                                 max_players))

    elif pid == 2:  # Handshake reply
        cap.log("Handshake reply %r" % wire.string())
        if sock is not None and not state.get("login_sent"):
            send_login(sock, user, state["protocol"])
            state["login_sent"] = True
            cap.log("-> Login (protocol %d, user %r)" % (state["protocol"], user))

    elif pid == 3:  # Chat
        message = wire.string()
        cap.log("Chat %r" % (message[:120],))

    elif pid == 4:  # UpdateTime
        wire.i64()

    elif pid == 5:  # PlayerInventory
        wire.i32(); wire.i16(); wire.i16(); wire.i16()

    elif pid == 6:  # SpawnPosition
        x, y, z = wire.i32(), wire.i32(), wire.i32()
        cap.log("SpawnPosition (%d, %d, %d)" % (x, y, z))

    elif pid == 8:  # UpdateHealth
        wire.i16(); wire.i16(); wire.f32()

    elif pid == 9:  # Respawn
        wire.i32(); wire.u8(); wire.u8(); wire.i16(); wire.string()

    elif pid == 13:  # PlayerLookMove -- the teleport.
        x = wire.f64(); y = wire.f64(); stance = wire.f64(); z = wire.f64()
        yaw = wire.f32(); pitch = wire.f32(); wire.u8()
        if cap.teleport_at is None:
            cap.teleport_at = time.monotonic() - cap.start
            cap.teleport_pos = (x, z)
            cap.log("TELEPORT -> (%.2f, %.2f, %.2f) stance=%.2f yaw=%.1f "
                    "pitch=%.1f" % (x, y, z, stance, yaw, pitch))
            if sock is not None:
                # Passive mode (the relay) records the teleport without
                # echoing it -- the real client behind the relay speaks for
                # itself. The 1.2.5 server writes (eye, feet) in the two
                # slots the port's reader names y and stance; the real
                # client echoes them swapped, (feet, eye) -- mirror that or
                # every movement packet reads as invalid to the server.
                feet, eye = stance, y
                send_position(sock, x, feet, eye, z)
                state["player_pos"] = (x, feet, eye, z)
                # One second after the teleport the AuthMe session is ready;
                # a registration here unblocks this server's chunk-data gate
                # (it holds MapChunks for unauthenticated players -- see the
                # 0x33 counts of an unregistered capture).
                if state.get("register_pass") is not None:
                    state["register_at"] = time.monotonic() + 1.0
                if state.get("say_queue"):
                    base = time.monotonic() + 2.0
                    state["say_schedule"] = [(base + i * 3.0, m)
                                             for i, m in enumerate(state["say_queue"])]
                cap.log("-> position echo")

    elif pid == 17:  # Sleep
        wire.i32(); wire.u8(); wire.i32(); wire.u8(); wire.i32()

    elif pid == 18:  # Animation
        wire.i32(); wire.u8()

    elif pid == 20:  # NamedEntitySpawn
        entity_id = wire.i32()
        name = wire.string()
        x = wire.i32(); y = wire.i32(); z = wire.i32()
        wire.u8(); wire.u8(); wire.i16()
        cap.log("NamedEntitySpawn id=%d name=%r at (%d,%d,%d)"
                % (entity_id, name, x, y, z))

    elif pid == 21:  # PickupSpawn
        wire.i32(); wire.i16(); wire.u8(); wire.i16()
        wire.i32(); wire.i32(); wire.i32()
        wire.u8(); wire.u8(); wire.u8()

    elif pid == 22:  # Collect
        wire.i32(); wire.i32()

    elif pid == 23:  # VehicleSpawn
        wire.i32(); wire.u8()
        wire.i32(); wire.i32(); wire.i32()
        thrower = wire.i32()
        if thrower > 0:
            wire.i16(); wire.i16(); wire.i16()

    elif pid == 24:  # MobSpawn
        wire.i32(); wire.u8()
        wire.i32(); wire.i32(); wire.i32()
        wire.u8(); wire.u8(); wire.u8()
        wire.watchables()

    elif pid == 25:  # EntityPainting
        wire.i32(); wire.string()
        wire.i32(); wire.i32(); wire.i32(); wire.i32()

    elif pid == 26:  # EntityExpOrb
        wire.i32(); wire.i32(); wire.i32(); wire.i32(); wire.i16()

    elif pid == 28:  # EntityVelocity
        wire.i32(); wire.i16(); wire.i16(); wire.i16()

    elif pid == 29:  # DestroyEntity
        wire.i32()

    elif pid == 30:  # Entity
        wire.i32()

    elif pid == 31:  # RelEntityMove
        wire.i32(); wire.u8(); wire.u8(); wire.u8()

    elif pid == 32:  # EntityLook
        wire.i32(); wire.u8(); wire.u8()

    elif pid == 33:  # RelEntityMoveLook
        wire.i32(); wire.u8(); wire.u8(); wire.u8(); wire.u8(); wire.u8()

    elif pid == 34:  # EntityTeleport
        wire.i32(); wire.i32(); wire.i32(); wire.i32(); wire.u8(); wire.u8()

    elif pid == 35:  # EntityHeadRotation
        wire.i32(); wire.u8()

    elif pid == 38:  # EntityStatus
        wire.i32(); wire.u8()

    elif pid == 39:  # AttachEntity
        wire.i32(); wire.i32()

    elif pid == 40:  # EntityMetadata
        wire.i32(); wire.watchables()

    elif pid == 41:  # EntityEffect
        wire.i32(); wire.u8(); wire.u8(); wire.i16()

    elif pid == 42:  # RemoveEntityEffect
        wire.i32(); wire.u8()

    elif pid == 43:  # Experience
        wire.f32(); wire.i16(); wire.i16()

    elif pid == 50:  # PreChunk
        x = wire.i32()
        z = wire.i32()
        mode = wire.u8()
        cap.prechunks[(x, z)] = mode != 0
        if cap.first_prechunk_at is None:
            cap.first_prechunk_at = time.monotonic() - cap.start
            cap.log("first PreChunk chunk=(%d,%d) mode=%s" % (x, z, mode != 0))

    elif pid == 51:  # MapChunk -- the payload the deferred pipeline parks.
        x = wire.i32()
        z = wire.i32()
        include_init = wire.u8() != 0
        primary = wire.u16()   # yChMin: the primary section bitmask
        secondary = wire.u16()  # yChMax: the add bitmask
        length = wire.i32()   # compressed payload length
        wire.i32()            # field_48178_h
        wire.take(length)
        cap.chunk_packets += 1
        cap.total_payload += length
        cap.map_chunks[(x, z)] = cap.map_chunks.get((x, z), 0) + length
        cap.map_chunk_init.setdefault((x, z), []).append(include_init)
        if cap.first_chunk_at is None:
            cap.first_chunk_at = time.monotonic() - cap.start
            cap.log("first MapChunk chunk=(%d,%d) init=%s primary=0x%04x "
                    "len=%d" % (x, z, include_init, primary, length))
        elif cap.chunk_packets % CHUNK_LOG_EVERY == 0:
            d = cap.chebyshev_from_teleport(x, z)
            cap.log("... %d MapChunks (last: (%d,%d) init=%s len=%d, "
                    "dist=%s), total %.2f MiB" %
                    (cap.chunk_packets, x, z, include_init, length,
                     "?" if d is None else d, cap.total_payload / 1048576.0))

    elif pid == 52:  # MultiBlockChange
        wire.i32(); wire.i32()
        wire.u16()
        payload_len = wire.i32()
        wire.take(payload_len)

    elif pid == 53:  # BlockChange
        wire.i32(); wire.u8(); wire.i32(); wire.u8(); wire.u8()

    elif pid == 54:  # PlayNoteBlock
        wire.i32(); wire.i16(); wire.i32(); wire.u8(); wire.u8()

    elif pid == 60:  # Explosion
        wire.f64(); wire.f64(); wire.f64(); wire.f32()
        count = wire.i32()
        wire.take(3 * count)

    elif pid == 61:  # DoorChange
        wire.i32(); wire.i32(); wire.u8(); wire.i32(); wire.i32()

    elif pid == 70:  # Bed
        wire.u8(); wire.u8()

    elif pid == 71:  # Weather
        wire.i32(); wire.u8(); wire.i32(); wire.i32(); wire.i32()

    elif pid == 100:  # OpenWindow
        wire.u8(); wire.u8(); wire.string(); wire.u8()

    elif pid == 101:  # CloseWindow
        wire.u8()

    elif pid == 103:  # SetSlot
        wire.u8(); wire.i16(); wire.item_stack()

    elif pid == 104:  # WindowItems
        wire.u8()
        count = wire.i16()
        for _ in range(count):
            wire.item_stack()

    elif pid == 105:  # UpdateProgressbar
        wire.u8(); wire.i16(); wire.i16()

    elif pid == 106:  # Transaction
        wire.u8(); wire.i16(); wire.u8()

    elif pid == 107:  # CreativeSetSlot
        wire.i16(); wire.item_stack()

    elif pid == 130:  # UpdateSign
        wire.i32(); wire.i16(); wire.i32()
        for _ in range(4):
            wire.string()

    elif pid == 131:  # MapData
        wire.i16(); wire.i16()
        length = wire.u8()
        wire.take(length)

    elif pid == 132:  # TileEntityData
        wire.i32(); wire.i16(); wire.i32(); wire.u8()
        wire.i32(); wire.i32(); wire.i32()

    elif pid == 200:  # Statistic
        wire.i32(); wire.u8()

    elif pid == 201:  # PlayerInfo
        name = wire.string()
        connected = wire.u8()
        ping = wire.i16()
        cap.log("PlayerInfo %r %s ping=%d" % (name, "join" if connected else "leave", ping))

    elif pid == 202:  # PlayerAbilities
        wire.u8(); wire.u8(); wire.u8(); wire.u8()

    elif pid == 250:  # CustomPayload
        channel = wire.string()
        length = wire.i16()
        wire.take(max(length, 0))
        cap.log("CustomPayload channel=%r len=%d" % (channel, length))

    elif pid == 255:  # KickDisconnect
        reason = wire.string()
        cap.offset += wire.pos - packet_start
        cap.kick_reason = reason
        cap.log("KICKED: %r" % reason)
        return False

    else:
        raise ValueError("unknown packet id %d (0x%02x) -- stream walk "
                         "cannot continue" % (pid, pid))

    consumed = wire.pos - packet_start
    cap.offset += consumed
    if cap.verbose:
        cap.log("  id=%-3d len=%-6d off=%d" % (pid, consumed, stream_start))
    return True


def main():
    parser = argparse.ArgumentParser(
        description="Log the login/chunk packet sequence of one server.")
    parser.add_argument("host")
    parser.add_argument("port", type=int)
    parser.add_argument("--user", default="Probe")
    parser.add_argument("--protocol", type=int, default=PROTOCOL_VERSION,
                        help="login protocol version (default 29, what the "
                             "game client sends)")
    parser.add_argument("--seconds", type=float, default=12.0,
                        help="capture duration (default 12)")
    parser.add_argument("--out", default=None,
                        help="also write the log to this file")
    parser.add_argument("--register", default=None, metavar="PASSWORD",
                        help="send /register PASSWORD PASSWORD one second "
                             "after the teleport (AuthMe-style servers)")
    parser.add_argument("--say", action="append", default=[],
                        help="send this chat message after the teleport; "
                             "repeatable, each fires 3s after the previous "
                             "(first at +2s)")
    parser.add_argument("--verbose", action="store_true",
                        help="log every packet with id/length/offset")
    args = parser.parse_args()

    out_file = open(args.out, "w", encoding="utf-8") if args.out else None
    cap = Capture(out_file)
    cap.verbose = args.verbose

    sock = socket.create_connection((args.host, args.port), timeout=10.0)
    sock.settimeout(1.0)
    cap.log("connected to %s:%d as %r" % (args.host, args.port, args.user))
    send_handshake(sock, args.user)
    cap.log("-> Handshake user=%r" % args.user)

    state = {"login_sent": False, "protocol": args.protocol}
    state["register_pass"] = args.register
    state["say_queue"] = args.say
    # The game client streams its position from the first tick after login
    # (EntityClientPlayerMP: Packet13 with feet-y and eye-stance); without
    # that traffic CraftBukkit never completes the login phase. Start from
    # the client's spawn guess; the teleport replaces it.
    state["player_pos"] = (0.5, 64.0, 65.62, 0.5)
    wire = Wire(sock)
    deadline = time.monotonic() + args.seconds
    last_heartbeat = time.monotonic()
    stopped = "timeout"
    try:
        while time.monotonic() < deadline:
            if state.get("login_sent"):
                now = time.monotonic()
                if state.get("register_at") is not None and now >= state["register_at"]:
                    password = state["register_pass"]
                    send_chat(sock, "/register %s %s" % (password, password))
                    cap.log("-> /register (AuthMe-style gate)")
                    state["register_at"] = None
                if state.get("say_schedule"):
                    due_now = time.monotonic()
                    due = [m for t, m in state["say_schedule"] if t <= due_now]
                    if due:
                        state["say_schedule"] = [x for x in state["say_schedule"]
                                                 if x[0] > due_now]
                        for message in due:
                            send_chat(sock, message)
                            cap.log("-> chat %r" % message)
                if now - last_heartbeat >= 0.3:
                    step = state.get("heartbeat_count", 0) + 1
                    state["heartbeat_count"] = step
                    x, y, stance, z = state["player_pos"]
                    # Mirror the real client's idle stream (relay capture):
                    # Packet10Flying at ~3/s, a full Packet13 only every
                    # ~2s. No falling: hold the ground the server put us
                    # on -- sinking below it reads as invalid movement and
                    # the 1.2.5 chunk pipeline follows only valid players.
                    if step % 7 == 0:
                        send_position(sock, x, y, stance, z)
                    else:
                        send_flying(sock)
                    last_heartbeat = now
            try:
                if not parse_packet(wire, cap, sock, args.user, state):
                    stopped = "kicked"
                    break
                wire.compact()
            except EOFError as exc:
                cap.log("stream end: %s" % exc)
                stopped = "closed"
                break
            except socket.timeout:
                continue
            except ValueError as exc:
                cap.log("WALK STOPPED: %s" % exc)
                stopped = "desync"
                break
    finally:
        elapsed = time.monotonic() - cap.start
        sock.close()

    summarize(cap, elapsed, stopped)
    if out_file is not None:
        cap.log("(full log also in %s)" % args.out)
        out_file.close()


def summarize(cap, elapsed, stopped):
    """Print the diffable summary of one capture (protocol_relay reuses this)."""
    unique_columns = len(cap.map_chunks)
    with_base = sum(1 for flags in cap.map_chunk_init.values() if any(flags))
    delta_only = unique_columns - with_base
    cap.log("")
    cap.log("==== summary -- %.1fs captured, stop=%s ====" % (elapsed, stopped))
    if cap.teleport_at is not None:
        chunks_first = (cap.first_chunk_at is not None and
                       cap.first_chunk_at < cap.teleport_at)
        cap.log("teleport at t=%.3fs -> (%.1f, %.1f); chunk burst BEFORE it: %s"
                % (cap.teleport_at, cap.teleport_pos[0], cap.teleport_pos[1],
                   "YES" if chunks_first else "no"))
    elif cap.first_chunk_at is not None:
        cap.log("NO TELEPORT SEEN; first chunk at t=%.3fs" % cap.first_chunk_at)
    cap.log("packet counts: " + ", ".join(
        "%d:%d" % (pid, count) for pid, count in sorted(cap.counts.items())))
    cap.log("MapChunk packets: %d over %d unique columns, %.2f MiB compressed"
            % (cap.chunk_packets, unique_columns, cap.total_payload / 1048576.0))
    cap.log("  columns with includeInitialize base: %d, delta-only: %d"
            % (with_base, delta_only))
    cap.log("PreChunk packets: %d unique columns (%d load, %d unload)"
            % (len(cap.prechunks),
               sum(1 for m in cap.prechunks.values() if m),
               sum(1 for m in cap.prechunks.values() if not m)))
    if cap.teleport_pos is not None and cap.map_chunks:
        buckets = [("<=%d (keep window)" % KEEP_RADIUS, 0), ("4-5", 0),
                   ("6-7", 0), ("8-10", 0), (">10", 0)]
        for (x, z) in cap.map_chunks:
            d = cap.chebyshev_from_teleport(x, z)
            if d is None:
                continue
            if d <= KEEP_RADIUS:
                buckets[0] = (buckets[0][0], buckets[0][1] + 1)
            elif d <= 5:
                buckets[1] = (buckets[1][0], buckets[1][1] + 1)
            elif d <= 7:
                buckets[2] = (buckets[2][0], buckets[2][1] + 1)
            elif d <= 10:
                buckets[3] = (buckets[3][0], buckets[3][1] + 1)
            else:
                buckets[4] = (buckets[4][0], buckets[4][1] + 1)
        cap.log("chunk distance (Chebyshev) from teleport: " +
                ", ".join("%s=%d" % b for b in buckets))
        over_entries = unique_columns > MAX_DEFERRED_ENTRIES
        over_bytes = cap.total_payload > MAX_DEFERRED_BYTES
        cap.log("deferred map pressure: %d entries (cap %d), %.2f MiB "
                "(cap %.2f MiB)%s" %
                (unique_columns, MAX_DEFERRED_ENTRIES,
                 cap.total_payload / 1048576.0,
                 MAX_DEFERRED_BYTES / 1048576.0,
                 "  <-- OVER BUDGET" if (over_entries or over_bytes) else ""))
    if cap.kick_reason is not None:
        cap.log("kicked: %r" % cap.kick_reason)
    cap.log("(keep radius %d, entry cap %d, byte cap %d -- the deferred "
            "pipeline's assumptions)" % (KEEP_RADIUS, MAX_DEFERRED_ENTRIES,
                                        MAX_DEFERRED_BYTES))


if __name__ == "__main__":
    main()
