"""Routes of the Bands, Digital modes, Time and Files pages, of the plain pages and of the static files.
They join the app of apps.py, which imports this module at its end."""

from datetime import datetime, timezone
try:
    import gzip
except ImportError:   # images built before python3-zlib was selected
    gzip = None
import functools
import json
import os
import shutil
import re
import subprocess
import urllib.parse

import bottle

from . import freq_lists
from . import settings
from .apps import PACKAGE_DIR, api_error, app, display_call

STATIC_PATH = os.path.join(PACKAGE_DIR, "static")


# Bands and digital mode frequencies (Bands and Digital modes pages; freq_lists.py does the checks)

def json_list(items):
    """A JSON array: bottle makes JSON of dicts only."""
    bottle.response.content_type = 'application/json'
    return json.dumps(items)


def freq_list_change(change, *args, created=False):
    """Runs change(dbcon, ..., request JSON): {"status": "OK"} (201 for a new entry) or 400 with the reason."""
    try:
        data = bottle.request.json
    except ValueError:   # not JSON at all
        data = None
    try:
        found = change(*args, data)
    except freq_lists.Invalid as e:
        return api_error(400, str(e))
    if found is False:
        return api_error(404, "No such frequency")
    if created:
        bottle.response.status = 201
    return {"status": "OK"}


def band_tx(lo, hi):
    """Whether the radio transmits somewhere in [lo, hi] Hz, as the GUI's band_use has it (src/cfg_socket.cpp: HF, the
    transverters that are on); HF alone when the GUI does not answer. For the TX shift on the Bands page."""
    reply = display_call({"cmd": "band_use", "lo": lo, "hi": hi})
    if reply and reply.get("t") == "band_use":
        return reply.get("use") == 2
    return freq_lists.hf_tx(lo, hi)


@app.get('/api/bands')
def bands_get(dbcon):
    return json_list(freq_lists.bands(dbcon))


@app.put('/api/bands')
def bands_add(dbcon):
    return freq_list_change(functools.partial(freq_lists.add_band, band_tx=band_tx), dbcon, created=True)


@app.post('/api/bands/<band_id:int>')
def bands_set(band_id, dbcon):
    return freq_list_change(functools.partial(freq_lists.update_band, band_tx=band_tx), dbcon, band_id)


@app.delete('/api/bands/<band_id:int>')
def bands_remove(band_id, dbcon):
    freq_lists.delete_band(dbcon, band_id)
    return {"status": "OK"}


@app.get('/api/digital_modes')
def freqs_get(dbcon):
    return json_list(freq_lists.digital_modes(dbcon))


@app.put('/api/digital_modes')
def freqs_add(dbcon):
    return freq_list_change(freq_lists.add_digital_mode, dbcon, created=True)


@app.post('/api/digital_modes/<mode_id:int>')
def freqs_set(mode_id, dbcon):
    return freq_list_change(freq_lists.update_digital_mode, dbcon, mode_id)


@app.delete('/api/digital_modes/<mode_id:int>')
def freqs_remove(mode_id, dbcon):
    freq_lists.delete_digital_mode(dbcon, mode_id)
    return {"status": "OK"}


# Static files, plain pages and the file browser

@app.get('/static/<filepath:path>')
def static_files(filepath):
    # The radio's WiFi is slow: send static files gzipped and let the browser keep them
    response = bottle.static_file(filepath, root=STATIC_PATH)
    if isinstance(response, bottle.HTTPError) or response.status_code != 200:
        return response
    response.set_header('Cache-Control', 'max-age=86400')
    if gzip and 'gzip' in bottle.request.headers.get('Accept-Encoding', ''):
        if filepath not in _gzip_cache:
            with open(os.path.join(STATIC_PATH, filepath), 'rb') as f:
                _gzip_cache[filepath] = gzip.compress(f.read())
        response.body.close()
        response.body = _gzip_cache[filepath]
        response.set_header('Content-Encoding', 'gzip')
        response.set_header('Content-Length', str(len(response.body)))
    return response


_gzip_cache = {}


@app.get('/')
@app.get('/<view:re:bands|digital_modes|time>')
def plain_page(view='index'):
    """Pages that are only their template: the data comes from the API."""
    return bottle.template(view)


@app.get('/files/')
@app.get('/files/<filepath:path>')
@app.get('/files/<filepath:path>/')   # kept for old links: the route above takes the slash too
def file_browser(filepath=''):
    """The DATA partition (FILEBROWSER_PATH): a folder lists its entries, a file is downloaded.
    Nothing outside it: no '..' above it, no absolute path."""
    root = os.path.abspath(settings.FILEBROWSER_PATH)
    target = os.path.abspath(os.path.join(root, filepath.lstrip('/')))
    if os.path.commonpath([root, target]) != root or filepath.startswith('/'):
        raise bottle.HTTPError(403, 'Access denied.')
    if os.path.isfile(target):
        os.sync()   # what the GUI has just written, complete
        response = bottle.static_file(os.path.relpath(target, root), root=root, download=True)
        response.set_header('Cache-Control', 'private, no-cache, no-store')
        return response
    try:
        names = sorted(os.listdir(target))
    except OSError:
        raise bottle.HTTPError(404, 'Not found')
    folders = {n for n in names if os.path.isdir(os.path.join(target, n))}
    sizes = {}
    for n in names:
        try:
            sizes[n] = os.path.getsize(os.path.join(target, n))
        except OSError:
            sizes[n] = 0
    try:
        st = os.statvfs(root)
        free_mb = st.f_bavail * st.f_frsize // (1024 * 1024)
    except OSError:
        free_mb = None
    rel = os.path.relpath(target, root)
    return bottle.template('files', dirs=[n for n in names if n in folders],
                           files=[n for n in names if n not in folders], href=urllib.parse.quote,
                           sizes=sizes, free_mb=free_mb, rel="" if rel == "." else rel,
                           deletable=lambda n: file_deletable(rel, n))


