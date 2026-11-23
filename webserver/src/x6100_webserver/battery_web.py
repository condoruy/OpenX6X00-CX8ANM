"""CX8ANM: the Battery page. Each discharge on battery found in the GUI's log (battery_log.csv, a row a minute: the
voltage, the base's %, the % our firmware shows and what was on) and in x6100-batmon's (battery_usage.csv: CPU,
programs, WiFi, Bluetooth, audio, and the power-offs), with what drew on the pack and the two % side by side.

The base measures no current: what each thing drew is an estimate from the load model of the GUI's time left
(src/bat_est.c), learned from full discharges of an X6100 at receive."""

import io
import json
import os
import time

import bottle

from . import settings
from .apps import app

# src/bat_est.c: %/min of each load (x = 1, average TX watts, WiFi, Bluetooth, headphones, brightness/10) and the
# minutes a pack took from 100 to 0 % with x_ref (receive, WiFi and BT on, screen at 2)
W = (0.42, 0.25, 0.04, 0.03, 0.03, 0.02)
X_REF = (1, 0, 1, 1, 0, 0.2)
REF_MIN = 256
CPU_W = 0.10        # %/min more with the CPU busy all the time: a guess (an A33 at full load, about 1 W of some 5.5)
GAP_S = 180         # rows further apart: the radio was off in between
MIN_ROWS = 15       # shorter stretches on battery are not listed


def data_dir():
    return settings.FILEBROWSER_PATH or "/mnt"


def fnum(v, default=None):
    try:
        return float(v)
    except (TypeError, ValueError):
        return default


def read_log():
    """The GUI's rows, oldest first: dicts"""
    rows = []
    for name in ("battery_log.csv.1", "battery_log.csv"):
        try:
            with open(os.path.join(data_dir(), name), encoding="utf-8", errors="replace") as f:
                for line in f:
                    p = line.strip().split(",")
                    if len(p) < 7 or not p[0].isdigit():
                        continue
                    r = {"t": int(p[0]), "v": fnum(p[1], 0), "base": int(fnum(p[2], 0)), "ext": fnum(p[3], 0),
                         "chg": p[4] == "1", "tx": p[5] == "1", "bl": fnum(p[6], 0)}
                    r["mhz"] = fnum(p[7], 0) if len(p) > 7 else 0
                    r["tx_s"] = fnum(p[8], 0) if len(p) > 8 else 0
                    r["tx_w"] = fnum(p[9], 0) if len(p) > 9 else 0
                    r["wifi"] = p[10] == "1" if len(p) > 10 else True
                    r["bt"] = p[11] == "1" if len(p) > 11 else True
                    r["hp"] = p[12] == "1" if len(p) > 12 else False
                    shown = int(fnum(p[13], -1)) if len(p) > 13 else -1
                    r["ours"] = shown if shown >= 0 else None
                    r["by_volts"] = p[14] == "1" if len(p) > 14 else False
                    rows.append(r)
        except OSError:
            pass
    rows.sort(key=lambda r: r["t"])
    return rows


def read_usage():
    """x6100-batmon's rows and the power-offs: (samples, events), oldest first"""
    samples, events = [], []
    for name in ("battery_usage.csv.1", "battery_usage.csv"):
        try:
            with open(os.path.join(data_dir(), name), encoding="utf-8", errors="replace") as f:
                for line in f:
                    p = line.rstrip("\n").split(",")
                    if not p[0].isdigit() or len(p) < 2:
                        continue
                    t = int(p[0])
                    if p[1] in ("start", "stop"):
                        events.append({"t": t, "what": p[1]})
                    elif p[1] == "poweroff":
                        events.append({"t": t, "what": "poweroff", "why": p[2] if len(p) > 2 else "",
                                       "v": fnum(p[3]) if len(p) > 3 else None})
                    elif len(p) >= 9:
                        procs = {}
                        for item in ",".join(p[8:]).split(";"):
                            k, _, v = item.rpartition(":")
                            if k:
                                procs[k] = fnum(v, 0)
                        samples.append({"t": t, "cpu": fnum(p[1], 0), "load": fnum(p[2], 0), "temp": fnum(p[3]),
                                        "mem": fnum(p[4]), "dbm": fnum(p[5]), "bt": p[6] == "1",
                                        "audio": p[7] == "1", "procs": procs})
        except OSError:
            pass
    samples.sort(key=lambda r: r["t"])
    events.sort(key=lambda r: r["t"])
    return samples, events


