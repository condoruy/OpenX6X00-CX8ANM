"""CX8ANM: GhostNet en la web de la radio (la página /ghostnet). Los mismos horarios que src/ghostnet.c
(GhostNet versión 1.5, s2underground), la configuración de /mnt/ghostnet.conf (la relee la radio sola) y el
registro de mensajes que deja la app JS8 (/mnt/ghostnet_log.txt)."""
import json
import os
import re
import time

from .iso3_web import ISO3

CONF = "/mnt/ghostnet.conf"
LOG = "/mnt/ghostnet_log.txt"
METEO_FILE = "/tmp/x6100-meteo.json"

# (red, día UTC 0 domingo, hh, mm, minutos, kHz, modo, qué)
SEGMENTS = [
    ("NA", 5, 1, 0, 30, 7107, "JS8", "JS8 40 m"),
    ("NA", 5, 1, 30, 30, 7107, "VARA", "Winlink VARA (desde una PC)"),
    ("NA", 5, 2, 0, 30, 7077, "RTTY", "RTTY 45,45 baudios"),
    ("NA", 5, 2, 30, 30, 7190, "LSB", "Voz, solo emergencias (últimos 5 min)"),
    ("EU", 4, 18, 0, 30, 7107, "JS8", "JS8 40 m"),
    ("EU", 4, 18, 30, 30, 7107, "VARA", "Winlink VARA (desde una PC)"),
    ("EU", 4, 19, 0, 30, 7077, "RTTY", "RTTY 45,45 baudios"),
    ("EU", 4, 19, 30, 30, 7190, "LSB", "Voz, solo emergencias (últimos 5 min)"),
    ("AU", 4, 7, 0, 30, 7107, "JS8", "JS8 40 m"),
    ("AU", 4, 7, 30, 30, 7107, "VARA", "Winlink VARA (desde una PC)"),
    ("AU", 4, 8, 0, 30, 7077, "RTTY", "RTTY 45,45 baudios"),
    ("AU", 4, 8, 30, 30, 7190, "LSB", "Voz, solo emergencias (últimos 5 min)"),
    ("B_NAEU", 6, 18, 0, 60, 14107, "JS8", "JS8 20 m"),
    ("B_NAEU", 6, 19, 0, 60, 0, "ALE", "Ion2G ALE (desde una PC)"),
    ("B_NAAU", 6, 12, 0, 60, 14107, "JS8", "JS8 20 m"),
    ("B_NAAU", 6, 13, 0, 30, 0, "ALE", "Ion2G ALE (desde una PC)"),
    ("B_NAAU", 6, 13, 30, 30, 3575, "JS8", "JS8 80 m"),
    ("B_EUAU", 6, 20, 0, 60, 14107, "JS8", "JS8 20 m"),
    ("B_EUAU", 6, 21, 0, 30, 0, "ALE", "Ion2G ALE (desde una PC)"),
    ("B_EUAU", 6, 21, 30, 30, 3575, "JS8", "JS8 80 m"),
    ("B_AUSP", 6, 8, 0, 60, 14107, "JS8", "JS8 20 m"),
    ("B_AUSP", 6, 9, 0, 30, 7107, "JS8", "JS8 40 m"),
    ("B_AUSP", 6, 9, 30, 30, 0, "ALE", "Ion2G ALE (desde una PC)"),
]
NETS = [
    ("NA", "Red Norteamérica", "Inglés. La semanal de los jueves a la noche de allá."),
    ("EU", "Red Europa", "Inglés. La semanal de los jueves."),
    ("AU", "Red Australia", "Inglés. La semanal de los jueves."),
    ("B_NAEU", "Puente Norteamérica-Europa", "Inglés. Sábados, para pasar mensajes entre regiones."),
    ("B_NAAU", "Puente Norteamérica-Australia", "Inglés. Sábados."),
    ("B_EUAU", "Puente Europa-Australia", "Inglés. Sábados."),
    ("B_AUSP", "Puente Australia-Pacífico Sur", "Inglés. Sábados."),
    ("ES", "Red hispana (propia)", "En español: la que elijas abajo."),
]
DEFAULTS = {"remind": "1", "remind_min": "10", "nets": "NA,B_NAEU,ES", "es_on": "0", "es_name": "Red hispana",
            "es_day": "4", "es_time": "21:00", "es_min": "30", "es_khz": "7107", "es_mode": "JS8",
            "translate_rx": "1", "translate_tx": "0", "region": ""}
