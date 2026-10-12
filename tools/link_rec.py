"""Read the recordings off a Link-LR device, and set its radio profile.

Usage:
    python tools/link_rec.py COM3 list
    python tools/link_rec.py COM3 get 12            # one session
    python tools/link_rec.py COM3 get current       # the running one
    python tools/link_rec.py COM3 get all -o elsewhere/
    python tools/link_rec.py COM3 follow            # live lines, Ctrl+C to stop
    python tools/link_rec.py COM3 erase
    python tools/link_rec.py COM3 time [--set]      # clock against the PC's
    python tools/link_rec.py COM3 profile           # the selected profile
    python tools/link_rec.py COM3 profile --sf 9 --gain boosted
    python tools/link_rec.py COM3 factory-reset     # as it left the bench

A session is saved in recordings/ at the repo root, as s<id>_<start>.jsonl,
the lines exactly as the device recorded them.
Format: link/protocol/v1/recording.md.

Exit code: 0 on success, 1 if the device answered with an error or not
at all, 2 if the port could not be used.
"""

import argparse
import json
import os
import sys
import time
from datetime import datetime

import serial

RECORDINGS = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          os.pardir, "recordings")

RECORD_TYPES = {"session", "end", "rx", "tx", "gap", "lost"}

# Silence after which a streamed reply is taken as cut short.
STALL_S = 5.0


def open_port(name):
    port = serial.Serial()
    port.port = name
    port.timeout = 0.05
    # On the ESP32-S3 native USB, DTR and RTS drive reset and boot mode.
    port.dtr = False
    port.rts = False
    try:
        port.open()
    except serial.SerialException as e:
        print(
            f"cannot open {name}: {e}\n"
            "Is it held by the IDF monitor, a serial terminal or Link Studio?",
            file=sys.stderr,
        )
        sys.exit(2)
    return port


class Link:
    def __init__(self, port):
        self.port = port
        self.buffer = b""

    def send(self, cmd):
        line = json.dumps(cmd, separators=(",", ":")) + "\n"
        self.port.write(line.encode("utf-8"))
        self.port.flush()

    def lines(self, stall_s=STALL_S):
        """JSON lines as (text, object); log lines are skipped."""
        last = time.monotonic()
        while True:
            chunk = self.port.read(4096)
            if chunk:
                self.buffer += chunk
                last = time.monotonic()
            elif stall_s is not None and time.monotonic() - last > stall_s:
                return

            while b"\n" in self.buffer:
                raw, self.buffer = self.buffer.split(b"\n", 1)
                text = raw.decode("utf-8", errors="replace").strip()
                if not text.startswith("{"):
                    continue
                try:
                    obj = json.loads(text)
                except json.JSONDecodeError:
                    continue
                if isinstance(obj, dict) and "type" in obj:
                    yield text, obj

    def stream(self, cmd):
        """The lines between the begin and end acks of a streamed command.

        Live lines that arrive before begin are dropped: the stream has
        them too.
        """
        self.send(cmd)
        name = cmd["cmd"]
        inside = False
        got = []
        for text, obj in self.lines():
            if obj["type"] == "err" and obj.get("cmd") == name:
                sys.exit(f"{name}: {obj.get('msg')}")
            if obj["type"] == "ack" and obj.get("cmd") == name:
                if obj.get("phase") == "begin":
                    inside = True
                elif obj.get("phase") == "end":
                    if obj.get("count") != len(got):
                        print(
                            f"warning: {len(got)} lines received, the device "
                            f"announced {obj.get('count')}",
                            file=sys.stderr,
                        )
                    return got
                continue
            if inside:
                got.append((text, obj))
        sys.exit(f"{name}: no complete reply within {STALL_S} s of silence")


def local(ts_ms):
    if ts_ms is None:
        return "-"
    return datetime.fromtimestamp(ts_ms / 1000).strftime("%Y-%m-%d %H:%M:%S")


def duration(info):
    if "ts" not in info or "end_ts" not in info:
        return "-"
    s = int((info["end_ts"] - info["ts"]) / 1000)
    return f"{s // 3600}:{s % 3600 // 60:02d}:{s % 60:02d}"


def sessions(link):
    return [obj for _, obj in link.stream({"cmd": "get_sessions"})]


