#pragma once

/*
===============================================================================

mu_files.h — path helpers + atomic file IO for engine/tools code.

No hidden allocation for path manipulation: every path routine writes into a
caller-owned buffer and returns the required NUL-terminated length (0 = buffer
too small). File IO allocates only at explicit read/write points and routes
everything through MU_MALLOC / MU_FREE.

Atomic write contract: write to a temp file in the same directory, fsync,
rename over the target. A crash mid-write cannot clobber the previous file.

-------------------------------------------------------------------------------
Platform notes
-------------------------------------------------------------------------------
POSIX:    unistd.h, sys/stat.h, fcntl.h, sys/mman.h
Windows:  windows.h, io.h, direct.h
===============================================================================
*/

#if !defined(_POSIX_C_SOURCE) && !defined(_DEFAULT_SOURCE) && !defined(__cplusplus) && !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif

#include "mu_common.h"

#if defined(_WIN32)
#  include <windows.h>
#  include <io.h>
#  include <direct.h>
#  define MU_FILES_PATH_SEP '\\'
#  define MU_FILES_PATH_SEP_C "\\"
#else
#  include <unistd.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <fcntl.h>
#  include <sys/mman.h>
#  define MU_FILES_PATH_SEP '/'
#  define MU_FILES_PATH_SEP_C "/"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

MU_BEGIN_EXTERN_C

/* ===========================================================================
 * Path manipulation (caller-owned buffers, no allocation)
 * =========================================================================== */

/* Append file onto dir with exactly one separator between them.
 * Returns required NUL-terminated length, or 0 if it does not fit in out_cap.
 */
MU_INLINE uint32_t mu_files_path_join(char* MU_RESTRICT out, uint32_t out_cap,
                                      const char* dir, const char* file)
{
    uint32_t dlen = (uint32_t)strlen(dir);
    uint32_t flen = (uint32_t)strlen(file);
    uint32_t sep  = (dlen > 0 && dir[dlen - 1] != MU_FILES_PATH_SEP) ? 1u : 0u;
    uint32_t need = dlen + sep + flen + 1u; /* + NUL */

    if(out_cap < need)
        return need; /* caller can retry with a bigger buffer */

    MU_MEMCPY(out, dir, dlen);
    uint32_t w = dlen;
    if(sep)
        out[w++] = MU_FILES_PATH_SEP;
    MU_MEMCPY(out + w, file, flen);
    out[w + flen] = '\0';
    return need;
}

/* Collapse //, strip trailing separators, resolve ./ and ../ lexically
 * (no filesystem symlinks). Returns required NUL-terminated length.
 */
MU_INLINE uint32_t mu_files_path_normalize(char* MU_RESTRICT out, uint32_t out_cap,
                                           const char* path)
{
    uint32_t n = (uint32_t)strlen(path);
    if(out_cap < 2u)
        return 2u;

    /* Stack of path component offsets into `path`. */
    uint32_t comp[1024];
    uint32_t comp_count = 0;
    uint32_t i = 0;

    while(i < n)
    {
        /* Skip separators. */
        while(i < n && path[i] == MU_FILES_PATH_SEP)
            ++i;
        if(i >= n)
            break;

        uint32_t start = i;
        while(i < n && path[i] != MU_FILES_PATH_SEP)
            ++i;
        uint32_t len = i - start;

        if(len == 1u && path[start] == '.')
        {
            /* current dir — drop */
            continue;
        }
        if(len == 2u && path[start] == '.' && path[start + 1] == '.')
        {
            if(comp_count > 0)
                --comp_count; /* pop */
            continue;
        }
        comp[comp_count++] = start;
        /* component length handled via next start; store length implicitly */
        /* We need lengths too — pack them: use a parallel array would be cleaner,
         * but to keep this header small we store (start << 16) | len. */
        comp[comp_count - 1] = (start << 16) | len;
    }

    /* Write out. */
    uint32_t w = 0;
    for(uint32_t c = 0; c < comp_count; ++c)
    {
        uint32_t start = comp[c] >> 16;
        uint32_t len   = comp[c] & 0xFFFFu;
        if(w > 0u)
            out[w++] = MU_FILES_PATH_SEP;
        if(w + len > out_cap - 1u)
            return w + len + 1u + (comp_count - c - 1u) * 2u; /* rough overestimate */
        MU_MEMCPY(out + w, path + start, len);
        w += len;
    }
    if(w == 0u)
    {
        out[w++] = '.';
    }
    out[w] = '\0';
    return w + 1u;
}

