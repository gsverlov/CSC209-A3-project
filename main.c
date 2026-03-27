/*
 * main.c – Parent (controller) process for Monte Carlo simulations.
 *
 * Usage:  ./montecarlo [-w workers] [-t trials] [-j jobs] [-s sim_type]
 *
 *   -w workers    Number of worker processes (1–8, default 4)
 *   -t trials     Trials per job (default 1000000)
 *   -j jobs       Total number of jobs to dispatch (default = workers × 3)
 *   -s sim_type   Simulation: 0=pi, 1=e, 2=sqrt2, 3=all (default 0)
 *
 * Architecture:
 *   The parent spawns a pool of worker processes at start-up.  Each
 *   worker has two pipes: one for receiving tasks, one for sending
 *   results.  The parent distributes jobs round-robin across idle
 *   workers, collects results, and prints a summary with the
 *   aggregated estimate and error.
 */

#include "montecarlo.h"

/* ── Forward declarations ────────────────────────────────────── */

static void spawn_workers(worker_info_t *workers, int n);
static void distribute_jobs(worker_info_t *workers, int n_workers,
                            int n_jobs, uint32_t trials, uint32_t sim_type);
static void collect_results(worker_info_t *workers, int n_workers,
                            int n_jobs, uint32_t sim_type);
static void shutdown_workers(worker_info_t *workers, int n_workers);
static void reap_workers(worker_info_t *workers, int n_workers);
static void print_usage(const char *prog);

/* ── Main ────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    int      n_workers = DEFAULT_WORKERS;
    uint32_t trials    = DEFAULT_TRIALS;
    int      n_jobs    = -1;  /* sentinel: will default to workers×3 */
    uint32_t sim_type  = SIM_PI;
    int      run_all   = 0;
    int      opt;

    while ((opt = getopt(argc, argv, "w:t:j:s:h")) != -1) {
        switch (opt) {
            case 'w':
                n_workers = atoi(optarg);
                if (n_workers < 1 || n_workers > MAX_WORKERS) {
                    fprintf(stderr, "Error: workers must be 1–%d\n",
                            MAX_WORKERS);
                    return 1;
                }
                break;
            case 't':
                trials = (uint32_t)atoi(optarg);
                if (trials == 0) {
                    fprintf(stderr, "Error: trials must be > 0\n");
                    return 1;
                }
                break;
            case 'j':
                n_jobs = atoi(optarg);
                if (n_jobs < 1 || n_jobs > MAX_JOBS) {
                    fprintf(stderr, "Error: jobs must be 1–%d\n", MAX_JOBS);
                    return 1;
                }
                break;
            case 's':
                sim_type = (uint32_t)atoi(optarg);
                if (sim_type == SIM_COUNT) {
                    run_all = 1;
                } else if (sim_type >= SIM_COUNT) {
                    fprintf(stderr, "Error: sim_type must be 0–%d\n",
                            SIM_COUNT);
                    return 1;
                }
                break;
            case 'h':
            default:
                print_usage(argv[0]);
                return (opt == 'h') ? 0 : 1;
        }
    }

    if (n_jobs < 0) {
        n_jobs = n_workers * 3;
    }

    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║          Monte Carlo Parallel Estimator                 ║\n");
    printf("╠══════════════════════════════════════════════════════════╣\n");
    printf("║  Workers: %-4d    Trials/job: %-10u                ║\n",
           n_workers, trials);
    printf("║  Jobs:    %-4d    Total trials: %-14lu           ║\n",
           n_jobs, (unsigned long)trials * n_jobs);
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    if (run_all) {
        /* Run all three simulation types sequentially */
        for (uint32_t s = 0; s < SIM_COUNT; s++) {
            worker_info_t workers[MAX_WORKERS];
            memset(workers, 0, sizeof(workers));

            printf("── %s ──\n\n", sim_type_name(s));
            spawn_workers(workers, n_workers);
            distribute_jobs(workers, n_workers, n_jobs, trials, s);
            collect_results(workers, n_workers, n_jobs, s);
            shutdown_workers(workers, n_workers);
            reap_workers(workers, n_workers);
            printf("\n");
        }
    } else {
        worker_info_t workers[MAX_WORKERS];
        memset(workers, 0, sizeof(workers));

        printf("── %s ──\n\n", sim_type_name(sim_type));
        spawn_workers(workers, n_workers);
        distribute_jobs(workers, n_workers, n_jobs, trials, sim_type);
        collect_results(workers, n_workers, n_jobs, sim_type);
        shutdown_workers(workers, n_workers);
        reap_workers(workers, n_workers);
    }

    return 0;
}