def cmd_list(link, args):
    infos = sessions(link)
    if not infos:
        print("no recording on the device")
        return
    print(f"{'id':>6}  {'start':19}  {'duration':>9}  {'closed by':10}  "
          f"{'bytes':>8}")
    for info in infos:
        reason = "(running)" if info.get("current") else info.get("reason", "-")
        flag = "  start lost to rotation" if info.get("truncated") else ""
        print(f"{info['id']:>6}  {local(info.get('ts')):19}  "
              f"{duration(info):>9}  {reason:10}  {info.get('bytes', 0):>8}"
              f"{flag}")


def save(link, info, out_dir):
    lines = link.stream({"cmd": "get_session", "id": info["id"]})
    stamp = "unknown"
    if "ts" in info:
        stamp = datetime.fromtimestamp(info["ts"] / 1000).strftime(
            "%Y%m%d-%H%M%S")
    path = os.path.join(out_dir, f"s{info['id']}_{stamp}.jsonl")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        for text, _ in lines:
            f.write(text + "\n")
    print(f"{path}: {len(lines)} lines")


def cmd_get(link, args):
    infos = sessions(link)
    if args.which == "all":
        wanted = infos
    elif args.which == "current":
        wanted = [i for i in infos if i.get("current")]
    else:
        wanted = [i for i in infos if str(i["id"]) == args.which]
    if not wanted:
        sys.exit(f"no session matching {args.which!r}")

    os.makedirs(args.out, exist_ok=True)
    for info in wanted:
        save(link, info, args.out)


def cmd_follow(link, args):
    out = open(args.out, "a", encoding="utf-8", newline="\n") if args.out else None
    try:
        for text, obj in link.lines(stall_s=None):
            if obj["type"] in RECORD_TYPES:
                print(text, flush=True)
                if out:
                    out.write(text + "\n")
                    out.flush()
    except KeyboardInterrupt:
        pass
    finally:
        if out:
            out.close()


def cmd_erase(link, args):
    if not args.yes:
        answer = input("Delete every recording on the device? [y/N] ")
        if answer.strip().lower() not in ("y", "yes", "o", "oui"):
            print("nothing deleted")
            return
    link.send({"cmd": "erase_sessions"})
    for _, obj in link.lines():
        if obj.get("cmd") != "erase_sessions":
            continue
        if obj["type"] == "ack":
            print("erased")
            return
        sys.exit(f"erase_sessions: {obj.get('msg')}")
    sys.exit("erase_sessions: no reply")


def cmd_factory_reset(link, args):
    if not args.yes:
        answer = input("Erase every setting, the clock, the recordings and "
                       "the event log, then restart? [y/N] ")
        if answer.strip().lower() not in ("y", "yes", "o", "oui"):
            print("nothing erased")
            return
    link.send({"cmd": "factory_reset", "confirm": "erase"})
    reply(link, "factory_reset")
    print("erased; the device is restarting")


def reply(link, name, wanted_type="ack"):
    """The reply to one command, or exit on an error or silence."""
    for _, obj in link.lines():
        if obj["type"] == "err" and obj.get("cmd") == name:
            sys.exit(f"{name}: {obj.get('msg')}")
        if obj["type"] == wanted_type and (
                wanted_type != "ack" or obj.get("cmd") == name):
            return obj
    sys.exit(f"{name}: no reply")


