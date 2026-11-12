"""CX8ANM: the configurator in Spanish when the radio's language is Spanish (Settings > Interface > Language).

The pages, the configurator's own scripts and the messages of its JSON answers are written in English; this
WSGI middleware puts each text it knows (i18n_es.txt: English<TAB>Spanish, one per line) in their place as
they leave the server:
  - HTML: text between tags and the placeholder / title / aria-label / alt / label / value attributes;
  - JavaScript (inline <script> and the configurator's own files under /static/js): string literals whose
    text, spaces at the ends aside, is in the table. A literal of one word only when it is not compared
    anywhere in the page (== 'Off', case 'Ready':): those are states, not texts;
  - JSON: the values of message / error / msg / detail.
Everything else (pictures, audio, video, streams, downloads, WebSockets) passes as it is. Another language than
Spanish, or no table: nothing is changed.
"""
import html
import json
import os
import re
import sqlite3
import threading
import time

from . import settings

_HERE = os.path.dirname(os.path.abspath(__file__))
_OWN_JS = ('/static/js/edit_flow.js', '/static/js/list_editor.js', '/static/js/radio_link.js',
           '/static/js/radio_session.js')
LANG_ES = 1


def _load():
    table = {}
    try:
        with open(os.path.join(_HERE, 'i18n_es.txt'), encoding='utf-8') as f:
            for line in f:
                line = line.rstrip('\n')
                if not line or line.startswith('#') or '\t' not in line:
                    continue
                k, v = line.split('\t', 1)
                if k and v:
                    table[k] = v
    except OSError:
        pass
    return table


TABLE = _load()

_lang = {'t': 0.0, 'v': None}
_lang_lock = threading.Lock()


def spanish():
    """The radio's language from params.db, read at most every 5 s."""
    now = time.monotonic()
    with _lang_lock:
        if now - _lang['t'] < 5 and _lang['v'] is not None:
            return _lang['v']
        v = LANG_ES   # the radio's default
        try:
            con = sqlite3.connect(f"file:{settings.DB_PATH}?mode=ro", uri=True)
            try:
                row = con.execute("SELECT val FROM params WHERE name = 'language'").fetchone()
            finally:
                con.close()
            if row and row[0] is not None and str(row[0]).strip() != '':
                v = int(row[0])
        except (sqlite3.Error, ValueError):
            pass
        _lang['t'], _lang['v'] = now, v == LANG_ES
        return _lang['v']


def tr(text):
    """The text with its spaces at the ends kept, the middle looked up with its spaces folded."""
    core = text.strip()
    if not core:
        return text
    key = ' '.join(core.split())
    v = TABLE.get(key)
    if v is None:
        return text
    lead = text[:len(text) - len(text.lstrip())]
    trail = text[len(text.rstrip()):]
    return lead + v + trail


_TEXT = re.compile(r'>([^<>]+)<')
_ATTR = re.compile(r'(\s(?:placeholder|title|aria-label|alt|label|data-confirm)=)(["\'])(.*?)\2', re.S)
# value= only on buttons: on an <option> or a field it is what the form sends
_BUTTON = re.compile(r'<input\b[^>]*\btype=(["\'])(?:submit|button|reset)\1[^>]*>', re.I)
_VALUE = re.compile(r'(\svalue=)(["\'])(.*?)\2', re.S)
_SCRIPT = re.compile(r'(<script\b[^>]*>)(.*?)(</script>)', re.S | re.I)
_STYLE = re.compile(r'(<style\b[^>]*>.*?</style>)', re.S | re.I)
_JSSTR = re.compile(r"""(?<![\w$\\])(['"`])((?:\\.|(?!\1)[^\\\n])*?)\1""")
_COMPARED = re.compile(r"""(?:===?|!==?|case)\s*(['"`])([^'"`\n]{1,40})\1|(['"`])([^'"`\n]{1,40})\3\s*(?:===?|!==?)""")


def _compared(code):
    out = set()
    for m in _COMPARED.finditer(code):
        out.add(m.group(2) or m.group(4))
    return out