def on_battery(r):
    return r["ext"] < 3.0 and r["v"] >= 3.0


def stretches(rows):
    """The stretches on battery: lists of rows. A charge in between (external power, or the radio off and the
    battery 0.2 V up when it came back) ends one"""
    out, cur = [], []
    for r in rows:
        if not on_battery(r):
            if cur:
                out.append(cur)
            cur = []
            continue
        if cur and r["t"] - cur[-1]["t"] > GAP_S and r["v"] >= cur[-1]["v"] + 0.2:
            out.append(cur)
            cur = []
        cur.append(r)
    if cur:
        out.append(cur)
    return [s for s in out if len(s) >= MIN_ROWS]


def ours(r):
    return r["ours"] if r["ours"] is not None else r["base"]


def analyse(rows, samples, events, now=None):
    now = now or time.time()
    a, b = rows[0], rows[-1]
    on_s, off_s = 0, 0
    for p, q in zip(rows, rows[1:]):
        d = q["t"] - p["t"]
        if d <= GAP_S:
            on_s += d
        else:
            off_s += d
    on_min = on_s / 60.0
    n = len(rows)

    # how it ended
    end = "unknown"
    off = [e for e in events if e["what"] == "poweroff" and b["t"] - 60 <= e["t"] <= b["t"] + 600]
    if now - b["t"] < GAP_S:
        end = "running"
    elif off:
        end = "lowbat" if off[-1]["why"] == "low battery" else "keyoff"
    elif b["v"] <= 6.4 or ours(b) <= 2:
        end = "cut"             # the power went with no power-off of ours: the base's own cut
    complete = end in ("lowbat", "cut") and max(ours(a), a["base"]) >= 85

    # what was on, over the time
    wifi = sum(r["wifi"] for r in rows) / n
    bt = sum(r["bt"] for r in rows) / n
    hp = sum(r["hp"] for r in rows) / n
    bl = sum(r["bl"] for r in rows) / n
    tx_s = sum(r["tx_s"] for r in rows)
    tx_ws = sum(r["tx_s"] * r["tx_w"] for r in rows)
    tx_w_avg = tx_ws / tx_s if tx_s else 0
    tx_w_time = tx_ws / on_s if on_s else 0          # watts averaged over all the time on
    mhz = [r["mhz"] for r in rows if r["mhz"]]

    us = [s for s in samples if a["t"] <= s["t"] <= b["t"] + 60]
    cpu = sum(s["cpu"] for s in us) / len(us) if us else None
    temps = [s["temp"] for s in us if s["temp"] is not None]
    procs = {}
    for s in us:
        for k, v in s["procs"].items():
            procs[k] = procs.get(k, 0) + v / 100.0     # CPU-minutes (a row a minute)
    top = sorted(procs.items(), key=lambda kv: -kv[1])[:10]

    # the load model: %/min of each part, and the minutes it should have lasted
    parts = [("rx", W[0]), ("tx", W[1] * tx_w_time), ("wifi", W[2] * wifi),
             ("bt", W[3] * bt), ("hp", W[4] * hp), ("bl", W[5] * bl / 10.0)]
    if cpu is not None:
        parts.append(("cpu", CPU_W * cpu / 100.0))
    rate = sum(v for _, v in parts)
    ref_rate = sum(w * x for w, x in zip(W, X_REF))
    used = max(ours(a) - ours(b), 0) if not complete else ours(a)
    expected = REF_MIN * ref_rate / rate * used / 100.0 if rate else None
    full_expected = REF_MIN * ref_rate / rate if rate else None
    health = on_min / expected * 100 if complete and expected else None

    # ours against the base's: where they stood, how far apart, and against a straight line to 0 at the end
    pts = [{"t": r["t"], "v": r["v"], "base": r["base"], "ours": r["ours"], "by_volts": r["by_volts"]} for r in rows]
    have_ours = any(r["ours"] is not None for r in rows)
    diffs = [abs(r["base"] - r["ours"]) for r in rows if r["ours"] is not None]
    maxd = max(diffs) if diffs else None
    rms = {}
    if complete and on_s:
        elapsed, prev = 0, rows[0]
        se_b = se_o = 0.0
        start = ours(a)
        for r in rows:
            d = r["t"] - prev["t"]
            elapsed += d if d <= GAP_S else 0
            prev = r
            ideal = start * (1 - elapsed / on_s)
            se_b += (r["base"] - ideal) ** 2
            se_o += (ours(r) - ideal) ** 2
        rms = {"base": round((se_b / n) ** 0.5, 1), "ours": round((se_o / n) ** 0.5, 1)}
    stuck = 0           # the longest the base's % stood still while the voltage fell 0.3 V or more
    i = 0
    for j in range(1, n):
        if rows[j]["base"] != rows[i]["base"]:
            i = j
        elif rows[i]["v"] - rows[j]["v"] >= 0.3:
            stuck = max(stuck, (rows[j]["t"] - rows[i]["t"]) / 60.0)

    # the voltage: when it went under each mark, and the time spent low
    marks = {}
    for m in (7.6, 7.4, 7.2, 7.0, 6.8):
        el, prev = 0, rows[0]
        for r in rows:
            d = r["t"] - prev["t"]
            el += d if d <= GAP_S else 0
            prev = r
            if r["v"] < m:
                marks["%.1f" % m] = round(el / 60.0)
                break
    low = sum(1 for r in rows if r["v"] < 7.2) / n

    return {
        "start": a["t"], "end": b["t"], "on_min": round(on_min), "off_min": round(off_s / 60.0), "end_how": end,
        "complete": complete, "rows": n,
        "first": {"v": a["v"], "base": a["base"], "ours": ours(a)},
        "last": {"v": b["v"], "base": b["base"], "ours": ours(b)},
        "on": {"wifi": round(wifi * 100), "bt": round(bt * 100), "hp": round(hp * 100), "bl": round(bl, 1),
               "tx_min": round(tx_s / 60.0, 1), "tx_w": round(tx_w_avg, 1), "mhz": round(sum(mhz) / len(mhz)) if mhz else None,
               "cpu": round(cpu, 1) if cpu is not None else None,
               "temp_avg": round(sum(temps) / len(temps)) if temps else None, "temp_max": max(temps) if temps else None,
               "audio": round(sum(s["audio"] for s in us) / len(us) * 100) if us else None,
               "wifi_dbm": round(sum(s["dbm"] for s in us if s["dbm"] is not None) /
                                 max(1, sum(1 for s in us if s["dbm"] is not None))) if any(s["dbm"] is not None for s in us) else None,
               "usage_rows": len(us)},
        "procs": [{"name": k, "cpu_min": round(v, 1)} for k, v in top],
        "parts": [{"name": k, "rate": round(v, 4), "share": round(v / rate * 100) if rate else 0} for k, v in parts],
        "rate": round(rate, 3), "used": used,
        "expected_min": round(expected) if expected else None,
        "full_expected_min": round(full_expected) if full_expected else None,
        "health": round(health) if health else None,
        "cmp": {"have_ours": have_ours, "max_diff": maxd, "mean_diff": round(sum(diffs) / len(diffs), 1) if diffs else None,
                "rms": rms, "base_stuck_min": round(stuck), "by_volts": round(sum(r["by_volts"] for r in rows) / n * 100)},
        "volts": {"marks": marks, "low_share": round(low * 100)},
        "points": pts,
    }