DAYS = ["domingo", "lunes", "martes", "miércoles", "jueves", "viernes", "sábado"]


def read_conf():
    c = dict(DEFAULTS)
    try:
        with open(CONF, encoding="utf-8") as f:
            for line in f:
                if "=" in line and not line.startswith("#"):
                    k, v = line.rstrip("\r\n").split("=", 1)
                    if k in c:
                        c[k] = v
    except OSError:
        pass
    return c


def write_conf(new):
    """new: lo que mandó la página; se valida todo. Devuelve (conf, error)"""
    c = read_conf()
    nets = [n for n in str(new.get("nets", c["nets"])).split(",") if n in {x[0] for x in NETS}]
    c["nets"] = ",".join(nets)
    for k in ("remind", "es_on", "translate_rx", "translate_tx"):
        if k in new:
            c[k] = "1" if new[k] in (True, 1, "1", "true", "on") else "0"
    try:
        if "remind_min" in new:
            m = int(new["remind_min"])
            if not 1 <= m <= 120:
                return None, "Reminder: between 1 and 120 minutes"
            c["remind_min"] = str(m)
        if "es_day" in new:
            d = int(new["es_day"])
            if not 0 <= d <= 6:
                return None, "Invalid day"
            c["es_day"] = str(d)
        if "es_min" in new:
            m = int(new["es_min"])
            if not 5 <= m <= 240:
                return None, "Duration: between 5 and 240 minutes"
            c["es_min"] = str(m)
        if "es_khz" in new:
            k = float(str(new["es_khz"]).replace(",", "."))
            k = int(round(k * 1000)) if k < 100 else int(round(k))
            if not 1800 <= k <= 54000:
                return None, "Frequency: between 1800 and 54000 kHz"
            c["es_khz"] = str(k)
    except (TypeError, ValueError):
        return None, "Check the numbers"
    if "es_time" in new:
        t = str(new["es_time"]).strip()
        if not re.fullmatch(r"([01]?\d|2[0-3]):[0-5]\d", t):
            return None, "Time like 21:00"
        h, m = t.split(":")
        c["es_time"] = "%02d:%s" % (int(h), m)
    if "es_mode" in new:
        if new["es_mode"] not in ("JS8", "RTTY", "LSB", "USB"):
            return None, "Invalid mode"
        c["es_mode"] = new["es_mode"]
    if "es_name" in new:
        c["es_name"] = re.sub(r"[\r\n=]", " ", str(new["es_name"])).strip()[:30] or "Red hispana"
    if "region" in new:
        r = str(new["region"]).strip().upper().lstrip("@")
        if r and not re.fullmatch(r"GN[A-Z0-9]{3,6}", r):
            return None, "The regional group is like @GNURYMO"
        c["region"] = ("@" + r) if r else ""
    tmp = CONF + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write("# GhostNet (CX8ANM): lo cambian la app JS8 y la página web GhostNet\n")
        for k in DEFAULTS:
            f.write("%s=%s\n" % (k, c[k]))
    os.replace(tmp, CONF)
    return c, None


def region_tag(country, conf):
    if conf.get("region"):
        return conf["region"], "elegido a mano"
    a3 = ISO3.get((country or "").upper()[:2])
    if not a3:
        return "", "elegí el país en Ajustes de la radio"
    state = ""
    try:
        with open(METEO_FILE, encoding="utf-8") as f:
            r = json.load(f).get("region") or ""
        if len(r) >= 4 and r[2] == "-" and r[:2].upper() == country.upper()[:2]:
            state = re.sub(r"[^A-Z0-9]", "", r[3:].upper())[:3]
    except (OSError, ValueError, AttributeError):
        pass
    return "@GN" + a3 + state, ("país y departamento (de Meteo)" if state else
                                "solo el país: abrí Meteo con WiFi para sumar el departamento")


