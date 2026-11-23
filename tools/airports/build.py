#!/usr/bin/env python3
"""rootfs/usr/share/x6100/airports.tsv from OurAirports' airports.csv (public domain, ourairports.com):
the large and medium airports with scheduled flights and a 4-letter ICAO code, for Vuelos' "Follow flight".
Usage: tools/airports/build.py airports.csv"""
import csv
import sys

out = []
for r in csv.DictReader(open(sys.argv[1], encoding="utf-8")):
    icao = (r["icao_code"] or r["gps_code"] or r["ident"]).strip().upper()
    if r["type"] not in ("large_airport", "medium_airport") or r["scheduled_service"] != "yes" or len(icao) != 4 \
            or not icao.isalpha():
        continue
    clean = lambda s: " ".join((s or "").replace("\t", " ").split())
    out.append("\t".join([icao, "%.4f" % float(r["latitude_deg"]), "%.4f" % float(r["longitude_deg"]),
                          "L" if r["type"] == "large_airport" else "M", r["iso_country"], clean(r["name"])[:60],
                          clean(r["municipality"])[:40]]))
out.sort()
with open("rootfs/usr/share/x6100/airports.tsv", "w", encoding="utf-8") as f:
    f.write("# ICAO\tlat\tlon\tL(arge)/M(edium)\tcountry\tname\tcity  (OurAirports, public domain: tools/airports/build.py)\n")
    f.write("\n".join(out) + "\n")
print(len(out))