def tr_js(code):
    keep = _compared(code)

    def one(m):
        q, s = m.group(1), m.group(2)
        core = s.strip()
        # One word: never in lower case (a class, a state, a CSS value), nor one the code compares
        if not core or (' ' not in core and (core in keep or core.islower())):
            return m.group(0)
        if q == '`' and '${' in s:
            parts = re.split(r'(\$\{[^}]*\})', s)
            return q + ''.join(p if p.startswith('${') else tr(p) for p in parts) + q
        t = tr(s)
        if t is s:
            return m.group(0)
        if q != '`':
            t = t.replace('\\' + q, q).replace(q, '\\' + q)   # the Spanish text may hold the quote
        return q + t + q

    return _JSSTR.sub(one, code)


def tr_html(page):
    # Scripts and styles out of the way, the markup translated, the scripts translated on their own
    held = []

    def hold(m):
        held.append(m.group(0))
        return f'\x00{len(held) - 1}\x00'

    def hold_script(m):
        held.append(m.group(1) + tr_js(m.group(2)) + m.group(3))
        return f'\x00{len(held) - 1}\x00'

    page = _SCRIPT.sub(hold_script, page)
    page = _STYLE.sub(hold, page)
    page = _TEXT.sub(lambda m: '>' + _tr_markup(m.group(1)) + '<', page)
    page = _ATTR.sub(lambda m: m.group(1) + m.group(2) + _tr_attr(m.group(3)) + m.group(2), page)
    page = _BUTTON.sub(lambda b: _VALUE.sub(lambda m: m.group(1) + m.group(2) + _tr_attr(m.group(3)) + m.group(2), b.group(0)), page)
    return re.sub(r'\x00(\d+)\x00', lambda m: held[int(m.group(1))], page)


def _tr_markup(text):
    """A text node: looked up as the browser shows it (&rarr; as →), put back escaped."""
    if '&' not in text:
        return tr(text)
    plain = html.unescape(text)
    t = tr(plain)
    return text if t is plain else html.escape(t, quote=False)


def _tr_attr(text):
    plain = html.unescape(text)
    t = tr(plain)
    return text if t is plain else html.escape(t, quote=True)


def _tr_json(v):
    if isinstance(v, dict):
        return {k: (tr(x) if isinstance(x, str) and k in ('message', 'error', 'msg', 'detail') else _tr_json(x))
                for k, x in v.items()}
    if isinstance(v, list):
        return [_tr_json(x) for x in v]
    return v


class Spanish:
    """WSGI middleware around the configurator."""

    def __init__(self, app):
        self.app = app

    def _kind(self, path, ctype):
        ctype = (ctype or '').split(';')[0].strip().lower()
        if ctype == 'text/html':
            return 'html'
        if path in _OWN_JS:
            return 'js'
        if ctype == 'application/json':
            return 'json'
        return None

    def __call__(self, environ, start_response):
        if not TABLE or environ.get('REQUEST_METHOD') == 'HEAD' or not spanish():
            return self.app(environ, start_response)
        path = environ.get('PATH_INFO', '')
        held = {}
        chunks = []

        def sr(status, headers, exc_info=None):
            ctype = next((v for k, v in headers if k.lower() == 'content-type'), '')
            kind = self._kind(path, ctype)
            # pages and scripts only when they are the real thing; the error messages (JSON) of any answer
            if kind and kind != 'json' and not status.startswith('200'):
                kind = None
            if kind:
                held.update(status=status, headers=headers, exc_info=exc_info, kind=kind)
                return chunks.append
            return start_response(status, headers, exc_info)

        result = self.app(environ, sr)
        if not held:
            return result
        try:
            body = b''.join(chunks) + b''.join(result)
        finally:
            if hasattr(result, 'close'):
                result.close()
        try:
            text = body.decode('utf-8')
            if held['kind'] == 'html':
                text = tr_html(text)
            elif held['kind'] == 'js':
                text = tr_js(text)
            else:
                text = json.dumps(_tr_json(json.loads(text)), ensure_ascii=False)
            body = text.encode('utf-8')
        except (UnicodeDecodeError, ValueError):
            pass
        headers = [(k, v) for k, v in held['headers'] if k.lower() not in ('content-length', 'etag', 'last-modified')]
        headers.append(('Content-Length', str(len(body))))
        if held['kind'] == 'js':
            headers.append(('Cache-Control', 'no-cache'))
        start_response(held['status'], headers, held['exc_info'])
        return [body]
