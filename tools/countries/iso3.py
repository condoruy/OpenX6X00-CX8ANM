#!/usr/bin/env python3
"""CX8ANM: códigos ISO de 2 y 3 letras de los países (para el grupo regional de GhostNet, @GN + país + estado):
src/ghostnet_iso3.inc. Hace falta pycountry (pip install pycountry) solo para volver a generarlo."""
import os

import pycountry

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, "src", "ghostnet_iso3.inc")
rows = sorted((c.alpha_2, c.alpha_3) for c in pycountry.countries)
with open(OUT, "w") as f:
    f.write("/* Hecho por tools/countries/iso3.py: no editar a mano. {ISO 3166-1 alfa-2, alfa-3} */\n")
    for i in range(0, len(rows), 6):
        f.write("    " + " ".join('{"%s", "%s"},' % r for r in rows[i:i + 6]) + "\n")
WEB = os.path.join(ROOT, "webserver", "src", "x6100_webserver", "iso3_web.py")
with open(WEB, "w") as f:
    f.write('"""Hecho por tools/countries/iso3.py: no editar a mano. ISO 3166-1 alfa-2 -> alfa-3 (grupo @GN de GhostNet)"""\n')
    f.write("ISO3 = {\n")
    for i in range(0, len(rows), 8):
        f.write("    " + " ".join("%r: %r," % r for r in rows[i:i + 8]) + "\n")
    f.write("}\n")
print(len(rows), "->", OUT, WEB)