def cmd_time(link, args):
    if args.set:
        # The device takes whole seconds: sent on the PC's second edge,
        # the error is the transfer time, not up to a second.
        now = time.time()
        time.sleep(1 - (now % 1))
        utc = int(round(time.time()))
        offset_min = int(datetime.now().astimezone().utcoffset()
                         .total_seconds() // 60)
        link.send({"cmd": "set_time", "utc": utc, "offset_min": offset_min})
        reply(link, "set_time")
        print(f"set to {local(utc * 1000)} (UTC{offset_min / 60:+g}); "
              "the running session was closed")

    link.send({"cmd": "get_time"})
    pc = time.time()
    obj = reply(link, "get_time", "time")
    if not obj.get("valid"):
        print("device clock: not set")
        return
    # Whole seconds on the device: the difference is good to about 1 s.
    print(f"device: {local(obj['utc'] * 1000)}  "
          f"PC: {local(pc * 1000)}  "
          f"difference: {obj['utc'] - pc:+.0f} s")


PROFILE_FIELDS = ("region", "freq_hz", "sf", "bw_hz", "cr", "power_dbm",
                  "preamble", "crc_on", "iq_inverted", "sync_word", "rx_gain")


def show_profile(p):
    print(f"{p.get('name')}")
    for key in PROFILE_FIELDS:
        value = p.get(key)
        if key == "sync_word" and isinstance(value, int):
            value = f"0x{value:02X} ({value})"
        print(f"  {key:12} {value}")


def cmd_profile(link, args):
    fields = {}
    if args.region is not None:
        fields["region"] = args.region
    if args.freq is not None:
        fields["freq_hz"] = round(args.freq * 1e6)
    if args.sf is not None:
        fields["sf"] = args.sf
    if args.bw is not None:
        fields["bw_hz"] = round(args.bw * 1000)
    if args.cr is not None:
        fields["cr"] = args.cr
    if args.power is not None:
        fields["power_dbm"] = args.power
    if args.preamble is not None:
        fields["preamble"] = args.preamble
    if args.crc is not None:
        fields["crc_on"] = args.crc == "on"
    if args.iq is not None:
        fields["iq_inverted"] = args.iq == "inverted"
    if args.gain is not None:
        fields["rx_gain"] = args.gain
    if args.sync is not None:
        fields["sync_word"] = args.sync

    if fields:
        link.send({"cmd": "set_profile", "profile": fields})
        obj = reply(link, "set_profile")
        print("applied; the running session was closed")
    else:
        link.send({"cmd": "get_profile"})
        obj = reply(link, "get_profile")
    show_profile(obj.get("profile", {}))


def main():
    parser = argparse.ArgumentParser(
        description="Read the recordings off a Link-LR device, and set "
                    "its radio profile.")
    parser.add_argument("port", help="serial port, e.g. COM3")
    sub = parser.add_subparsers(dest="action", required=True)

    sub.add_parser("list", help="list the sessions on the device")

    get = sub.add_parser("get", help="save sessions as .jsonl files")
    get.add_argument("which", help="a session id, 'current' or 'all'")
    get.add_argument("-o", "--out", default=RECORDINGS,
                     help="directory to save into (default: recordings/ at "
                          "the repo root)")

    follow = sub.add_parser("follow", help="print live lines as they come")
    follow.add_argument("-o", "--out", help="also append them to this file")

    erase = sub.add_parser("erase", help="delete every recording")
    erase.add_argument("--yes", action="store_true",
                       help="do not ask for confirmation")

    clock = sub.add_parser("time", help="show the device clock against the PC's")
    clock.add_argument("--set", action="store_true",
                       help="set it to the PC's time and zone first")

    factory = sub.add_parser(
        "factory-reset",
        help="erase settings, clock, recordings and event log, then restart")
    factory.add_argument("--yes", action="store_true",
                         help="do not ask for confirmation")

    prof = sub.add_parser(
        "profile",
        help="show the selected profile; with options, change it",
        description="Fields left out keep their value. Any change closes "
                    "the running session.")
    prof.add_argument("--region", choices=("EU868", "US915"))
    prof.add_argument("--freq", type=float, metavar="MHZ")
    prof.add_argument("--sf", type=int)
    prof.add_argument("--bw", type=float, metavar="KHZ",
                      help="62.5, 125, 250 or 500")
    prof.add_argument("--cr", type=int, metavar="5-8",
                      help="coding rate denominator, 5 for 4/5")
    prof.add_argument("--power", type=int, metavar="DBM")
    prof.add_argument("--preamble", type=int, metavar="SYMBOLS")
    prof.add_argument("--crc", choices=("on", "off"))
    prof.add_argument("--iq", choices=("standard", "inverted"))
    prof.add_argument("--gain", choices=("saving", "boosted"))
    prof.add_argument("--sync", type=lambda v: int(v, 0), metavar="0x12")

    args = parser.parse_args()
    actions = {
        "list": cmd_list,
        "get": cmd_get,
        "follow": cmd_follow,
        "erase": cmd_erase,
        "time": cmd_time,
        "profile": cmd_profile,
        "factory-reset": cmd_factory_reset,
    }

    with open_port(args.port) as port:
        port.reset_input_buffer()
        actions[args.action](Link(port), args)


if __name__ == "__main__":
    main()
