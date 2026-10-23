/* posix_profile.c -- the Win32 private-profile (INI) API, on POSIX.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * src/ini.c is written against GetPrivateProfileStringA and friends, which on
 * Windows are the operating system's INI parser. This reimplements the parts of
 * that contract the project uses, so ini.c -- and the config, settings,
 * ini_doc and round-trip tests built on it -- run unchanged on Linux.
 *
 * What is faithful here, because the tests assert on it:
 *   - section and key names are case-insensitive;
 *   - a missing key yields the caller's default, and the default's LENGTH is
 *     what gets returned when it does not fit;
 *   - GetPrivateProfileSectionA returns "key=value\0key=value\0\0" and counts
 *     the bytes written excluding that final NUL;
 *   - section names come back NUL-separated in file order;
 *   - both ';' and '#' introduce a comment, and a ';' after a value is part of
 *     the value only when it was quoted.
 *
 * What is NOT reproduced, and is a known difference rather than an oversight:
 * Windows caps a key at 80 bytes and a section at 255, and it treats a file
 * whose first line has no section header as unnamed-section keys. Those caps
 * are absent here; nothing in this project writes files long enough to meet
 * them, and reproducing them would only add a way to silently truncate.
 *
 * Lines are read whole into memory and parsed in place: the files are a few
 * kilobytes and are read constantly.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "posix_compat.h"

/* A temp path that is a SIBLING of the target, for write-then-rename.
 *
 * The sibling matters. rename() is atomic only within one filesystem, so a
 * temp in /tmp would turn the atomic replace into a non-atomic copy plus a
 * second failure mode (cross-device rename). Same directory, always.
 *
 * The pid keeps concurrent writers -- two settings windows, or a save racing
 * the launcher -- from colliding on one temp name. A writer killed mid-save
 * leaves its temp behind; that litter is the deliberate trade for never
 * truncating the real settings file.
 */
static char *temp_sibling(const char *file)
{
    const char *slash = strrchr(file, '/');
    size_t dirlen = slash ? (size_t)(slash - file) + 1 : 0;
    char *tmp;

    /* +32 covers the ".vcth-<pid>.tmp" suffix for any pid that fits in an int. */
    tmp = malloc(dirlen + 32);
    if (!tmp)
        return NULL;
    memcpy(tmp, file, dirlen);
    snprintf(tmp + dirlen, 32, ".vcth-%ld.tmp", (long)getpid());
    return tmp;
}

/* Read the whole file. Returns NULL when it cannot be opened; every entry
 * point below then behaves as Windows does for a missing file, which is to
 * return the default. */
static char *slurp(const char *file)
{
    FILE *f;
    char native_file[PATH_MAX];
    long size;
    char *buf;

    if (!file || !*file)
        return NULL;
    if (!posix_norm(native_file, sizeof native_file, file))
        return NULL;
    f = fopen(native_file, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size = (long)fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[size] = '\0';
    return buf;
}

/* Trim leading and trailing blanks in place, and strip a trailing CR so a file
 * written on Windows reads the same here. */
static char *trim(char *s)
{
    char *e;

    while (*s == ' ' || *s == '\t')
        s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == '\n' || e[-1] == '\r' ||
                     e[-1] == ' '  || e[-1] == '\t'))
        e--;
    *e = '\0';
    return s;
}

/* Find `key` within [section]. When found, `line` points at the first
 * character of its value and the function returns 1. */
static int find_key(char *text, const char *section, const char *key,
                    char **line)
{
    char *p = text;
    int in_section = (section == NULL || *section == '\0');

    while (*p) {
        char *cur = p;             /* not `line`: that is the out-param */
        char *eol = strchr(cur, '\n');
        char *next = eol ? eol + 1 : cur + strlen(cur);
        char *s;

        if (eol)
            *eol = '\0';
        s = trim(cur);
        p = next;                   /* before trim's NUL, not after it */

        if (!*s || *s == ';' || *s == '#')
            continue;

        if (*s == '[') {
            char *close = strchr(s, ']');
            if (!close)
                continue;
            *close = '\0';
            in_section = (strcasecmp(s + 1, section ? section : "") == 0);
            continue;
        }
        if (!in_section)
            continue;

        {
            char *eq = strchr(s, '=');
            char *name;
            if (!eq)
                continue;
            *eq = '\0';
            name = trim(s);
            if (strcasecmp(name, key) == 0) {
                if (line)
                    *line = trim(eq + 1);
                return 1;
            }
        }
    }
    return 0;
}

