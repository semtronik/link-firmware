"""Send one JSON command to a Link device and print its replies.

Usage:
    python tools/send_cmd.py COM3 '{"cmd":"get_version"}'
    python tools/send_cmd.py COM3 '{"cmd":"tx","value":"48656C6C6F"}' --timeout 2

Replies are the JSON lines carrying a "type" field. Logs share the port
and are hidden unless --all is given.

Exit code: 0 if an "ack" came back, 1 on "err" or no reply, 2 if the
command or the port could not be used.
"""

import argparse
import json
import sys
import time

import serial


def parse_command(text):
    try:
        cmd = json.loads(text)
    except json.JSONDecodeError as e:
        hint = ""
        if '"' not in text:
            # Windows PowerShell 5 strips the inner double quotes when
            # passing an argument to a native program.
            hint = (
                "\nThe command has no double quotes left: the shell probably "
                "stripped them. Escape them (\\\") or use PowerShell 7."
            )
        sys.exit(f"invalid JSON: {e}{hint}")

    if not isinstance(cmd, dict) or not isinstance(cmd.get("cmd"), str):
        sys.exit('the command must be a JSON object with a "cmd" string')

    return cmd


def open_port(name):
    port = serial.Serial()
    port.port = name
    port.timeout = 0.05
    # On the ESP32-S3 native USB, DTR and RTS drive reset and boot mode.
    # Asserting them on open would reboot the board or park it in the
    # bootloader.
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


def main():
    parser = argparse.ArgumentParser(
        description="Send one JSON command to a Link device."
    )
    parser.add_argument("port", help="serial port, e.g. COM3")
    parser.add_argument("command", help='JSON command, e.g. {"cmd":"get_version"}')
    parser.add_argument(
        "--timeout", type=float, default=1.0,
        help="seconds to listen for replies (default: 1)",
    )
    parser.add_argument(
        "--all", action="store_true",
        help="also print log lines, not only the JSON replies",
    )
    args = parser.parse_args()

    cmd = parse_command(args.command)
    line = json.dumps(cmd, separators=(",", ":")) + "\n"

    got_ack = False
    got_reply = False

    with open_port(args.port) as port:
        port.reset_input_buffer()
        port.write(line.encode("utf-8"))
        port.flush()

        buffer = b""
        deadline = time.monotonic() + args.timeout

        while time.monotonic() < deadline:
            buffer += port.read(4096)

            while b"\n" in buffer:
                raw, buffer = buffer.split(b"\n", 1)
                text = raw.decode("utf-8", errors="replace").strip()
                if not text:
                    continue

                reply = None
                if text.startswith("{"):
                    try:
                        reply = json.loads(text)
                    except json.JSONDecodeError:
                        pass

                if isinstance(reply, dict) and "type" in reply:
                    print(text)
                    got_reply = True
                    if reply["type"] == "ack":
                        got_ack = True
                elif args.all:
                    print(text)

    if not got_reply:
        print(f"no reply within {args.timeout} s", file=sys.stderr)

    sys.exit(0 if got_ack else 1)


if __name__ == "__main__":
    main()