/* ── Spawn worker pool ───────────────────────────────────────── */

static void spawn_workers(worker_info_t *workers, int n) {
    for (int i = 0; i < n; i++) {
        int task_pipe[2];   /* parent writes, worker reads */
        int result_pipe[2]; /* worker writes, parent reads */

        if (pipe(task_pipe) < 0) {
            perror("pipe (task)");
            exit(1);
        }
        if (pipe(result_pipe) < 0) {
            perror("pipe (result)");
            exit(1);
        }

        /* Flush all stdio buffers before forking so children
         * do not inherit and re-print buffered output. */
        fflush(stdout);
        fflush(stderr);

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            exit(1);
        }

        if (pid == 0) {
            /* ── Child process ─────────────────────────────── */

            /* Close the ends of the pipes the child doesn't use */
            close(task_pipe[1]);    /* child does not write to task pipe */
            close(result_pipe[0]);  /* child does not read from result pipe */

            /* Also close pipes belonging to previously spawned workers.
             * The child inherited copies of those file descriptors from
             * the parent; leaving them open would prevent EOF detection. */
            for (int j = 0; j < i; j++) {
                close(workers[j].task_fd);
                close(workers[j].result_fd);
            }

            worker_main(i, task_pipe[0], result_pipe[1]);
            /* worker_main calls exit(), should not reach here */
            _exit(1);
        }

        /* ── Parent process ────────────────────────────────── */
        close(task_pipe[0]);    /* parent does not read from task pipe */
        close(result_pipe[1]);  /* parent does not write to result pipe */

        workers[i].pid       = pid;
        workers[i].task_fd   = task_pipe[1];
        workers[i].result_fd = result_pipe[0];
        workers[i].busy      = 0;

        printf("  Spawned worker %d (PID %d)\n", i, pid);
    }
    printf("\n");
}

/* ── Distribute jobs to workers round-robin ──────────────────── */

static void distribute_jobs(worker_info_t *workers, int n_workers,
                            int n_jobs, uint32_t trials, uint32_t sim_type) {
    /*
     * We use a simple round-robin scheme: job i goes to worker
     * (i % n_workers).  Each worker may receive multiple jobs in
     * sequence; it processes them one at a time and sends back
     * one result per job.
     */

    /* Seed the RNG for generating per-job seeds */
    srand((unsigned int)time(NULL));

    printf("  Dispatching %d jobs...\n", n_jobs);

    for (int j = 0; j < n_jobs; j++) {
        int w = j % n_workers;

        job_msg_t job;
        memset(&job, 0, sizeof(job));
        job.job_id   = (uint32_t)(j + 1);  /* job IDs start at 1 */
        job.trials   = trials;
        job.seed     = (uint32_t)rand();
        job.sim_type = sim_type;

        if (write_all(workers[w].task_fd, &job, sizeof(job)) < 0) {
            fprintf(stderr, "Error sending job %d to worker %d: %s\n",
                    j + 1, w, strerror(errno));
            exit(1);
        }
    }

    printf("  All jobs dispatched.\n\n");
}

/* ── Collect results from all workers ────────────────────────── */