def findings(r):
    """Codes for the page (it has the texts, translated) and plain text for the report"""
    out = []
    on = r["on"]
    if r["health"] is not None:
        if r["health"] < 60:
            out.append(("worn", r["health"]))
        elif r["health"] < 80:
            out.append(("tired", r["health"]))
        else:
            out.append(("healthy", r["health"]))
    if r["volts"]["marks"].get("7.4") is not None and r["on_min"] and r["volts"]["marks"]["7.4"] < r["on_min"] * 0.25:
        out.append(("sag", r["volts"]["marks"]["7.4"]))
    if on["wifi"] >= 50:
        out.append(("wifi", on["wifi"]))
    if on["bt"] >= 50 and on["hp"] < 10:
        out.append(("bt_idle", on["bt"]))
    if on["bl"] >= 6:
        out.append(("bright", on["bl"]))
    if on["cpu"] is not None and on["cpu"] >= 35:
        out.append(("cpu", on["cpu"]))
    if on["tx_min"] >= 10:
        out.append(("tx", on["tx_min"]))
    if r["cmp"]["max_diff"] is not None and r["cmp"]["max_diff"] >= 20:
        out.append(("apart", r["cmp"]["max_diff"]))
    if r["cmp"]["base_stuck_min"] >= 30:
        out.append(("stuck", r["cmp"]["base_stuck_min"]))
    rms = r["cmp"]["rms"]
    if rms and r["cmp"]["have_ours"]:
        out.append(("better_ours" if rms["ours"] + 3 < rms["base"] else "better_base" if rms["base"] + 3 < rms["ours"]
                    else "both_alike", rms))
    if r["end_how"] == "cut":
        out.append(("cut", r["last"]["v"]))
    if on["usage_rows"] == 0:
        out.append(("no_usage", 0))
    return [{"code": c, "val": v} for c, v in out]