/* Last path component after the final separator. Writes into out, returns
 * required NUL-terminated length. Points at the separator itself if there
 * is no basename (e.g. "dir/") — returns length 1 with out[0] = 0. */
MU_INLINE uint32_t mu_files_basename(char* MU_RESTRICT out, uint32_t out_cap,
                                     const char* path)
{
    uint32_t n = (uint32_t)strlen(path);
    uint32_t cut = n;
    while(cut > 0u && path[cut - 1] == MU_FILES_PATH_SEP)
        --cut; /* strip trailing separators */
    uint32_t start = cut;
    while(start > 0u && path[start - 1] != MU_FILES_PATH_SEP)
        --start;
    uint32_t len = cut - start;
    uint32_t need = len + 1u;
    if(out_cap < need)
        return need;
    if(len > 0u)
        MU_MEMCPY(out, path + start, len);
    out[len] = '\0';
    return need;
}

/* Directory portion: everything up to and including the last separator.
 * Returns required NUL-terminated length. "file" -> "." (length 2). */
MU_INLINE uint32_t mu_files_dirname(char* MU_RESTRICT out, uint32_t out_cap,
                                    const char* path)
{
    uint32_t n = (uint32_t)strlen(path);
    uint32_t cut = n;
    while(cut > 0u && path[cut - 1] == MU_FILES_PATH_SEP)
        --cut;
    uint32_t sep = cut;
    while(sep > 0u && path[sep - 1] != MU_FILES_PATH_SEP)
        --sep;
    uint32_t len = sep; /* characters [0, sep) */
    if(len == 0u)
    {
        len = 1u; /* "." */
    }
    uint32_t need = len + 1u;
    if(out_cap < need)
        return need;
    if(len > 1u)
        MU_MEMCPY(out, path, len - 1u);
    out[len - 1u] = '.';
    out[len] = '\0';
    return need;
}

/* Extension including the leading dot, or "" if none. Returns NUL-terminated
 * length (0 means extension is empty, still fits any buffer). */
MU_INLINE uint32_t mu_files_extension(char* MU_RESTRICT out, uint32_t out_cap,
                                      const char* path)
{
    /* basename first */
    char base[1024];
    uint32_t blen = mu_files_basename(base, (uint32_t)MU_ARRAY_COUNT(base), path);
    (void)blen;

    uint32_t n = (uint32_t)strlen(base);
    /* Find the last '.' in the basename. */
    uint32_t dot = n;
    bool found_dot = false;
    while(dot > 0u)
    {
        --dot;
        if(base[dot] == '.')
        {
            found_dot = true;
            break;
        }
    }
    /* No dot, or dot is the first char (hidden file like ".gitignore"),
     * or dot is the last char (trailing dot): no extension. */
    if(!found_dot || dot == 0u || dot == n - 1u)
    {
        if(out_cap < 1u)
            return 1u;
        out[0] = '\0';
        return 1u;
    }
    uint32_t elen = n - dot; /* includes the dot */
    uint32_t need = elen + 1u;
    if(out_cap < need)
        return need;
    MU_MEMCPY(out, base + dot, elen);
    out[elen] = '\0';
    return need;
}

/* True if the path exists on disk (file, directory, or symlink). */
MU_INLINE bool mu_files_path_exists(const char* path)
{
#if defined(_WIN32)
    DWORD attr = GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return stat(path, &st) == 0;
#endif
}

