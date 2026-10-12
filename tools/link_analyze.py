"""Summarise Link-LR recordings.

Usage:
    python tools/link_analyze.py recordings/s8_20261004-190828.jsonl
    python tools/link_analyze.py recordings/*.jsonl --csv recordings/frames.csv
    python tools/link_analyze.py recordings/s8.jsonl --id 1:3
    python tools/link_analyze.py recordings/s10.jsonl --plot recordings/nuit.html

Reads the .jsonl files link_rec.py saves; never talks to a device.
Format: link/protocol/v1/recording.md.

Every figure is tagged: [measured] comes straight from the device's
lines, [computed] is derived from them here.
"""

import argparse
import csv
import glob
import json
import math
import statistics
import sys
from datetime import datetime

# SX1261/2 datasheet, table 6-1: SNR below which a frame no longer
# demodulates, per spreading factor.
SNR_LIMIT_DB = {5: -2.5, 6: -5, 7: -7.5, 8: -10, 9: -12.5, 10: -15,
                11: -17.5, 12: -20}

# An interval within this share of a whole number of periods counts as
# that many periods.
PERIOD_TOLERANCE = 0.1
# Below this share of intervals fitting the period, the sender is not
# treated as periodic and nothing is estimated as missed.
PERIODIC_SHARE = 0.8
# Failures closer than this many periods belong to the same episode.
EPISODE_PERIODS = 5
EPISODE_DEFAULT_S = 600
# A frame is perturbed when its in-frame peak stands this far over its own
# signal, or over the noise when that is higher: something else was on the
# channel while it was received.
PERTURB_DB = 10
# Highest SNR seen on our bench whatever the signal: past it, signal - SNR
# no longer gives the noise. Observed, not from the datasheet.
SNR_CEILING_DB = 11


def when(ts_ms, with_date=False):
    if ts_ms is None:
        return "-"
    fmt = "%Y-%m-%d %H:%M:%S" if with_date else "%H:%M:%S"
    return datetime.fromtimestamp(ts_ms / 1000).strftime(fmt)


def span(ms):
    s = int(round(ms / 1000))
    return f"{s // 3600}:{s % 3600 // 60:02d}:{s % 60:02d}"


