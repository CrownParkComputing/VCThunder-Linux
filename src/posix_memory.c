/* Process-wide mappings for the host and the game's reserve/commit API. */
/* SPDX-License-Identifier: MIT */
#include "posix_compat.h"

typedef struct mapping {
    void *base;
    size_t size;
    DWORD protect;
    struct mapping *next;
} mapping;

static mapping *s_maps;
static pthread_mutex_t s_map_lock = PTHREAD_MUTEX_INITIALIZER;

static int page_prot(DWORD protect)
{
    int p = 0;
    if (protect != PAGE_NOACCESS) p |= PROT_READ;
    if (protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) p |= PROT_WRITE;
    if (protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) p |= PROT_EXEC;
    return p;
}

static mapping *find_map(const void *addr, size_t size)
{
    mapping *m;
    uintptr_t at = (uintptr_t)addr;
    for (m = s_maps; m; m = m->next)
        if (at >= (uintptr_t)m->base && at - (uintptr_t)m->base <= m->size &&
            size <= m->size - (at - (uintptr_t)m->base))
            return m;
    return NULL;
}

LPVOID VirtualAlloc(LPVOID addr, size_t size, DWORD type, DWORD protect)
{
    size_t offset = (uintptr_t)addr & 4095u;
    size_t len;
    void *p;
    mapping *m;
    if (!size || size > SIZE_MAX - 65536u - offset) return NULL;
    len = (size + offset + 4095u) & ~(size_t)4095u;
    addr = (void *)((uintptr_t)addr & ~(uintptr_t)4095u);
    pthread_mutex_lock(&s_map_lock);
    if (addr && (m = find_map(addr, len)) != NULL && !(type & MEM_RESERVE)) {
        int ok = mprotect(addr, len, page_prot(protect)) == 0;
        if (ok) m->protect = protect;
        pthread_mutex_unlock(&s_map_lock);
        return ok ? addr : NULL;
    }
    m = malloc(sizeof *m);
    if (!m) { pthread_mutex_unlock(&s_map_lock); return NULL; }
    if (addr) {
        p = mmap(addr, len, (type & MEM_COMMIT) ? page_prot(protect) : PROT_NONE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    } else {
        /* Windows reservations start on 64 KiB boundaries. */
        void *raw = mmap(NULL, len + 65536, (type & MEM_COMMIT) ? page_prot(protect) : PROT_NONE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        p = raw;
        if (raw != MAP_FAILED) {
            size_t lead;
            p = (void *)(((uintptr_t)raw + 65535u) & ~(uintptr_t)65535u);
            lead = (uintptr_t)p - (uintptr_t)raw;
            if (lead) munmap(raw, lead);
            munmap((char *)p + len, 65536 - lead);
        }
    }
    if (p == MAP_FAILED || (addr && p != addr)) {
        if (p != MAP_FAILED) munmap(p, len);
        free(m);
        pthread_mutex_unlock(&s_map_lock);
        return NULL;
    }
    *m = (mapping){ p, len, protect, s_maps };
    s_maps = m;
    pthread_mutex_unlock(&s_map_lock);
    return p;
}

BOOL VirtualFree(LPVOID addr, size_t size, DWORD type)
{
    mapping **link, *m;
    int ok = 0;
    pthread_mutex_lock(&s_map_lock);
    for (link = &s_maps; *link; link = &(*link)->next) {
        m = *link;
        if (type & MEM_RELEASE) {
            if (m->base != addr || size) continue;
            ok = munmap(m->base, m->size) == 0;
            if (ok) { *link = m->next; free(m); }
            break;
        }
        if (find_map(addr, size) == m && size) {
            size_t len = (size + 4095u) & ~(size_t)4095u;
            ok = mprotect(addr, len, PROT_NONE) == 0;
            if (ok) madvise(addr, len, MADV_DONTNEED);
            break;
        }
    }
    pthread_mutex_unlock(&s_map_lock);
    return ok;
}

BOOL VirtualProtect(LPVOID addr, size_t size, DWORD protect, DWORD *old)
{
    uintptr_t base = (uintptr_t)addr & ~(uintptr_t)4095u;
    size_t len;
    mapping *m;
    int ok;
    if (!size || size > SIZE_MAX - 8192u) return FALSE;
    len = (size + (uintptr_t)addr - base + 4095u) & ~(size_t)4095u;
    pthread_mutex_lock(&s_map_lock);
    m = find_map((void *)base, len);
    if (old) *old = m ? m->protect : PAGE_EXECUTE_READWRITE;
    ok = mprotect((void *)base, len, page_prot(protect)) == 0;
    if (ok && m) m->protect = protect;
    pthread_mutex_unlock(&s_map_lock);
    return ok;
}