static void collect_results(worker_info_t *workers, int n_workers,
                            int n_jobs, uint32_t sim_type) {
    /*
     * We know exactly how many results to expect (one per job).
     * We read from each worker's result pipe using a round-robin
     * that matches the dispatch order: job j was sent to worker
     * (j % n_workers), so result j will come from that worker.
     *
     * This is simpler than using select() and sufficient because
     * each worker processes jobs in FIFO order.
     */

    double   total_estimate = 0.0;
    uint64_t total_trials   = 0;
    uint64_t total_hits     = 0;

    printf("  Collecting results...\n\n");
    printf("  %-6s  %-8s  %-12s  %-12s  %s\n",
           "Job", "Worker", "Trials", "Hits", "Estimate");
    printf("  %-6s  %-8s  %-12s  %-12s  %s\n",
           "---", "------", "------", "----", "--------");

    for (int j = 0; j < n_jobs; j++) {
        int w = j % n_workers;
        result_msg_t result;

        int rc = read_all(workers[w].result_fd, &result, sizeof(result));
        if (rc != 0) {
            fprintf(stderr, "Error reading result for job %d from "
                    "worker %d: %s\n", j + 1, w,
                    rc == 1 ? "unexpected EOF" : strerror(errno));
            exit(1);
        }

        printf("  %-6u  %-8u  %-12u  %-12u  %.8f\n",
               result.job_id, result.worker_id,
               result.trials, result.hits, result.estimate);

        total_estimate += result.estimate;
        total_trials   += result.trials;
        total_hits     += result.hits;
    }

    /* Compute aggregated estimate */
    double avg_estimate = total_estimate / n_jobs;
    double true_val     = sim_true_value(sim_type);
    double abs_error    = fabs(avg_estimate - true_val);
    double rel_error    = (true_val != 0.0)
                          ? (abs_error / true_val * 100.0) : 0.0;

    printf("\n  ══════════════════════════════════════════════════\n");
    printf("  RESULTS SUMMARY: %s\n", sim_type_name(sim_type));
    printf("  ──────────────────────────────────────────────────\n");
    printf("  Total trials:        %lu\n", (unsigned long)total_trials);
    printf("  Total hits:          %lu\n", (unsigned long)total_hits);
    printf("  Average estimate:    %.10f\n", avg_estimate);
    printf("  True value:          %.10f\n", true_val);
    printf("  Absolute error:      %.10f\n", abs_error);
    printf("  Relative error:      %.6f%%\n", rel_error);
    printf("  ══════════════════════════════════════════════════\n");
}

/* ── Send shutdown sentinel to all workers ───────────────────── */

static void shutdown_workers(worker_info_t *workers, int n_workers) {
    job_msg_t shutdown;
    memset(&shutdown, 0, sizeof(shutdown));
    shutdown.job_id = 0;  /* sentinel value */

    for (int i = 0; i < n_workers; i++) {
        /* Write the shutdown message; ignore errors (worker may
         * have already exited if it encountered an issue). */
        write_all(workers[i].task_fd, &shutdown, sizeof(shutdown));
        close(workers[i].task_fd);
        workers[i].task_fd = -1;
    }
}

/* ── Wait for all child processes to exit ────────────────────── */

static void reap_workers(worker_info_t *workers, int n_workers) {
    for (int i = 0; i < n_workers; i++) {
        int status;
        pid_t p = waitpid(workers[i].pid, &status, 0);
        if (p < 0) {
            fprintf(stderr, "Warning: waitpid for worker %d (PID %d) "
                    "failed: %s\n", i, workers[i].pid, strerror(errno));
        } else if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
            fprintf(stderr, "Warning: worker %d (PID %d) exited with "
                    "status %d\n", i, workers[i].pid, WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            fprintf(stderr, "Warning: worker %d (PID %d) killed by "
                    "signal %d\n", i, workers[i].pid, WTERMSIG(status));
        }

        /* Close the result pipe */
        close(workers[i].result_fd);
        workers[i].result_fd = -1;
    }
}

/* ── Usage ───────────────────────────────────────────────────── */

static void print_usage(const char *prog) {
    printf("Usage: %s [-w workers] [-t trials] [-j jobs] [-s sim_type]\n\n",
           prog);
    printf("Options:\n");
    printf("  -w workers   Number of worker processes (1–%d, default %d)\n",
           MAX_WORKERS, DEFAULT_WORKERS);
    printf("  -t trials    Trials per job (default %d)\n", DEFAULT_TRIALS);
    printf("  -j jobs      Total number of jobs (default workers × 3)\n");
    printf("  -s sim_type  Simulation type:\n");
    printf("                 0 = Estimate pi  (default)\n");
    printf("                 1 = Estimate e\n");
    printf("                 2 = Estimate sqrt(2)\n");
    printf("                 3 = Run all three\n");
    printf("  -h           Show this help message\n");
}
