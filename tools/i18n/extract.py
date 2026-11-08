#!/usr/bin/env python3
"""Lista los textos candidatos a traducir de uno o más fuentes.

Uso: tools/i18n/extract.py src/dialog_ft8.c [...]   [--todo]
Con --todo muestra solo los que todavía no están en tools/i18n/es.txt.
Salida: archivo:línea <TAB> texto <TAB> contexto
"""
import re, sys, pathlib

ROOT = pathlib.Path(__file__).resolve().parents[2]
LIT = re.compile(r'"((?:[^"\\]|\\.)*)"')
SKIP_LINE = re.compile(
    r'LV_LOG_|#\s*include|fprintf\s*\(\s*std|perror|syslog|sqlite3_|"SELECT |"INSERT |"UPDATE |"DELETE |'
    r'popen|system\s*\(|fopen|getenv|strn?cmp|strcasecmp|strstr|cJSON_|json_|remote_send|g_variant|dbus|'
    r'\.name\s*=\s*"|sscanf|strtok|access\s*\(|opendir|unlink|rename\s*\(|mkdir|exec[lv]')
PATHY = re.compile(r'^[\w.\-/]*\.(c|h|db|json|png|jpg|wav|txt|so|sh|conf|csv|adi|adif|log|bin|pem|ini|xml|dat)$|^/')

def known():
    k = set()
    for line in (ROOT / 'tools/i18n/es.txt').read_text(encoding='utf-8').splitlines():
        if line and not line.startswith('#') and '\t' in line:
            k.add(line.split('\t')[0].replace('\\n', '\n'))
    return k

def lines_of(s):
    return [l.strip() for l in s.split('\n')]

SPEC = re.compile(r'%[-+ #0]*\d*(?:\.\d+)?(?:hh|h|ll|l|z)?[diouxXeEfgGcsp%]')
UNITS = re.compile(r'\b(Hz|kHz|MHz|dB|dBm|ms|W|wpm|px|km|mi|s|UTC|bd|min)\b')

def trivial(line):
    """Una línea que no hace falta traducir: números, unidades, siglas cortas"""
    t = UNITS.sub('', SPEC.sub('', line))
    words = re.findall(r'[A-Za-z]{2,}', t)
    if not words:
        return True
    return all(w.isupper() and len(w) <= 5 for w in words) and len(words) <= 3

def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    todo = '--todo' in sys.argv
    k = known() if todo else set()
    seen = set()
    for f in args:
        src = pathlib.Path(f).read_text(encoding='utf-8', errors='replace')
        # Literales pegados ("a" "b") en uno solo
        joined = re.sub(r'"\s*\n\s*"', '', src)
        for no, line in enumerate(joined.split('\n'), 1):
            st = line.strip()
            if st.startswith(('//', '/*', '*')) or SKIP_LINE.search(line):
                continue
            for m in LIT.finditer(line):
                s = bytes(m.group(1), 'utf-8').decode('unicode_escape').encode('latin-1').decode('utf-8', 'replace') \
                    if '\\x' in m.group(1) else m.group(1).replace('\\n', '\n').replace('\\"', '"').replace('\\\\', '\\')
                if not re.search(r'[A-Za-z]{2,}', s) or PATHY.search(s):
                    continue
                if todo and (s in k or all((not l) or l in k or l.rstrip(':') in k or trivial(l) for l in lines_of(s))):
                    continue
                key = (s,)
                if key in seen:
                    continue
                seen.add(key)
                esc = s.replace('\n', '\\n')
                print(f'{pathlib.Path(f).name}:{no}\t{esc}\t{st[:110]}')

main()
