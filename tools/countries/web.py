#!/usr/bin/env python3
"""CX8ANM: la lista de países de la radio (src/countries_data.inc) para la web del tiempo:
webserver/src/x6100_webserver/countries_web.py. Correr de nuevo si cambia la tabla de la radio."""
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(ROOT, "src", "countries_data.inc")
OUT = os.path.join(ROOT, "webserver", "src", "x6100_webserver", "countries_web.py")
ROW = re.compile(r'\{"([A-Z]{2})", (\d), \{"((?:[^"\\]|\\.)*)", "((?:[^"\\]|\\.)*)"[^}]*\}, "(?:[^"\\]|\\.)*", "([A-Z0-9]*)"\}')

rows = []
with open(SRC, encoding="utf-8") as f:
    for line in f:
        m = ROW.search(line)
        if m:
            rows.append((m.group(1), int(m.group(2)), m.group(3), m.group(4), m.group(5)))
assert len(rows) > 150, len(rows)
with open(OUT, "w", encoding="utf-8") as f:
    f.write('"""Países de la radio (src/countries_data.inc), hecho por tools/countries/web.py: no editar a mano.\n'
            '(código, región IARU, nombre en inglés, nombre en español, locator de la capital)"""\n')
    f.write("COUNTRIES = [\n")
    for r in rows:
        f.write("    %r,\n" % (r,))
    f.write("]\n")
print(len(rows), "países ->", OUT)
