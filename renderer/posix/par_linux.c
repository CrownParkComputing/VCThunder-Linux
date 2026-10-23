/* par_linux.c -- the raster worker pool, on pthreads.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * A direct translation of renderer/win32/thread.c, which owns the scheduling
 * policy and whose comments explain why it is shaped the way it is. The split
 * is by contiguous scanline rows and per-pixel access order is unchanged, so
 * this produces the same pixels as the Win32 pool -- which is what lets the
 * conformance hash be a meaningful comparison rather than a new baseline.
 *
 * What changed: Win32 threading primitives became pthreads and C11 atomics.
 * YieldProcessor becomes x86 PAUSE; SwitchToThread becomes
 * sched_yield(); the per-worker wake event becomes a condition variable with a
 * predicate, because a bare flag would race the sleep exactly where the Win32
 * version's SetEvent/WaitForSingleObject pair cannot.
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>

#include "../core/vgl.h"
#include "../core/vcglide.h"

#define SPIN_YIELDS   4000
#define YIELD_ROUNDS  2000

typedef struct {
    pthread_t       thread;
    pthread_mutex_t lock;
    pthread_cond_t  wake;
    int             index;
    int             y0, y1;
    int             sleeping;
} worker_t;

static worker_t      s_worker[VGL_PAR_MAX];
static int           s_workers;        /* including the submitting thread */
static atomic_int    s_gen;            /* bumped once per job */
static atomic_int    s_done;           /* workers finished with this job */
static atomic_int    s_quit;
static vgl_par_fn    s_fn;
static void         *s_ctx;

int vgl_par_workers(void)
{
    return s_workers < 1 ? 1 : s_workers;
}

/* Size the pool by physical cores, as the Win32 build does: logical CPUs
 * include SMT siblings, and one raster worker per sibling buys contention
 * rather than throughput. */
static int physical_cores(void)
{
    FILE *f = fopen("/proc/cpuinfo", "r");
    char line[256];
    int logical = 0, cores = 0;
    long ids[1024][2];
    long phys = -1, core = -1;

    if (!f)
        return (int)sysconf(_SC_NPROCESSORS_ONLN);

    /* Count distinct (physical id, core id) pairs. cpuinfo repeats the same
     * core id across the SMT siblings of one physical package, which is
     * exactly the duplication to collapse. */
    while (fgets(line, sizeof line, f)) {
        long v;
        if (sscanf(line, "processor : %ld", &v) == 1 ||
            sscanf(line, "processor:%ld", &v) == 1)
            { logical++; phys = -1; }
        if (sscanf(line, "physical id : %ld", &v) == 1 ||
            sscanf(line, "physical id:%ld", &v) == 1) {
            phys = v;
        }
        if (sscanf(line, "core id : %ld", &v) == 1 ||
            sscanf(line, "core id:%ld", &v) == 1)
        {
            core = v;
            if (phys >= 0) {
                int i;
                for (i = 0; i < cores; i++)
                    if (ids[i][0] == phys && ids[i][1] == core) break;
                if (i == cores && cores < (int)(sizeof ids / sizeof *ids)) {
                    ids[cores][0] = phys; ids[cores][1] = core; cores++;
                }
            }
        }
    }
    fclose(f);

    if (cores < 1)
        cores = logical;
    if (cores < 1)
        cores = (int)sysconf(_SC_NPROCESSORS_ONLN);
    return cores;
}

static void *worker_main(void *arg)
{
    worker_t *w = (worker_t *)arg;
    int seen = 0;

    for (;;) {
        long spins = 0, yields = 0;

        while (atomic_load(&s_gen) == seen) {
            if (atomic_load(&s_quit))
                return NULL;
            if (++spins < SPIN_YIELDS) {
                __builtin_ia32_pause();
            } else if (++yields < YIELD_ROUNDS) {
                sched_yield();
            } else {
                pthread_mutex_lock(&w->lock);
                w->sleeping = 1;
                /* The predicate is re-tested under the lock, so a generation
                 * bump that lands between the spin giving up and the wait
                 * beginning cannot leave the worker asleep past its job. */
                if (atomic_load(&s_gen) == seen)
                    pthread_cond_wait(&w->wake, &w->lock);
                w->sleeping = 0;
                pthread_mutex_unlock(&w->lock);
                yields = 0;
                spins = 0;
            }
        }
        seen = atomic_load(&s_gen);
        if (atomic_load(&s_quit))
            return NULL;
        if (w->y1 > w->y0)
            s_fn(s_ctx, w->index, w->y0, w->y1);
        atomic_fetch_add(&s_done, 1);
    }
}

