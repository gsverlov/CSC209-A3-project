#ifndef MONTECARLO_H
#define MONTECARLO_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <time.h>
#include <math.h>
#include <signal.h>

/* ── Configuration ──────────────────────────────────────────────── */

#define MAX_WORKERS        8
#define DEFAULT_WORKERS    4
#define DEFAULT_TRIALS     1000000   /* per worker per job */
#define MAX_JOBS          64

/* ── Communication Protocol ─────────────────────────────────────
 *
 *  Parent → Worker  (task pipe):   job_msg_t
 *  Worker → Parent  (result pipe): result_msg_t
 *
 *  A job_id of 0 is the SHUTDOWN sentinel: the worker must exit
 *  cleanly upon receiving it.
 * ────────────────────────────────────────────────────────────── */

/* Message: parent → worker (task assignment) */
typedef struct {
    uint32_t job_id;        /* 0 = shutdown sentinel              */
    uint32_t trials;        /* number of random trials to run     */
    uint32_t seed;          /* RNG seed for reproducibility       */
    uint32_t sim_type;      /* 0 = estimate pi, 1 = estimate e,
                               2 = estimate sqrt(2)              */
} job_msg_t;

/* Message: worker → parent (result) */
typedef struct {
    uint32_t job_id;        /* echoes the job_id from the task    */
    uint32_t worker_id;     /* which worker produced this result  */
    uint32_t trials;        /* trials actually completed          */
    uint32_t hits;          /* "hits" (meaning depends on sim)    */
    double   estimate;      /* the worker's local estimate        */
} result_msg_t;

/* Per-worker bookkeeping stored by the parent */
typedef struct {
    pid_t    pid;
    int      task_fd;       /* parent writes tasks here           */
    int      result_fd;     /* parent reads results here          */
    int      busy;          /* 1 if the worker has a pending job  */
} worker_info_t;

/* ── Function prototypes (worker.c) ─────────────────────────── */

void worker_main(int worker_id, int task_read_fd, int result_write_fd);

/* ── Function prototypes (simulate.c) ───────────────────────── */

/* Run a Monte Carlo simulation and return the estimate.
 * Sets *hits_out to the number of "hits" for aggregation. */
double run_simulation(uint32_t sim_type, uint32_t trials,
                      uint32_t seed, uint32_t *hits_out);

/* Return a human-readable name for a simulation type. */
const char *sim_type_name(uint32_t sim_type);

/* Return the known true value for a simulation type. */
double sim_true_value(uint32_t sim_type);

/* ── Function prototypes (protocol.c) ───────────────────────── */

/* Robust read/write that handle partial I/O and EINTR. */
int write_all(int fd, const void *buf, size_t len);
int read_all(int fd, void *buf, size_t len);

/* ── Simulation type constants ──────────────────────────────── */

#define SIM_PI     0   /* Estimate π via unit-circle method      */
#define SIM_E      1   /* Estimate e via random sum method       */
#define SIM_SQRT2  2   /* Estimate √2 via geometric probability */
#define SIM_COUNT  3   /* Total number of simulation types       */

#endif /* MONTECARLO_H */
