/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Idiomas de la interfaz: ver i18n.h. Las tablas están en i18n_<idioma>.inc,
 *  una línea por texto: { "texto original en inglés", "traducción" },
 */
#include "i18n.h"

#include <string.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <mutex>
#include <stdio.h>
#include <stdlib.h>

namespace {

struct entry_t {
    const char *en;
    const char *tr;
};

const entry_t table_es[] = {
#include "i18n_es.inc"
};
const entry_t table_de[] = {
#include "i18n_de.inc"
};
const entry_t table_fr[] = {
#include "i18n_fr.inc"
};
const entry_t table_it[] = {
#include "i18n_it.inc"
};

typedef std::unordered_map<std::string_view, const char *> dict_t;

struct lang_tab_t {
    const entry_t *rows;
    size_t         n;
    dict_t         dict;
    std::once_flag once;
};

#define TAB(t) { t, sizeof(t) / sizeof(t[0]), {}, {} }
lang_tab_t tabs[LANG_COUNT] = { {nullptr, 0, {}, {}}, TAB(table_es), TAB(table_de), TAB(table_fr), TAB(table_it) };

volatile uint8_t cur_lang = LANG_EN;

/* Resultados armados línea por línea: un anillo por hilo. LVGL copia el texto
 * al momento (lv_label_set_text y compañía), así que alcanza con que el
 * puntero viva un rato. */
constexpr int    RING_N   = 24;
constexpr size_t RING_LEN = 2048;
thread_local char ring[RING_N][RING_LEN];
thread_local int  ring_pos;

/* Para los *_static de LVGL, que guardan el puntero: copias permanentes */
std::mutex                                   perm_mutex;
std::unordered_map<std::string, std::string> perm;

void build_dict(lang_tab_t *t) {
    for (size_t i = 0; i < t->n; i++) {
        t->dict.emplace(std::string_view(t->rows[i].en), t->rows[i].tr);
    }
}

const dict_t *cur_dict() {
    uint8_t l = cur_lang;
    if (l == LANG_EN || l >= LANG_COUNT) return nullptr;
    lang_tab_t *t = &tabs[l];
    std::call_once(t->once, build_dict, t);
    return &t->dict;
}

const char *find(const dict_t *d, std::string_view key) {
    auto it = d->find(key);
    return it == d->end() ? nullptr : it->second;
}

/* Una línea: se respetan los espacios de los costados (" Off ") y, si no está
 * con dos puntos al final, se busca sin ellos ("Volume:" -> "Volume" + ":") */
bool line_tr(const dict_t *d, std::string_view line, std::string &out) {
    size_t a = 0, b = line.size();
    while (a < b && (line[a] == ' ' || line[a] == '\t')) a++;
    while (b > a && (line[b - 1] == ' ' || line[b - 1] == '\t')) b--;
    if (a == b) return false;

    std::string_view core  = line.substr(a, b - a);
    const char      *t     = find(d, core);
    bool             colon = false;

    if (!t && core.size() > 1 && core.back() == ':') {
        t     = find(d, core.substr(0, core.size() - 1));
        colon = t != nullptr;
    }
    if (!t) return false;
    out.append(line.substr(0, a));
    out.append(t);
    if (colon) out.push_back(':');
    out.append(line.substr(b));
    return true;
}

/* Una línea con colores de LVGL ("#cbd5e1 Volume:# #4f8bf5 30#"): se traduce el texto de cada
 * tramo, los códigos de color quedan */
bool recolor_tr(const dict_t *d, std::string_view line, std::string &out) {
    if (line.find('#') == std::string_view::npos) return false;
    std::string res;
    bool        changed = false;
    size_t      i       = 0;
    while (i < line.size()) {
        size_t h = line.find('#', i);
        if (h == std::string_view::npos) {
            std::string_view rest = line.substr(i);
            if (!line_tr(d, rest, res)) res.append(rest); else changed = true;
            break;
        }
        std::string_view before = line.substr(i, h - i);
        if (!before.empty()) {
            if (!line_tr(d, before, res)) res.append(before); else changed = true;
        }
        /* "#rrggbb " abre un tramo, "#" suelto lo cierra */
        if (h + 8 <= line.size() && line[h + 7] == ' ') {
            bool hex = true;
            for (size_t k = h + 1; k < h + 7; k++) {
                char c = line[k];
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) hex = false;
            }
            if (hex) {
                res.append(line.substr(h, 8));
                size_t end = line.find('#', h + 8);
                std::string_view inner = line.substr(h + 8, end == std::string_view::npos ? std::string_view::npos : end - h - 8);
                if (!line_tr(d, inner, res)) res.append(inner); else changed = true;
                if (end == std::string_view::npos) { i = line.size(); break; }
                res.push_back('#');
                i = end + 1;
                continue;
            }
        }
        res.push_back('#');
        i = h + 1;
    }
    if (changed) out.append(res);
    return changed;
}