/* Make all intermediate directories (like mkdir -p). Returns true on success. */
MU_INLINE bool mu_files_mkdirs(const char* path)
{
#if defined(_WIN32)
    char tmp[4096];
    uint32_t len = (uint32_t)strlen(path);
    if(len == 0u || len >= (uint32_t)MU_ARRAY_COUNT(tmp))
        return false;
    MU_MEMCPY(tmp, path, len);
    tmp[len] = '\0';
    for(uint32_t i = 1u; i < len; ++i)
    {
        if(tmp[i] == '/' || tmp[i] == '\\')
        {
            char c = tmp[i];
            tmp[i] = '\0';
            if(!CreateDirectoryA(tmp, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
            {
                tmp[i] = c;
                return false;
            }
            tmp[i] = c;
        }
    }
    if(!CreateDirectoryA(tmp, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        return false;
    return true;
#else
    char tmp[4096];
    uint32_t len = (uint32_t)strlen(path);
    if(len == 0u || len >= (uint32_t)MU_ARRAY_COUNT(tmp))
        return false;
    MU_MEMCPY(tmp, path, len);
    tmp[len] = '\0';
    for(uint32_t i = 1u; i < len; ++i)
    {
        if(tmp[i] == '/')
        {
            tmp[i] = '\0';
            if(mkdir(tmp, 0755) != 0 && errno != EEXIST)
            {
                tmp[i] = '/';
                return false;
            }
            tmp[i] = '/';
        }
    }
    if(mkdir(tmp, 0755) != 0 && errno != EEXIST)
        return false;
    return true;
#endif
}

/* ===========================================================================
 * Whole-file read (malloc'd, caller frees with MU_FREE)
 * =========================================================================== */

/* Read entire file into *out_data (*out_size bytes). Returns true on success.
 * On failure *out_data is NULL and *out_size is 0.
 */
MU_INLINE bool mu_files_read_all(const char* path, void** MU_RESTRICT out_data,
                                 uint64_t* MU_RESTRICT out_size)
{
    *out_data = NULL;
    *out_size = 0;

#if defined(_WIN32)
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if(h == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER sz;
    if(!GetFileSizeEx(h, &sz))
    {
        CloseHandle(h);
        return false;
    }
    uint64_t size = (uint64_t)sz.QuadPart;
    void* buf = NULL;
    if(size > 0u)
    {
        buf = MU_MALLOC((size_t)size);
        if(!buf)
        {
            CloseHandle(h);
            return false;
        }
        DWORD read = 0;
        if(!ReadFile(h, buf, (DWORD)size, &read, NULL) || (uint64_t)read != size)
        {
            MU_FREE(buf);
            CloseHandle(h);
            return false;
        }
    }
    CloseHandle(h);
    *out_data = buf;
    *out_size = size;
    return true;
#else
    int fd = open(path, O_RDONLY);
    if(fd < 0)
        return false;
    struct stat st;
    if(fstat(fd, &st) != 0)
    {
        close(fd);
        return false;
    }
    uint64_t size = (uint64_t)st.st_size;
    void* buf = NULL;
    if(size > 0u)
    {
        buf = MU_MALLOC((size_t)size);
        if(!buf)
        {
            close(fd);
            return false;
        }
        uint64_t off = 0;
        while(off < size)
        {
            ssize_t r = read(fd, (char*)buf + off, (size_t)(size - off));
            if(r <= 0)
            {
                MU_FREE(buf);
                close(fd);
                return false;
            }
            off += (uint64_t)r;
        }
    }
    close(fd);
    *out_data = buf;
    *out_size = size;
    return true;
#endif
}

/* ===========================================================================
 * Atomic write (temp file -> fsync -> rename)
 * =========================================================================== */

/* Write `size` bytes atomically to `path`. Writes to "<path>.tmp.<pid>" in the
 * same directory, fsyncs, then renames over `path`. The parent directory must
 * already exist. Returns true on success. */
MU_INLINE bool mu_files_write_atomic(const char* path, const void* data,
                                     uint64_t size)
{
    /* Build temp name next to the target so rename() stays on one filesystem. */
    char tmp[4096];
    uint32_t plen = (uint32_t)strlen(path);
    if(plen + 64 > (uint32_t)sizeof(tmp))
        return false;
    MU_MEMCPY(tmp, path, plen);
    /* ".tmp" suffix is enough; rename is atomic on the same filesystem. */
    tmp[plen++] = '.';
    tmp[plen++] = 't';
    tmp[plen++] = 'm';
    tmp[plen++] = 'p';
    tmp[plen] = '\0';

#if defined(_WIN32)
    HANDLE h = CreateFileA(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if(h == INVALID_HANDLE_VALUE)
        return false;
    bool ok = true;
    if(size > 0u)
    {
        DWORD written = 0;
        if(!WriteFile(h, data, (DWORD)size, &written, NULL) || (uint64_t)written != size)
            ok = false;
    }
    if(ok)
    {
        /* FlushFileBuffers == fsync on Windows. */
        if(!FlushFileBuffers(h))
            ok = false;
    }
    CloseHandle(h);
    if(!ok)
    {
        DeleteFileA(tmp);
        return false;
    }
    if(!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        DeleteFileA(tmp);
        return false;
    }
    return true;
#else
    /* mkstemp needs a writable template ending in "XXXXXX". */
    char tmpl[4096];
    uint32_t tlen = (uint32_t)snprintf(tmpl, sizeof(tmpl), "%s.tmp.XXXXXX", path);
    if(tlen >= (uint32_t)sizeof(tmpl))
        return false;
    int fd = mkstemp(tmpl);
    if(fd < 0)
        return false;
    bool ok = true;
    if(size > 0u)
    {
        uint64_t off = 0;
        while(off < size)
        {
            ssize_t w = write(fd, (const char*)data + off, (size_t)(size - off));
            if(w <= 0)
            {
                ok = false;
                break;
            }
            off += (uint64_t)w;
        }
    }
    if(ok && fsync(fd) != 0)
        ok = false;
    close(fd);
    if(!ok)
    {
        unlink(tmpl);
        return false;
    }
    if(rename(tmpl, path) != 0)
    {
        unlink(tmpl);
        return false;
    }
    return true;
#endif
}

/* ===========================================================================
 * Memory-mapped read (mmap). Caller calls mu_files_mmap_unmap when done.
 * =========================================================================== */

typedef struct mu_files_mmap
{
    void*   data;
    uint64_t size;
#if defined(_WIN32)
    HANDLE h_file;
    HANDLE h_map;
#else
    int fd;
#endif
} mu_files_mmap;

/* Map `path` for reading. *out is zeroed on failure. Returns true on success.
 * An empty file maps to data==NULL, size==0 (still "success"). */
MU_INLINE bool mu_files_mmap_read(const char* path, mu_files_mmap* MU_RESTRICT out)
{
    out->data = NULL;
    out->size = 0;
#if defined(_WIN32)
    out->h_file = INVALID_HANDLE_VALUE;
    out->h_map  = NULL;
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if(h == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER sz;
    if(!GetFileSizeEx(h, &sz))
    {
        CloseHandle(h);
        return false;
    }
    uint64_t size = (uint64_t)sz.QuadPart;
    out->h_file = h;
    out->size   = size;
    if(size == 0u)
        return true; /* nothing to map */
    HANDLE m = CreateFileMappingA(h, NULL, PAGE_READONLY, 0, 0, NULL);
    if(!m)
    {
        CloseHandle(h);
        return false;
    }
    out->h_map = m;
    void* view = MapViewOfFile(m, FILE_MAP_READ, 0, 0, (SIZE_T)size);
    if(!view)
    {
        CloseHandle(m);
        CloseHandle(h);
        return false;
    }
    out->data = view;
    return true;
#else
    out->fd = -1;
    int fd = open(path, O_RDONLY);
    if(fd < 0)
        return false;
    struct stat st;
    if(fstat(fd, &st) != 0)
    {
        close(fd);
        return false;
    }
    uint64_t size = (uint64_t)st.st_size;
    out->fd  = fd;
    out->size = size;
    if(size == 0u)
        return true;
    void* map = mmap(NULL, (size_t)size, PROT_READ, MAP_PRIVATE, fd, 0);
    if(map == MAP_FAILED)
    {
        close(fd);
        return false;
    }
    out->data = map;
    return true;
#endif
}

/* Unmap and close. Safe to call on a zeroed / failed handle. */
MU_INLINE void mu_files_mmap_unmap(mu_files_mmap* MU_RESTRICT mm)
{
    if(mm->data != NULL && mm->size > 0u)
    {
#if defined(_WIN32)
        UnmapViewOfFile(mm->data);
#else
        munmap(mm->data, (size_t)mm->size);
#endif
        mm->data = NULL;
    }
#if defined(_WIN32)
    if(mm->h_map != NULL)
    {
        CloseHandle(mm->h_map);
        mm->h_map = NULL;
    }
    if(mm->h_file != INVALID_HANDLE_VALUE)
    {
        CloseHandle(mm->h_file);
        mm->h_file = INVALID_HANDLE_VALUE;
    }
#else
    if(mm->fd >= 0)
    {
        close(mm->fd);
        mm->fd = -1;
    }
#endif
    mm->size = 0;
}

MU_END_EXTERN_C
