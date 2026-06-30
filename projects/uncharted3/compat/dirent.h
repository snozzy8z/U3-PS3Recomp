/* ps3recomp - shim <dirent.h> minimal pour Windows/MSVC (opendir/readdir/
 * closedir via Win32). Suffisant pour ppu_fs.cpp (n'utilise que d_name). */
#ifndef PS3RECOMP_COMPAT_DIRENT_H
#define PS3RECOMP_COMPAT_DIRENT_H

#if defined(_WIN32)
#include <windows.h>
#include <stdlib.h>
#include <string.h>

struct dirent { char d_name[MAX_PATH]; };

typedef struct DIR {
    HANDLE           h;
    WIN32_FIND_DATAA fd;
    int              first;
    struct dirent    ent;
} DIR;

static DIR* opendir(const char* path)
{
    if (!path || !*path) return NULL;
    size_t n = strlen(path);
    char* pat = (char*)malloc(n + 3);
    if (!pat) return NULL;
    memcpy(pat, path, n);
    size_t k = n;
    if (!(k && (pat[k-1] == '/' || pat[k-1] == '\\'))) pat[k++] = '\\';
    pat[k++] = '*';
    pat[k]   = 0;
    DIR* d = (DIR*)calloc(1, sizeof(DIR));
    if (!d) { free(pat); return NULL; }
    d->h = FindFirstFileA(pat, &d->fd);
    free(pat);
    if (d->h == INVALID_HANDLE_VALUE) { free(d); return NULL; }
    d->first = 1;
    return d;
}

static struct dirent* readdir(DIR* d)
{
    if (!d) return NULL;
    if (!d->first) { if (!FindNextFileA(d->h, &d->fd)) return NULL; }
    d->first = 0;
    strncpy(d->ent.d_name, d->fd.cFileName, sizeof d->ent.d_name - 1);
    d->ent.d_name[sizeof d->ent.d_name - 1] = 0;
    return &d->ent;
}

static int closedir(DIR* d)
{
    if (!d) return -1;
    if (d->h != INVALID_HANDLE_VALUE) FindClose(d->h);
    free(d);
    return 0;
}
#endif /* _WIN32 */
#endif