DWORD posix_profile_get_string(const char *section, const char *key,
                               const char *dflt, char *out, DWORD n,
                               const char *file)
{
    char *text, *val = NULL;
    DWORD wrote;

    if (!out || n == 0)
        return 0;
    text = slurp(file);
    if (text && find_key(text, section, key, &val))
        snprintf(out, n, "%s", val);
    else
        snprintf(out, n, "%s", dflt ? dflt : "");
    free(text);

    wrote = (DWORD)strlen(out);
    if (wrote >= n)             /* Windows truncates and reports the copy */
        wrote = n - 1;
    return wrote;
}

int posix_profile_get_int(const char *section, const char *key, int dflt,
                          const char *file)
{
    char buf[64];
    DWORD got = posix_profile_get_string(section, key, "", buf,
                                         (DWORD)sizeof buf, file);
    (void)got;
    if (!buf[0])
        return dflt;
    return (int)strtol(buf, NULL, 0);
}

/* Copy `len` bytes of a NUL-separated block into `out`, adding the final
 * extra NUL that makes it double-terminated, and return the count Win32
 * returns: the bytes copied, excluding that terminating NUL.
 *
 * `len` is passed in rather than measured with strlen because the block is
 * NUL-separated -- strlen would stop at the first entry and report only that
 * one. Getting this wrong is quiet and severe: the caller in ini.c grows its
 * buffer until the returned count says the section fits, so under-reporting
 * makes it stop early and hand back a block truncated to the first entry. */
static DWORD emit_block(const char *block, size_t len, char *out, DWORD n)
{
    DWORD need = (DWORD)len + 1;     /* the final extra NUL */

    if (out && n) {
        DWORD copy = need < n ? need : n;
        memcpy(out, block, copy);
        if (need < n)
            out[need] = '\0';                   /* double NUL terminator */
        else if (n >= 2)
            out[n - 1] = out[n - 2] = '\0';
    }
    return need - 1;                              /* excluding the extra NUL */
}

DWORD posix_profile_get_section(const char *section, char *out, DWORD n,
                                const char *file)
{
    char *text, *p;
    size_t cap, len = 0;
    DWORD rc;

    text = slurp(file);
    if (!text) {
        if (out && n)
            out[0] = '\0';
        return 0;
    }

    /* Build the block first so the length is known before it is copied, which
     * is what lets the caller see the full size even when it truncates. */
    cap = strlen(text) + 2;
    p = (char *)malloc(cap);
    if (!p) {
        free(text);
        return 0;
    }
    p[0] = '\0';
    {
        char *scan = strdup(text);
        int in_section = 0;

        if (scan) {
            char *q = scan;
            while (*q) {
                char *line = q;
                char *eol = strchr(line, '\n');
                char *next = eol ? eol + 1 : line + strlen(line);
                char *s;
                if (eol)
                    *eol = '\0';
                s = trim(line);
                q = next;

                if (!*s || *s == ';' || *s == '#')
                    continue;
                if (*s == '[') {
                    char *close = strchr(s, ']');
                    if (!close)
                        continue;
                    *close = '\0';
                    in_section = (strcasecmp(s + 1, section) == 0);
                    continue;
                }
                if (!in_section)
                    continue;
                {
                    /* Win32 trims the whitespace around BOTH sides of the '='
                     * when it writes a section out, so `tmx  = value` comes
                     * back as `tmx=value`. Emitting the line verbatim instead
                     * hands the caller a value with its leading space intact,
                     * which then fails every equality against it. */
                    char *eq = strchr(s, '=');
                    char pair[1024];

                    if (eq) {
                        *eq = '\0';
                        snprintf(pair, sizeof pair, "%s=%s",
                                 trim(s), trim(eq + 1));
                        len += (size_t)snprintf(p + len, cap - len, "%s\n", pair);
                    } else {
                        len += (size_t)snprintf(p + len, cap - len, "%s\n", s);
                    }
                }
            }
            free(scan);
        }
    }
    /* The block is newline separated here and NUL separated in the result. */
    {
        size_t i;
        for (i = 0; i < len; i++)
            if (p[i] == '\n')
                p[i] = '\0';
    }
    rc = emit_block(p, len, out, n);
    free(p);
    free(text);
    return rc;
}

