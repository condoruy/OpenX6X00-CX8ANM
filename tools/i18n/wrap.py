#!/usr/bin/env python3
"""Pone tr() a los textos de la tabla que se arman con snprintf/sprintf/strncat
antes de llegar a la pantalla (los que van directo a LVGL ya pasan por tr()).

Uso: tools/i18n/wrap.py src/dialog_js8.c [...]   (--dry: solo muestra)
Solo toca literales que están en tools/i18n/es.txt, en líneas con esas
funciones; nunca los que ya tienen tr( delante.
"""
import re, sys, pathlib

ROOT = pathlib.Path(__file__).resolve().parents[2]
CALLS = re.compile(r'\b(v?s?n?printf|strn?cat|strn?cpy)\s*\(')
LIT = re.compile(r'"((?:[^"\\]|\\.)*)"')

def keys():
    k = set()
    for line in (ROOT / 'tools/i18n/es.txt').read_text(encoding='utf-8').splitlines():
        if line and not line.startswith('#') and '\t' in line:
            k.add(line.split('\t')[0])
    return k

def lookup_forms(raw):
    """El literal como queda en es.txt (\\n escrito), y sin espacios a los costados"""
    s = raw.replace('\\"', '"')
    return {s, s.strip()}

def main():
    dry = '--dry' in sys.argv
    k = keys()
    total = 0
    for f in [a for a in sys.argv[1:] if not a.startswith('--')]:
        p = pathlib.Path(f)
        lines = p.read_text(encoding='utf-8').split('\n')
        changed = 0
        depth = 0           # paréntesis abiertos de una llamada que sigue en la línea siguiente
        for i, line in enumerate(lines):
            st = line.strip()
            nostr = LIT.sub('""', line)
            if depth > 0:
                depth += nostr.count('(') - nostr.count(')')
            elif CALLS.search(line) and not st.startswith(('//', '/*', '*', '#')) and 'LV_LOG' not in line \
                    and 'fprintf' not in line:
                m = CALLS.search(nostr)
                depth = max(0, nostr[m.start():].count('(') - nostr[m.start():].count(')')) if m else 0
            else:
                continue
            out, pos = [], 0
            for m in LIT.finditer(line):
                before = line[:m.start()].rstrip()
                if before.endswith('tr(') or not (lookup_forms(m.group(1)) & k):
                    continue
                # sin espacios afuera: el literal tal cual en tr()
                out.append(line[pos:m.start()] + 'tr(' + m.group(0) + ')')
                pos = m.end()
            if out:
                new = ''.join(out) + line[pos:]
                if dry:
                    print(f'{p.name}:{i+1}: {new.strip()[:140]}')
                lines[i] = new
                changed += 1
        if changed and not dry:
            p.write_text('\n'.join(lines), encoding='utf-8')
        total += changed
        print(f'{p.name}: {changed} líneas')
    print('total', total)

main()