def duration(ms):
    """As the Stats view writes it: whole seconds, truncated."""
    s = int(ms // 1000)
    if s < 60:
        return f"{s} s"
    if s < 3600:
        return f"{s // 60} min {s % 60:02d} s"
    return f"{s // 3600} h {s % 3600 // 60:02d} min"


def expected_r_minus_s(snr_db):
    """RSSI over the signal alone if only noise is added: 10log(1+N/S)."""
    return 10 * math.log10(1 + 10 ** (-snr_db / 10))


def describe(values, unit, digits=1):
    if not values:
        return "-"
    med = statistics.median(values)
    return (f"median {med:.{digits}f} {unit}, "
            f"range {min(values):.{digits}f} to {max(values):.{digits}f}")


class Session:
    def __init__(self, head):
        self.head = head
        self.id = head["id"]
        self.lines = []

    @property
    def profile(self):
        return self.head.get("profile", {})

    def of(self, kind):
        return [o for o in self.lines if o["type"] == kind]


def load(paths):
    sessions = {}
    for pattern in paths:
        files = glob.glob(pattern) or [pattern]
        for path in files:
            current = None
            with open(path, encoding="utf-8") as f:
                for n, raw in enumerate(f, 1):
                    raw = raw.strip()
                    if not raw:
                        continue
                    try:
                        obj = json.loads(raw)
                    except json.JSONDecodeError:
                        print(f"{path}:{n}: not JSON, skipped", file=sys.stderr)
                        continue
                    if obj.get("type") == "session":
                        current = sessions.get(obj["id"])
                        if current is None:
                            current = sessions[obj["id"]] = Session(obj)
                        # A later part repeats the head: not a line of its own.
                        continue
                    if current is None:
                        print(f"{path}:{n}: line before any session line, "
                              "skipped", file=sys.stderr)
                        continue
                    # Recordings before format cleanup kept these under dbg.
                    for k, v in obj.pop("dbg", {}).items():
                        obj.setdefault(k, v)
                    if obj.get("status") == "crc":
                        obj["status"] = "crc_err"
                    current.lines.append(obj)
    return [sessions[k] for k in sorted(sessions)]


def sender_of(frame, offset, length):
    if length <= 0 or frame.get("status") != "ok" or "data" not in frame:
        return None
    data = bytes.fromhex(frame["data"])
    if len(data) < offset + length:
        return None
    return data[offset:offset + length].hex().upper()


def group_senders(rx, offset, length):
    """Frames by sender id. Failed frames carry no trustworthy id."""
    groups = {}
    for frame in rx:
        groups.setdefault(sender_of(frame, offset, length), []).append(frame)
    return groups


def period_of(frames):
    """Median interval, and whether most intervals are whole periods."""
    ts = [f["ts"] for f in frames if "ts" in f]
    gaps = [b - a for a, b in zip(ts, ts[1:]) if b > a]
    if len(gaps) < 3:
        return None, False
    period = statistics.median(gaps)
    fits = 0
    for g in gaps:
        k = round(g / period)
        if k >= 1 and abs(g - k * period) <= PERIOD_TOLERANCE * period:
            fits += 1
    return period, fits / len(gaps) >= PERIODIC_SHARE


def missed_slots(frames, period):
    """(ts, count) for each gap that held frames never seen."""
    ts = [f["ts"] for f in frames if "ts" in f]
    out = []
    for a, b in zip(ts, ts[1:]):
        k = round((b - a) / period)
        if k >= 2 and abs((b - a) - k * period) <= PERIOD_TOLERANCE * period:
            out.append((a, k - 1))
    return out


# The device's Stats view: margin classes, link_radio.h.
MARGIN_GOOD_DB = 10
MARGIN_FAIR_DB = 3


def share(n, total):
    """As the Stats view writes it: tenths rounded half up, as the C does,
    and never 0 % for a count."""
    if total == 0:
        return f"{n}"
    tenths = (n * 2000 + total) // (2 * total)
    if n > 0 and tenths == 0:
        return f"{n} (<0.1 %)"
    return f"{n} ({tenths // 10}.{tenths % 10} %)"


def peak_over(f):
    """In-frame peak over the frame's own signal, or over its lowest sample
    when no signal was measured, or over the noise when that is higher: a
    signal below the noise would make the noise look like a peak. None when
    nothing was sampled."""
    if not f.get("inst_n"):
        return None
    ref = f.get("signal_dbm", f["inst_min_dbm"])
    if "noise_dbm" in f:
        ref = max(ref, f["noise_dbm"])
    return f["inst_max_dbm"] - ref


def report_perturbed(rx):
    ok = [f for f in rx if f["status"] == "ok"]
    bad = [f for f in rx if f["status"] != "ok"]
    print(f"perturbed, in-frame peak >= {PERTURB_DB} dB over the signal or "
          "the noise, whichever is higher [measured peak; criterion computed]:")
    for name, frames in (("errors", bad), ("received ok", ok)):
        peaks = [peak_over(f) for f in frames]
        hit = sum(1 for x in peaks if x is not None and x >= PERTURB_DB)
        measured = sum(1 for x in peaks if x is not None)
        line = f"    {name:16} {hit} of {measured} had one"
        if measured < len(frames):
            line += f"   ({len(frames) - measured} not sampled)"
        print(line)
    print()


def report_stats(rx, tx, limit, first_ts, last_ts):
    """The same figures as the device's Stats view, for this session. The
    device counts its duration up to now; a file can only reach its last
    line."""
    ok = [f for f in rx if f["status"] == "ok"]
    errors = {k: sum(1 for f in rx if f["status"] == k)
              for k in ("crc_err", "header_err", "incomplete")}
    n_err = sum(errors.values())
    total = len(ok) + n_err

    print("stats, as on the device [measured; classes computed from SNR]:")
    print(f"    duration {duration(last_ts - first_ts) if first_ts else '-'}"
          f"    {len(tx)} sent    {total} received")
    print("    SNR margin:")
    if limit is None:
        print("        unknown spreading factor")
    else:
        margins = [f["snr_db"] - limit for f in ok]
        rows = (
            (f">= {MARGIN_GOOD_DB} dB",
             sum(1 for m in margins if m >= MARGIN_GOOD_DB)),
            (f"{MARGIN_FAIR_DB}-{MARGIN_GOOD_DB} dB",
             sum(1 for m in margins if MARGIN_FAIR_DB <= m < MARGIN_GOOD_DB)),
            (f"< {MARGIN_FAIR_DB} dB",
             sum(1 for m in margins if m < MARGIN_FAIR_DB)),
            ("errors", n_err),
        )
        for name, n in rows:
            print(f"        {name:16} {share(n, total)}")
    print("    errors by type:")
    for name, key in (("CRC", "crc_err"), ("header invalid", "header_err"),
                      ("incomplete", "incomplete")):
        print(f"        {name:16} {share(errors[key], n_err)}")
    print("    sent:")
    for name, key in (("succeeded", "ok"), ("timed out", "timeout"),
                      ("aborted", "aborted")):
        n = sum(1 for t in tx if t.get("result") == key)
        print(f"        {name:16} {share(n, len(tx))}")
    print()


def report_session(s, args, rows):
    p = s.profile
    rx = s.of("rx")
    sf = p.get("sf")
    limit = SNR_LIMIT_DB.get(sf)
    ends = s.of("end")

    first_ts = s.head.get("ts")
    last_ts = max((o.get("ts", 0) for o in s.lines), default=first_ts)
    print(f"=== Session {s.id}  {when(first_ts, True)} -> {when(last_ts)}"
          f"  ({span(last_ts - first_ts) if first_ts else '-'})")
    opened = s.head.get("open", "?")
    if opened == "boot":
        opened += f" ({s.head.get('boot', '?')})"
    closed = ends[-1]["reason"] if ends else "no closing line (reset or still running)"
    print(f"opened by {opened}, closed by {closed}")
    print(f"profile {p.get('name')}: {p.get('freq_hz', 0) / 1e6:.3f} MHz "
          f"SF{sf} BW{p.get('bw_hz', 0) / 1e3:g} CR4/{p.get('cr')} "
          f"sync 0x{p.get('sync_word', 0):02X}, preamble {p.get('preamble')}, "
          f"CRC {'on' if p.get('crc_on') else 'off'}, "
          f"IQ {'inverted' if p.get('iq_inverted') else 'standard'}, "
          f"rx gain {p.get('rx_gain')}, {p.get('region')}")
    print()

    report_stats(rx, s.of("tx"), limit, first_ts, last_ts)
    report_perturbed(rx)

    counts = {}
    for f in rx:
        counts[f["status"]] = counts.get(f["status"], 0) + 1
    print("receptions [measured]: "
          + ", ".join(f"{k} {v}" for k, v in sorted(counts.items()))
          if counts else "receptions: none")
    for kind in ("gap", "lost"):
        if s.of(kind):
            print(f"{kind} lines: {len(s.of(kind))}")
    print()

    offset, length = args.id or (0, 0)
    groups = group_senders(rx, offset, length)
    named = {k: v for k, v in groups.items() if k is not None}
    if not named:
        named = {"all": rx}
    singles = sum(1 for v in named.values() if len(v) == 1)
    if named and singles > len(named) / 2:
        print(f"note: the id field (offset {offset}, {length} bytes) differs "
              "on most frames, so it is not a sender id here. All frames are "
              "taken as one stream; set the field with --id OFFSET:LEN.")
        print()
        groups = {"all": rx}
        named = {"all": rx}

    failures = []  # (ts, label, frame or None)
    for f in rx:
        if f["status"] != "ok" and "ts" in f:
            failures.append((f["ts"], f["status"], f))

    for sender, frames in sorted(named.items(), key=lambda kv: -len(kv[1])):
        stream = frames if sender != "all" else rx
        ok = [f for f in stream if f["status"] == "ok"]
        print(f"--- sender {sender}: {len(ok)} good frames")
        period, periodic = period_of(stream)
        if period:
            print(f"period [computed]: {period / 1000:.3f} s "
                  f"({'periodic' if periodic else 'irregular, no miss estimate'})")
        if period and periodic:
            missed = missed_slots(stream, period)
            n = sum(c for _, c in missed)
            print(f"never seen [computed, assumes a periodic sender]: {n}")
            for ts, c in missed:
                print(f"    {c} after {when(ts)}")
                failures.append((ts + period, f"never seen x{c}", None))
        rssi = [f["rssi_dbm"] for f in ok]
        sig = [f["signal_dbm"] for f in ok if "signal_dbm" in f]
        snr = [f["snr_db"] for f in ok]
        toa = [f["toa_ms"] for f in ok if "toa_ms" in f]
        if toa:
            print(f"time on air [as the device computed it]: "
                  f"{describe(toa, 'ms', 0)}")
        print(f"RSSI [measured]: {describe(rssi, 'dBm', 0)}")
        print(f"signal alone [measured]: {describe(sig, 'dBm', 0)}")
        print(f"SNR [measured]: {describe(snr, 'dB', 2)}")
        if limit is not None and snr:
            print(f"margin over SF{sf} limit {limit} dB [computed]: "
                  f"{describe([x - limit for x in snr], 'dB', 2)}")
        pairs = [(f["rssi_dbm"] - f["signal_dbm"], f["snr_db"])
                 for f in ok if "signal_dbm" in f]
        if pairs:
            meas = statistics.mean(r for r, _ in pairs)
            exp = statistics.mean(expected_r_minus_s(x) for _, x in pairs)
            print(f"RSSI - signal: {meas:.1f} dB [measured] against "
                  f"{exp:.1f} dB [computed] if only noise were added "
                  "(readings are whole dB: +-1)")
        noise = [f["noise_dbm"] for f in ok if "noise_dbm" in f]
        if noise:
            print(f"noise just before the frames [measured]: "
                  f"{describe(noise, 'dBm', 0)}")
        elif snr and sig:
            if statistics.median(snr) >= SNR_CEILING_DB:
                print(f"noise: not estimable, the SNR is at its observed "
                      f"ceiling (>= {SNR_CEILING_DB} dB)")
            else:
                floor = statistics.median(sig) - statistics.median(snr)
                print(f"noise in the channel, signal - SNR [computed, on "
                      f"received frames only]: {floor:.1f} dBm")
        print()

    failures.sort(key=lambda x: x[0])
    period = period_of(rx)[0]
    join_ms = (EPISODE_PERIODS * period) if period else EPISODE_DEFAULT_S * 1000
    episodes = []
    for item in failures:
        if episodes and item[0] - episodes[-1][-1][0] <= join_ms:
            episodes[-1].append(item)
        else:
            episodes.append([item])
    episodes = [e for e in episodes if len(e) >= 2]
    print(f"episodes, failures closer than {join_ms / 60000:.0f} min "
          f"[computed]: {len(episodes)}")
    for e in episodes:
        print(f"  {when(e[0][0])} -> {when(e[-1][0])}: {len(e)} failures")
        for ts, label, f in e:
            line = f"    {when(ts)} {label:14}"
            if f is not None:
                if "rssi_dbm" in f:
                    line += (f" RSSI {f['rssi_dbm']} sig {f.get('signal_dbm', '-')}"
                             f" SNR {f['snr_db']}")
                if f.get("inst_n"):
                    line += (f"  in-frame {f['inst_min_dbm']}..{f['inst_max_dbm']}"
                             f", peak {peak_over(f):+d} dB")
            print(line)
    print()

    for f in rx:
        margin = f["snr_db"] - limit if "snr_db" in f and limit is not None else ""
        peak = peak_over(f)
        rows.append({
            "session": s.id,
            "time": when(f.get("ts"), True),
            "ts": f.get("ts", ""),
            "status": f["status"],
            "len": f.get("len", ""),
            "toa_ms": f.get("toa_ms", ""),
            "rssi_dbm": f.get("rssi_dbm", ""),
            "signal_dbm": f.get("signal_dbm", ""),
            "snr_db": f.get("snr_db", ""),
            "margin_db": margin,
            "freq_err_hz": f.get("freq_err_hz", ""),
            "noise_dbm": f.get("noise_dbm", ""),
            "cr": f.get("cr", ""),
            "inst_n": f.get("inst_n", ""),
            "inst_min_dbm": f.get("inst_min_dbm", ""),
            "inst_max_dbm": f.get("inst_max_dbm", ""),
            "peak_over_db": "" if peak is None else peak,
            "perturbed": "" if peak is None else int(peak >= PERTURB_DB),
            "sender": sender_of(f, offset, length) or "",
            "data": f.get("data", ""),
        })


FAIL_STYLE = {
    "crc_err": ("CRC failed", "red", "x"),
    "header_err": ("header error", "darkorange", "triangle-up"),
    "incomplete": ("incomplete", "purple", "diamond"),
}


def stamp(ts_ms):
    return datetime.fromtimestamp(ts_ms / 1000)


def with_breaks(points, max_gap_ms):
    """(ts, value) pairs, with a break where the series stopped."""
    xs, ys = [], []
    last = None
    for ts, v in points:
        if last is not None and ts - last > max_gap_ms:
            xs.append(None)
            ys.append(None)
        xs.append(stamp(ts))
        ys.append(v)
        last = ts
    return xs, ys


def plot_session(s, path, args):
    """One HTML file, self-contained: two panels on a shared time axis."""
    import plotly.graph_objects as go
    from plotly.subplots import make_subplots

    p = s.profile
    rx = s.of("rx")
    limit = SNR_LIMIT_DB.get(p.get("sf"))

    fig = make_subplots(
        rows=2, cols=1, shared_xaxes=True, vertical_spacing=0.06,
        row_heights=[0.65, 0.35],
        subplot_titles=(
            "Frames [measured]; never seen [computed]",
            "SNR [measured]",
        ),
    )

    ok = [f for f in rx if f["status"] == "ok"]
    sampled = [f for f in rx if f.get("inst_n") and "ts" in f]
    fig.add_trace(go.Scatter(
        x=[stamp(f["ts"]) for f in sampled],
        y=[f["inst_max_dbm"] for f in sampled],
        name="in-frame peak", mode="markers",
        marker=dict(size=7, color="steelblue", symbol="line-ew-open",
                    line=dict(width=2)),
        hovertemplate="%{x|%H:%M:%S}  %{y} dBm<extra>in-frame peak</extra>"),
        row=1, col=1)
    noisy = [f for f in rx if "noise_dbm" in f and "ts" in f]
    if noisy:
        fig.add_trace(go.Scatter(
            x=[stamp(f["ts"]) for f in noisy],
            y=[f["noise_dbm"] for f in noisy],
            name="noise before the frame", mode="markers",
            marker=dict(size=4, color="grey"),
            hovertemplate="%{x|%H:%M:%S}  %{y} dBm<extra>noise</extra>"),
            row=1, col=1)
    for key, name, color in (("rssi_dbm", "RSSI", "black"),
                             ("signal_dbm", "signal alone", "seagreen")):
        pts = [(f["ts"], f[key]) for f in ok if "ts" in f and key in f]
        fig.add_trace(go.Scatter(
            x=[stamp(t) for t, _ in pts], y=[v for _, v in pts],
            name=name, mode="markers", marker=dict(size=4, color=color),
            hovertemplate="%{x|%H:%M:%S}  %{y} dBm<extra>" + name
                          + "</extra>"), row=1, col=1)

    floor_y = min((f["inst_min_dbm"] for f in sampled), default=-120) - 2
    for status, (name, color, symbol) in FAIL_STYLE.items():
        bad = [f for f in rx if f["status"] == status and "ts" in f]
        if not bad:
            continue
        fig.add_trace(go.Scatter(
            x=[stamp(f["ts"]) for f in bad],
            y=[f.get("rssi_dbm", floor_y) for f in bad],
            name=name, mode="markers",
            marker=dict(size=9, color=color, symbol=symbol),
            customdata=[[f.get("snr_db", ""), f.get("inst_min_dbm", ""),
                         f.get("inst_max_dbm", "")] for f in bad],
            hovertemplate="%{x|%H:%M:%S}  RSSI %{y}  SNR %{customdata[0]}"
                          "<br>in frame %{customdata[1]}..%{customdata[2]} dBm"
                          "<extra>" + name + "</extra>"), row=1, col=1)

    period, periodic = period_of(rx)
    if period and periodic:
        missed = []
        for ts, count in missed_slots(rx, period):
            missed += [ts + k * period for k in range(1, count + 1)]
        if missed:
            fig.add_trace(go.Scatter(
                x=[stamp(t) for t in missed], y=[floor_y] * len(missed),
                name="never seen", mode="markers",
                marker=dict(size=10, color="crimson", symbol="line-ns-open",
                            line=dict(width=2)),
                hovertemplate="%{x|%H:%M:%S}<extra>never seen (expected "
                              "slot)</extra>"), row=1, col=1)

    snr = [(f["ts"], f["snr_db"]) for f in rx if "snr_db" in f and "ts" in f]
    fig.add_trace(go.Scatter(
        x=[stamp(t) for t, _ in snr], y=[v for _, v in snr], name="SNR",
        mode="markers", marker=dict(size=4, color="black"),
        hovertemplate="%{x|%H:%M:%S}  %{y} dB<extra>SNR</extra>"),
        row=2, col=1)
    if limit is not None:
        for level, name, color in ((limit, f"SF{p.get('sf')} limit", "red"),
                                   (limit + 3, "margin 3 dB", "orange"),
                                   (limit + 10, "margin 10 dB", "green")):
            fig.add_hline(y=level, line=dict(width=1, dash="dot", color=color),
                          annotation_text=name, annotation_position="right",
                          row=2, col=1)

    fig.update_yaxes(title_text="dBm", row=1, col=1)
    fig.update_yaxes(title_text="dB", row=2, col=1)
    fig.update_xaxes(rangeslider=dict(visible=True, thickness=0.04),
                     row=2, col=1)
    title = (f"Session {s.id} - {p.get('name')}: {p.get('freq_hz', 0) / 1e6:.3f} "
             f"MHz SF{p.get('sf')} BW{p.get('bw_hz', 0) / 1e3:g}, rx gain "
             f"{p.get('rx_gain')} - {when(s.head.get('ts'), True)}")
    fig.update_layout(title=title, height=800, hovermode="closest",
                      template="plotly_white",
                      legend=dict(orientation="h", y=-0.08))
    fig.write_html(path, include_plotlyjs=True)
    print(f"{path}: plot written")


def main():
    parser = argparse.ArgumentParser(description="Summarise Link-LR recordings.")
    parser.add_argument("files", nargs="+", help=".jsonl files, wildcards allowed")
    parser.add_argument("--id", metavar="OFFSET:LEN",
                        help="where the sender id sits in a payload; "
                             "without it, all frames are one stream")
    parser.add_argument("--max-list", type=int, default=20,
                        help="longest list printed (default: 20)")
    parser.add_argument("--csv", metavar="FILE",
                        help="also write one row per reception")
    parser.add_argument("--plot", metavar="FILE.html",
                        help="also write an interactive chart, one file per "
                             "session (needs plotly)")
    args = parser.parse_args()

    if args.id:
        try:
            off, ln = args.id.split(":")
            args.id = (int(off), int(ln))
        except ValueError:
            sys.exit("--id takes OFFSET:LEN, e.g. 1:3")

    sessions = load(args.files)
    if not sessions:
        sys.exit("no session found")

    if args.plot:
        try:
            import plotly  # noqa: F401
        except ImportError:
            sys.exit("--plot needs plotly: "
                     "python -m pip install -r tools/requirements.txt")

    rows = []
    for s in sessions:
        report_session(s, args, rows)
        if args.plot:
            path = args.plot
            if len(sessions) > 1:
                stem, dot, ext = path.rpartition(".")
                path = f"{stem}_s{s.id}.{ext}" if dot else f"{path}_s{s.id}"
            plot_session(s, path, args)

    if args.csv:
        with open(args.csv, "w", newline="", encoding="utf-8") as f:
            w = csv.DictWriter(f, fieldnames=list(rows[0].keys()) if rows else ["session"])
            w.writeheader()
            w.writerows(rows)
        print(f"{args.csv}: {len(rows)} rows")


if __name__ == "__main__":
    main()