/* nullptr: sin cambios */
bool compose(const dict_t *d, const char *s, std::string &out) {
    std::string_view all(s);
    bool             changed = false;
    size_t           start   = 0;

    while (true) {
        size_t           nl   = all.find('\n', start);
        std::string_view line = all.substr(start, nl == std::string_view::npos ? std::string_view::npos : nl - start);

        if (line_tr(d, line, out) || recolor_tr(d, line, out)) changed = true;
        else out.append(line);
        if (nl == std::string_view::npos) break;
        out.push_back('\n');
        start = nl + 1;
    }
    return changed;
}

/* Para completar las traducciones: con X6100_I18N_MISSING=<archivo>, cada texto que llega a la
 * pantalla sin traducción se anota una vez (la versión de PC, tools/i18n/missing.py) */
void note_missing(const char *s) {
    static const char *file = getenv("X6100_I18N_MISSING");
    if (!file || !*file) return;
    static std::mutex                      m;
    static std::unordered_map<std::string, bool> seen;
    bool letters = false;
    for (const char *p = s; *p; p++)
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')) { letters = true; break; }
    if (!letters) return;
    std::lock_guard<std::mutex> lock(m);
    if (!seen.emplace(s, true).second) return;
    FILE *f = fopen(file, "a");
    if (!f) return;
    for (const char *p = s; *p; p++) {
        if (*p == '\n') fputs("\\n", f);
        else if (*p == '\t') fputs("\\t", f);
        else fputc(*p, f);
    }
    fputc('\n', f);
    fclose(f);
}

} // namespace

extern "C" const char *tr(const char *s) {
    if (!s || !*s) return s;

    const dict_t *d = cur_dict();
    if (!d) return s;

    const char *t = find(d, std::string_view(s));
    if (t) return t;

    std::string out;
    if (!compose(d, s, out) || out.size() >= RING_LEN) {
        note_missing(s);
        return s;
    }

    char *buf = ring[ring_pos];
    ring_pos  = (ring_pos + 1) % RING_N;
    memcpy(buf, out.c_str(), out.size() + 1);
    return buf;
}

extern "C" const char *tr_static(const char *s) {
    if (!s || !*s) return s;

    const dict_t *d = cur_dict();
    if (!d) return s;

    const char *t = find(d, std::string_view(s));
    if (t) return t;

    std::lock_guard<std::mutex> lock(perm_mutex);
    auto it = perm.find(s);
    if (it != perm.end()) return it->second.c_str();

    std::string out;
    if (!compose(d, s, out)) return s;
    return perm.emplace(s, std::move(out)).first->second.c_str();
}

extern "C" void i18n_set_lang(uint8_t lang) {
    cur_lang = lang < (uint8_t)LANG_COUNT ? lang : (uint8_t)LANG_EN;
}

extern "C" uint8_t i18n_get_lang(void) {
    return cur_lang;
}

extern "C" const char *i18n_lang_name(uint8_t lang) {
    static const char *const names[LANG_COUNT] = {"English", "Español", "Deutsch", "Français", "Italiano"};
    return lang < LANG_COUNT ? names[lang] : "";
}