def _next_utc(now, wday, hh, mm, minutes):
    t = time.gmtime(now)
    day0 = now - (t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec)
    ahead = (wday - (t.tm_wday + 1) % 7 + 7) % 7     # tm_wday de Python: 0 lunes
    s = day0 + ahead * 86400 + hh * 3600 + mm * 60
    if s + minutes * 60 <= now:
        s += 7 * 86400
    if s - 7 * 86400 + minutes * 60 > now:
        s -= 7 * 86400
    return s


def _next_local(now, wday, hh, mm, minutes):
    t = time.localtime(now)
    ahead = (wday - (t.tm_wday + 1) % 7 + 7) % 7
    for week in (-1, 0, 1):
        s = time.mktime((t.tm_year, t.tm_mon, t.tm_mday + ahead + week * 7, hh, mm, 0, 0, 0, -1))
        if s + minutes * 60 > now:
            return s
    return now + 7 * 86400


def schedule(conf, days=7):
    now = time.time()
    names = {n[0]: n[1] for n in NETS}
    reminded = set(conf["nets"].split(","))
    out = []
    for net, wday, hh, mm, minutes, khz, mode, what in SEGMENTS:
        s = _next_utc(now, wday, hh, mm, minutes)
        if s <= now + days * 86400:
            out.append((s, net, names[net], minutes, khz, mode, what))
    if conf["es_on"] == "1":
        h, m = (int(x) for x in conf["es_time"].split(":"))
        s = _next_local(now, int(conf["es_day"]), h, m, int(conf["es_min"]))
        if s <= now + days * 86400:
            out.append((s, "ES", conf["es_name"], int(conf["es_min"]), int(conf["es_khz"]), conf["es_mode"],
                        conf["es_mode"] + " en español"))
    out.sort()
    return [{"start": int(s), "local": time.strftime("%Y-%m-%d %H:%M", time.localtime(s)),
             "day": DAYS[(time.localtime(s).tm_wday + 1) % 7], "utc": time.strftime("%a %H:%M", time.gmtime(s)) + "Z",
             "net": net, "name": name, "minutes": minutes, "khz": khz, "mode": mode, "what": what,
             "now": s <= now < s + minutes * 60, "reminder": conf["remind"] == "1" and net in reminded and
             (net == "ES" or mode == "JS8")}
            for s, net, name, minutes, khz, mode, what in out]


def messages(limit=150):
    """Los últimos mensajes del registro, con su traducción, y las estaciones que se oyeron"""
    lines = []
    for path in (LOG + ".1", LOG):
        try:
            with open(path, encoding="utf-8", errors="replace") as f:
                lines += f.read().splitlines()
        except OSError:
            pass
    msgs, trans = [], {}
    for line in lines:
        p = line.split("\t")
        if len(p) < 8:
            continue
        kind, epoch, mid, frm, to, snr, dial, text = p[0], p[1], p[2], p[3], p[4], p[5], p[6], "\t".join(p[7:])
        try:
            epoch, snr, dial = int(epoch), int(snr), int(dial)
        except ValueError:
            continue
        if kind == "T":
            trans[(mid, frm)] = text
            continue
        if kind in ("R", "X"):
            msgs.append({"kind": kind, "time": epoch, "local": time.strftime("%d/%m %H:%M", time.localtime(epoch)),
                         "id": mid, "from": frm, "to": to, "snr": snr, "dial": dial, "text": text,
                         "flash": to.upper() == "@GSTFLASH" or "@GSTFLASH" in text.upper()})
    for m in msgs:
        m["es"] = trans.get((m["id"], m["from"]), "") if m["id"] != "0" else ""
    stations = {}
    for m in msgs:
        if m["kind"] != "R" or not m["from"]:
            continue
        s = stations.setdefault(m["from"], {"call": m["from"], "count": 0, "groups": []})
        s["count"] += 1
        s["last"], s["local"], s["snr"], s["dial"] = m["time"], m["local"], m["snr"], m["dial"]
        if m["to"].startswith("@") and m["to"] not in s["groups"]:
            s["groups"].append(m["to"])
    st = sorted(stations.values(), key=lambda s: -s["last"])
    return msgs[-limit:][::-1], st[:100]