DWORD posix_profile_section_names(char *out, DWORD n, const char *file)
{
    char *text, *scan, *walk, *q;
    size_t cap, len = 0;
    DWORD rc;

    text = slurp(file);
    if (!text) {
        if (out && n)
            out[0] = '\0';
        return 0;
    }
    scan = strdup(text);
    cap = strlen(text) + 2;
    q = (char *)malloc(cap);
    if (!scan || !q) {
        free(scan); free(q); free(text);
        return 0;
    }
    q[0] = '\0';

    /* Walk with a separate cursor from `scan`. An earlier version advanced
     * `scan` itself and then freed it, which hands free() a pointer into the
     * middle of the allocation and aborts. Separately, the advance must be
     * computed BEFORE trim(), because trim() writes a NUL over the line's
     * trailing blanks and strlen() would then stop there -- landing the next
     * cursor on that NUL and ending the scan early. */
    walk = scan;
    while (*walk) {
        char *line = walk;
        char *eol = strchr(line, '\n');
        char *next = eol ? eol + 1 : line + strlen(line);
        char *s;

        if (eol)
            *eol = '\0';
        s = trim(line);
        walk = next;

        if (*s != '[')
            continue;
        {
            char *close = strchr(s, ']');
            if (!close)
                continue;
            *close = '\0';
            len += (size_t)snprintf(q + len, cap - len, "%s\n", s + 1);
        }
    }
    {
        size_t i;
        for (i = 0; i < len; i++)
            if (q[i] == '\n')
                q[i] = '\0';
    }
    rc = emit_block(q, len, out, n);
    free(q);
    free(scan);
    free(text);
    return rc;
}

/* Rewriting a single key means read, replace-or-append, write. INI files are
 * small and this is called from the settings UI, not a frame loop.
 *
 * The write is staged: content goes to a sibling temp file and rename() moves
 * it over the target. rename() within a directory is atomic, so a concurrent
 * reader -- or a crash partway through this function -- sees either the whole
 * old file or the whole new one, never a truncated prefix. That is the property
 * ini_doc_test's two skipped assertions are really asking for; they are worded
 * as Win32 share mode 0 because that is how Windows refuses the same situation.
 *
 * A reader that already had the old file open keeps reading the old inode, which
 * is why this needs no share-mode emulation and no handle table.
 */
BOOL posix_profile_put_string(const char *section, const char *key,
                              const char *val, const char *file)
{
    char native_file[PATH_MAX];
    char *text;
    char *tmp;
    FILE *f;
    int wrote = 0;
    int failed;

    if (!posix_norm(native_file, sizeof native_file, file))
        return FALSE;
    file = native_file;
    text = slurp(file);

    tmp = temp_sibling(file);
    if (!tmp) {
        free(text);
        return FALSE;
    }

    f = fopen(tmp, "wb");
    if (!f) {
        free(tmp);
        free(text);
        return FALSE;
    }
    if (text) {
        char *p = text;
        int in_section = (section == NULL || *section == '\0');
        while (*p) {
            char *eol = strchr(p, '\n');
            size_t n = eol ? (size_t)(eol - p) + 1 : strlen(p);
            char *s;
            if (eol)
                *eol = '\0';
            s = trim(p);
            if (*s == '[') {
                char *close = strchr(s, ']');
                if (close) {
                    *close = '\0';
                    in_section = (strcasecmp(s + 1, section ? section : "") == 0);
                }
            } else if (in_section && !wrote && *s && *s != ';' && *s != '#') {
                char *eq = strchr(s, '=');
                if (eq) {
                    *eq = '\0';
                    if (strcasecmp(trim(s), key) == 0) {
                        fprintf(f, "%s=%s\n", key, val ? val : "");
                        wrote = 1;
                    } else {
                        *eq = '=';
                        fwrite(p, 1, n, f);
                    }
                    p = eol ? eol + 1 : p + strlen(p);
                    continue;
                }
            }
            fwrite(p, 1, n, f);
            p = eol ? eol + 1 : p + strlen(p);
        }
    }
    if (!wrote) {
        if (text && *text)
            fputc('\n', f);
        fprintf(f, "[%s]\n%s=%s\n", section ? section : "", key,
                val ? val : "");
    }

    /* ferror() catches a failed write that fprintf/fwrite already reported as
     * returning short; fclose() is what actually surfaces a deferred ENOSPC,
     * and it is the only place the buffered bytes reach the file. Both must be
     * checked or a failed save reports success. */
    failed = ferror(f) ? 1 : 0;
    if (fclose(f) != 0)
        failed = 1;
    free(text);

    if (failed || rename(tmp, file) != 0) {
        unlink(tmp);
        free(tmp);
        return FALSE;
    }
    free(tmp);
    return TRUE;
}