# CX8ANM: borrar desde la página Archivos (antes había que sacar la tarjeta). Solo archivos de la partición DATA;
# nunca los ajustes (params.db y sus -wal/-shm/-journal), ni lo que no es un archivo común. Las carpetas no,
# salvo dentro de update/ (lo que deja una actualización a medias)
KEEP_FILES = re.compile(r'^params\.db(-wal|-shm|-journal)?$')


def file_deletable(rel, name):
    if rel in ("", ".") and KEEP_FILES.match(name):
        return False
    return True


@app.delete('/api/files/<filepath:path>')
def file_delete(filepath):
    root = os.path.abspath(settings.FILEBROWSER_PATH)
    target = os.path.abspath(os.path.join(root, filepath.lstrip('/')))
    if os.path.commonpath([root, target]) != root or target == root or filepath.startswith('/'):
        raise bottle.HTTPError(403, 'Access denied.')
    rel_dir, name = os.path.split(os.path.relpath(target, root))
    if not file_deletable(rel_dir, name):
        bottle.response.status = 403
        return {"error": "The settings file cannot be deleted"}
    try:
        if os.path.isdir(target) and not os.path.islink(target):
            if os.path.relpath(target, root).split(os.sep)[0] != "update":
                bottle.response.status = 403
                return {"error": "Folders cannot be deleted here, only files"}
            shutil.rmtree(target)
        elif os.path.isfile(target) or os.path.islink(target):
            os.remove(target)
        else:
            raise bottle.HTTPError(404, 'Not found')
        os.sync()
    except OSError as e:
        bottle.response.status = 500
        return {"error": "Could not delete: %s" % e.strerror}
    return {"ok": True}


# Time page: the radio clock (set by hand or by NTP) and its timezone (/etc/localtime)

ZONE_NAME = re.compile(r'[A-Za-z0-9_+-]+(/[A-Za-z0-9_+-]+)*')    # Europe/Rome, Etc/GMT+1: no '..', no '/' first
NTP_SERVER = re.compile(r'[A-Za-z0-9][A-Za-z0-9.:-]{0,252}')     # name or address, never an ntpdate option


def time_request():
    try:
        data = bottle.request.json
    except ValueError:
        data = None
    return data if isinstance(data, dict) else {}


@app.get('/api/get_time')
def clock_get():
    return {"server_time": datetime.now(timezone.utc).isoformat()}


def clock_from_ntp(server):
    if not isinstance(server, str) or not NTP_SERVER.fullmatch(server):
        return api_error(400, "Invalid NTP server address")
    try:
        r = subprocess.run(["ntpdate", "-u", server], capture_output=True, timeout=20)
    except subprocess.TimeoutExpired:
        return api_error(500, "NTP update timeout")
    except OSError as e:
        return api_error(500, f"NTP update failed: {e}")
    if r.returncode != 0:
        return api_error(500, "NTP update failed: " + r.stderr.decode(errors="replace"))
    return {"status": "success", "msg": "NTP update successful"}


@app.post('/api/update_time')
def clock_set():
    """update_mode "ntp" with server_address, or "manual" with manual_time "YYYY-MM-DD HH:MM:SS" (radio local time)."""
    data = time_request()
    how = data.get("update_mode")
    if not how:
        return api_error(400, "update_mode is required")
    if how == "ntp":
        return clock_from_ntp(data.get("server_address"))
    if how != "manual":
        return api_error(400, f"unknown update_mode: {how}")
    when = data.get("manual_time")
    if not when:
        return api_error(400, "manual_time is required")
    try:
        when = datetime.strptime(when, "%Y-%m-%d %H:%M:%S")
        subprocess.run(["date", "-s", when.strftime("%Y-%m-%d %H:%M:%S")], check=True)
    except (TypeError, ValueError, OSError, subprocess.SubprocessError) as e:
        return api_error(500, f"Failed to set manual time: {e}")
    return {"status": "success", "msg": "Server time updated manually"}


@app.get('/api/get_timezone')
def zone_get():
    """The zone name of the /etc/localtime link ("Etc/UTC", "Europe/Rome")."""
    path = os.path.realpath(settings.LOCALTIME)
    if not os.path.exists(path):
        return api_error(500, f"Failed to fetch timezone: {settings.LOCALTIME} is missing")
    marker = "/posix/" if "/posix/" in path else "/zoneinfo/"
    return {"timezone": path.rpartition(marker)[2]}


@app.post('/api/set_timezone')
def zone_set():
    zone = time_request().get("timezone")
    if not zone:
        return api_error(400, "Timezone is required")
    target = os.path.join(settings.ZONEINFO, str(zone))
    if not (isinstance(zone, str) and ZONE_NAME.fullmatch(zone) and os.path.isfile(target)):
        return api_error(400, f"Invalid timezone: {zone}")
    tmp = settings.LOCALTIME + ".new"
    try:   # a new link renamed over the old one: never a moment without /etc/localtime
        if os.path.lexists(tmp):
            os.remove(tmp)
        os.symlink(target, tmp)
        os.replace(tmp, settings.LOCALTIME)
    except OSError as e:
        return api_error(500, f"Failed to set timezone: {e}")
    return {"status": "success", "msg": "Timezone updated successfully"}