def all_runs():
    rows = read_log()
    samples, events = read_usage()
    runs = []
    for s in stretches(rows):
        r = analyse(s, samples, events)
        r["findings"] = findings(r)
        runs.append(r)
    runs.reverse()      # newest first
    now = rows[-1] if rows else None
    return runs, now


@app.route('/bateria')
def battery_page():
    return bottle.template('bateria')


@app.get('/api/battery')
def battery_api():
    runs, now = all_runs()
    for r in runs[1:]:
        r.pop("points", None)       # the chart of the one asked for (?n=), not of all
    return {"runs": runs, "now": now}


@app.get('/api/battery/run/<n:int>')
def battery_run(n):
    runs, _ = all_runs()
    if not 0 <= n < len(runs):
        bottle.response.status = 404
        return {"msg": "No such discharge"}
    return runs[n]


def fmt_t(t):
    return time.strftime("%Y-%m-%d %H:%M", time.localtime(t))


@app.get('/bateria/informe.txt')
def battery_report():
    """Everything, as text: to send for a look"""
    runs, now = all_runs()
    out = io.StringIO()
    ver = ""
    try:
        with open("/usr/share/x6100/app-version") as f:
            ver = f.read().strip()
    except OSError:
        pass
    out.write("OpenX6X00-CX8ANM battery report  %s  %s\n" % (fmt_t(time.time()), ver))
    if now:
        out.write("Now: %.1f V, base %d%%, ours %s%%, supply %.1f V, charging %s\n\n" %
                  (now["v"], now["base"], now["ours"] if now["ours"] is not None else "?", now["ext"], now["chg"]))
    if not runs:
        out.write("No discharge on battery in the logs yet.\n")
    for i, r in enumerate(runs):
        out.write("== Discharge %d: %s -> %s, %d min on (%d min off), ended: %s%s\n" %
                  (i + 1, fmt_t(r["start"]), fmt_t(r["end"]), r["on_min"], r["off_min"], r["end_how"],
                   ", COMPLETE" if r["complete"] else ""))
        f, l = r["first"], r["last"]
        out.write("   start %.1f V base %d%% ours %d%%   end %.1f V base %d%% ours %d%%\n" %
                  (f["v"], f["base"], f["ours"], l["v"], l["base"], l["ours"]))
        out.write("   on: %s\n" % json.dumps(r["on"]))
        out.write("   model: %.3f %%/min, expected %s min for the %s%% used (100%%: %s min), health %s%%\n" %
                  (r["rate"], r["expected_min"], r["used"], r["full_expected_min"], r["health"]))
        out.write("   parts: %s\n" % ", ".join("%s %d%%" % (p["name"], p["share"]) for p in r["parts"]))
        out.write("   programs (CPU-min): %s\n" % ", ".join("%s %.1f" % (p["name"], p["cpu_min"]) for p in r["procs"]))
        out.write("   ours vs base: %s\n" % json.dumps(r["cmp"]))
        out.write("   voltage: %s\n" % json.dumps(r["volts"]))
        out.write("   findings: %s\n" % ", ".join("%s=%s" % (x["code"], x["val"]) for x in r["findings"]))
        if i == 0 and r.get("points"):
            out.write("   minute by minute (time, V, base %, ours %, voltage's):\n")
            for p in r["points"]:
                out.write("   %s,%.1f,%d,%s,%d\n" % (time.strftime("%H:%M", time.localtime(p["t"])), p["v"], p["base"],
                                                     p["ours"] if p["ours"] is not None else "", p["by_volts"]))
        out.write("\n")
    bottle.response.content_type = "text/plain; charset=utf-8"
    bottle.response.set_header("Content-Disposition", 'attachment; filename="bateria_informe.txt"')
    return out.getvalue()
