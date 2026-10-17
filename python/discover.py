"""Discover SUXITECH LAN MIC devices without reserving either audio direction."""

import argparse
import socket
import struct
import sys
import time

# Wire format is specified by common/Protocol.h and docs/PROTOCOL.md.
HEADER = struct.Struct("<IBBHIIIIHH")
MAGIC = 0x31534F53
VERSION = 6
PORT = 45678


def discover(seconds, broadcast):
    request = HEADER.pack(MAGIC, VERSION, 1, 0, 0, 0, 0, 48000, 0, 0)
    found = set()
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        sock.bind(("", 0))
        deadline = time.monotonic() + seconds
        next_send = 0.0
        while time.monotonic() < deadline:
            now = time.monotonic()
            if now >= next_send:
                sock.sendto(request, (broadcast, PORT))
                next_send = now + 1.0
            sock.settimeout(max(0.001, min(0.2, deadline - now)))
            try:
                packet, source = sock.recvfrom(509)
            except socket.timeout:
                continue
            if not HEADER.size <= len(packet) <= 508 or source[1] != PORT:
                continue
            magic, version, kind, flags, session, _, _, rate, length, reserved = HEADER.unpack_from(packet)
            if (magic, version, kind, flags, rate, reserved) != (MAGIC, VERSION, 2, 0, 48000, 0):
                continue
            if not session or not 0 < length <= 64 or len(packet) != HEADER.size + length:
                continue
            try:
                identity = packet[HEADER.size:].decode("ascii")
            except UnicodeDecodeError:
                continue
            if not identity.startswith("vst_lan_mic-") or not all(
                char.isalnum() or char in "_-" for char in identity
            ):
                continue
            key = (identity, source[0], session)
            if key not in found:
                found.add(key)
                print(f"{identity}  {source[0]}  48000 Hz  boot={session:08x}", flush=True)
    return len(found)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds", type=float, default=3.0, help="Discovery duration (default: 3)")
    parser.add_argument("--broadcast", default="255.255.255.255", help="Optional interface broadcast address")
    args = parser.parse_args()
    if not 0 < args.seconds <= 3600:
        parser.error("--seconds must be in (0, 3600]")
    try:
        if discover(args.seconds, args.broadcast):
            return 0
    except OSError as error:
        print(f"UDP error: {error}", file=sys.stderr)
        return 2
    print("No devices found. Check Wi-Fi credentials, LAN permissions and AP isolation.", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