int vgl_par_open(int want)
{
    int i, cores;

    if (s_workers)
        return s_workers;

    /* A pool is a new generation namespace. vgl_par_close bumps s_gen to wake
     * the old workers; leaving that value in place would make each replacement
     * worker (whose local `seen` starts at zero) immediately run s_fn/s_ctx
     * from the last triangle before the surface reopen, against pointers that
     * name the submitting thread's expired stack frame. */
    atomic_store(&s_gen, 0);
    atomic_store(&s_done, 0);
    atomic_store(&s_quit, 0);
    s_fn = NULL;
    s_ctx = NULL;

    cores = physical_cores();
    if (want <= 0)
        want = cores;
    if (want > VGL_PAR_MAX) want = VGL_PAR_MAX;
    if (want < 1) want = 1;
    s_workers = want;
    if (want == 1) {
        vgl_log(0, "raster: single-threaded (%d physical cores, %ld logical)",
                cores, sysconf(_SC_NPROCESSORS_ONLN));
        return 1;
    }

    /* Worker 0 is the thread that submits the job: it rasterises its own slice
     * rather than waiting, which is both one more core and one less handshake. */
    for (i = 1; i < s_workers; i++) {
        s_worker[i].index = i;
        pthread_mutex_init(&s_worker[i].lock, NULL);
        pthread_cond_init(&s_worker[i].wake, NULL);
        if (pthread_create(&s_worker[i].thread, NULL, worker_main,
                           &s_worker[i]) != 0) {
            vgl_log(0, "raster: only %d of %d workers started; continuing "
                       "with what came up", i, s_workers);
            s_workers = i;
            break;
        }
    }
    vgl_log(0, "raster: %d workers (%d physical cores, %ld logical), "
               "row-split, threshold %d rows", s_workers, cores,
            sysconf(_SC_NPROCESSORS_ONLN), VGL_PAR_MIN_ROWS);
    return s_workers;
}

void vgl_par_close(void)
{
    int i;

    if (s_workers <= 1) {
        s_workers = 0;
        atomic_store(&s_gen, 0);
        atomic_store(&s_done, 0);
        atomic_store(&s_quit, 0);
        s_fn = NULL;
        s_ctx = NULL;
        return;
    }
    atomic_store(&s_quit, 1);
    atomic_fetch_add(&s_gen, 1);
    for (i = 1; i < s_workers; i++) {
        pthread_mutex_lock(&s_worker[i].lock);
        pthread_cond_signal(&s_worker[i].wake);
        pthread_mutex_unlock(&s_worker[i].lock);
        pthread_join(s_worker[i].thread, NULL);
        pthread_mutex_destroy(&s_worker[i].lock);
        pthread_cond_destroy(&s_worker[i].wake);
    }
    s_workers = 0;
    atomic_store(&s_gen, 0);
    atomic_store(&s_done, 0);
    atomic_store(&s_quit, 0);
    s_fn = NULL;
    s_ctx = NULL;
}

void vgl_par_rows(vgl_par_fn fn, void *ctx, int y0, int y1)
{
    int rows = y1 - y0, n = s_workers, i, at, share, rem;
    long spins = 0;

    if (n <= 1 || rows <= 0) {
        fn(ctx, 0, y0, y1);
        return;
    }

    /* Rows are handed out in contiguous blocks rather than interleaved. Both
     * are correct; contiguous keeps each worker on its own cache lines of the
     * colour and depth buffers. */
    share = rows / n;
    rem   = rows % n;
    at    = y0;
    for (i = 0; i < n; i++) {
        int take = share + (i < rem ? 1 : 0);
        if (i > 0) {
            s_worker[i].y0 = at;
            s_worker[i].y1 = at + take;
        } else {
            y0 = at; y1 = at + take;
        }
        at += take;
    }

    s_fn  = fn;
    s_ctx = ctx;
    atomic_store(&s_done, 0);
    atomic_fetch_add(&s_gen, 1);      /* publishes everything above */
    for (i = 1; i < n; i++) {
        pthread_mutex_lock(&s_worker[i].lock);
        if (s_worker[i].sleeping)
            pthread_cond_signal(&s_worker[i].wake);
        pthread_mutex_unlock(&s_worker[i].lock);
    }

    if (y1 > y0)
        fn(ctx, 0, y0, y1);            /* the submitter's own share */

    /* Wait without timeout: returning early would expose a partially drawn
     * triangle. */
    while (atomic_load(&s_done) < n - 1) {
        if (++spins < SPIN_YIELDS) {
            __builtin_ia32_pause();
        } else {
            sched_yield();
        }
    }
}
