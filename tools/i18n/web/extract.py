#!/usr/bin/env python3
"""Textos en inglés de la web (views/*.html, static/js propios, mensajes JSON de los .py) para webserver/src/x6100_webserver/i18n_es.txt.
Uso: tools/i18n/web/extract.py  -> imprime los que todavía no están en i18n_es.txt"""
import html.parser, re, os, sys, glob
ROOT = os.path.join(os.path.dirname(__file__), '../../../webserver/src/x6100_webserver')
OWN_JS = ['edit_flow.js', 'list_editor.js', 'radio_link.js', 'radio_session.js']
WORD = re.compile(r'[A-Za-z]{2,}')
found = {}

def add(s, where):
    s = ' '.join(s.split())
    if not s or not WORD.search(s): return
    if '{{' in s or '{%' in s or s.startswith('%') : return
    if re.fullmatch(r'[\w.\-/:#]+\.(html|js|css|py|png|svg|json|txt|db)', s): return
    found.setdefault(s, where)

class P(html.parser.HTMLParser):
    def __init__(s, where):
        super().__init__(convert_charrefs=True); s.where = where; s.skip = 0
    def handle_starttag(s, tag, attrs):
        if tag in ('script', 'style'): s.skip += 1
        for k, v in attrs:
            if k in ('placeholder', 'title', 'aria-label', 'alt', 'data-confirm', 'label') and v: add(v, s.where)
            if k == 'value' and v and tag == 'input' and dict(attrs).get('type') in ('submit', 'button'): add(v, s.where)
    def handle_endtag(s, tag):
        if tag in ('script', 'style'): s.skip -= 1
    def handle_data(s, d):
        if not s.skip: add(d, s.where)

JSSTR = re.compile(r"""(?<![\w$])(['"`])((?:\\.|(?!\1).)*?)\1""")
def js_strings(code, where):
    for m in JSSTR.finditer(code):
        t = m.group(2)
        if m.group(1) == '`': t = re.sub(r'\$\{[^}]*\}', '', t)
        if re.search(r'[A-Z][a-z]+ [a-z]|^[A-Z][a-z]+[.!:…]?$|[a-z]+ [a-z]+ [a-z]+', t) and not re.search(r'^[#.\[]|=>|function|\bvar\b|document\.|querySelector|^\w+/', t):
            if '<' in t:
                p = P(where); p.feed(t)
            else:
                add(t, where)

for f in sorted(glob.glob(ROOT + '/views/*.html')):
    src = open(f, encoding='utf-8').read()
    w = os.path.basename(f)
    body = re.sub(r'<script.*?</script>', '', src, flags=re.S)
    body = re.sub(r'^\s*%.*$', '', body, flags=re.M)
    body = re.sub(r'\{\{.*?\}\}', ' ', body)
    p = P(w); p.feed(body)
    for code in re.findall(r'<script[^>]*>(.*?)</script>', src, flags=re.S): js_strings(code, w)
for j in OWN_JS: js_strings(open(ROOT + '/static/js/' + j, encoding='utf-8').read(), j)
for f in glob.glob(ROOT + '/*.py'):
    src = open(f, encoding='utf-8').read()
    for m in re.finditer(r"""["'](?:error|message|msg|status|detail)["']\s*:\s*f?(["'])(.*?)\1""", src): add(m.group(2), os.path.basename(f))
    for m in re.finditer(r"""(?:abort|HTTPError|HTTPResponse)\(\s*\d+\s*,\s*f?(["'])(.*?)\1""", src): add(m.group(2), os.path.basename(f))

have = set()
es = os.path.join(ROOT, 'i18n_es.txt')
if os.path.exists(es):
    have = {l.split('\t')[0] for l in open(es, encoding='utf-8') if '\t' in l}
for s, w in found.items():
    if s not in have: print(f"{w}\t{s}")
