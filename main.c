// main.c - parent process, spawns workers and coordinates everything

#include "montecarlo.h"

static void spawn_workers(worker_info_t *workers, int n);
static void distribute_jobs(worker_info_t *workers, int n_workers,
                            int n_jobs, uint32_t trials, uint32_t sim_type);
static void collect_results(worker_info_t *workers, int n_workers,
                            int n_jobs, uint32_t sim_type);
static void shutdown_workers(worker_info_t *workers, int n_workers);
static void reap_workers(worker_info_t *workers, int n_workers);
static void print_usage(const char *prog);

int main(int argc, char *argv[]) {
    int n_workers = DEFAULT_WORKERS;
    uint32_t trials = DEFAULT_TRIALS;
    int n_jobs = -1;
    uint32_t sim_type = SIM_PI;
    int run_all = 0;
    int opt;

    while ((opt = getopt(argc, argv, "w:t:j:s:h")) != -1) {
        switch (opt) {
        case 'w':
            n_workers = atoi(optarg);
            if (n_workers < 1 || n_workers > MAX_WORKERS) {
                fprintf(stderr, "error: workers must be between 1 and %d\n", MAX_WORKERS);
                return 1;
            }
            break;
        case 't':
            trials = (uint32_t)atoi(optarg);
            if (trials == 0) {
                fprintf(stderr, "error: trials has to be > 0\n");
                return 1;
            }
            break;
        case 'j':
            n_jobs = atoi(optarg);
            if (n_jobs < 1 || n_jobs > MAX_JOBS) {
                fprintf(stderr, "error: jobs must be between 1 and %d\n", MAX_JOBS);
                return 1;
            }
            break;
        case 's':
            sim_type = (uint32_t)atoi(optarg);
            if (sim_type == SIM_COUNT) {
                run_all = 1;
            } else if (sim_type >= SIM_COUNT) {
                fprintf(stderr, "error: sim_type must be 0-%d\n", SIM_COUNT);
                return 1;
            }
            break;
        case 'h':
        default:
            print_usage(argv[0]);
            return (opt == 'h') ? 0 : 1;
        }
    }

    if (n_jobs < 0) n_jobs = n_workers * 3;

    printf("=== Monte Carlo Parallel Estimator ===\n");
    printf("Workers: %d, Trials per job: %u, Jobs: %d, Total trials: %lu\n\n",
           n_workers, trials, n_jobs, (unsigned long)trials * n_jobs);

    if (run_all) {
        for (uint32_t s = 0; s < SIM_COUNT; s++) {
            worker_info_t workers[MAX_WORKERS];
            memset(workers, 0, sizeof(workers));

            printf("--- %s ---\n\n", sim_type_name(s));
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

        printf("--- %s ---\n\n", sim_type_name(sim_type));
        spawn_workers(workers, n_workers);
        distribute_jobs(workers, n_workers, n_jobs, trials, sim_type);
        collect_results(workers, n_workers, n_jobs, sim_type);
        shutdown_workers(workers, n_workers);
        reap_workers(workers, n_workers);
    }

    return 0;
}

static void spawn_workers(worker_info_t *workers, int n) {
    for (int i = 0; i < n; i++) {
        int task_pipe[2];
        int result_pipe[2];

        if (pipe(task_pipe) < 0) {
            perror("pipe");
            exit(1);
        }
        if (pipe(result_pipe) < 0) {
            perror("pipe");
            exit(1);
        }

        fflush(stdout);
        fflush(stderr);

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            exit(1);
        }

        if (pid == 0) {
            // child - close the ends we dont need
            close(task_pipe[1]);
            close(result_pipe[0]);

            // close fds from earlier workers so EOF works right
            for (int j = 0; j < i; j++) {
                close(workers[j].task_fd);
                close(workers[j].result_fd);
            }

            worker_main(i, task_pipe[0], result_pipe[1]);
            _exit(1); // shouldnt get here
        }

        // parent side
        close(task_pipe[0]);
        close(result_pipe[1]);

        workers[i].pid = pid;
        workers[i].task_fd = task_pipe[1];
        workers[i].result_fd = result_pipe[0];
        workers[i].busy = 0;

        printf("spawned worker %d (pid %d)\n", i, pid);
    }
    printf("\n");
}

// sends jobs to workers round robin
static void distribute_jobs(worker_info_t *workers, int n_workers,
                            int n_jobs, uint32_t trials, uint32_t sim_type) {
    srand((unsigned int)time(NULL));

    printf("dispatching %d jobs...\n", n_jobs);

    for (int j = 0; j < n_jobs; j++) {
        int w = j % n_workers;

        job_msg_t job;
        memset(&job, 0, sizeof(job));
        job.job_id = (uint32_t)(j + 1);
        job.trials = trials;
        job.seed = (uint32_t)rand();
        job.sim_type = sim_type;

        if (write_all(workers[w].task_fd, &job, sizeof(job)) < 0) {
            fprintf(stderr, "failed to send job %d to worker %d: %s\n",
                    j + 1, w, strerror(errno));
            exit(1);
        }
    }
    printf("all jobs sent.\n\n");
}

static void collect_results(worker_info_t *workers, int n_workers,
                            int n_jobs, uint32_t sim_type) {
    double total_estimate = 0.0;
    uint64_t total_trials = 0;
    uint64_t total_hits = 0;

    printf("collecting results...\n\n");
    printf("Job\tWorker\tTrials\t\tHits\t\tEstimate\n");
    printf("---\t------\t------\t\t----\t\t--------\n");

    for (int j = 0; j < n_jobs; j++) {
        int w = j % n_workers;
        result_msg_t result;

        int rc = read_all(workers[w].result_fd, &result, sizeof(result));
        if (rc != 0) {
            fprintf(stderr, "error reading result for job %d from worker %d: %s\n",
                    j + 1, w, rc == 1 ? "unexpected EOF" : strerror(errno));
            exit(1);
        }

        printf("%u\t%u\t%u\t\t%u\t\t%.8f\n",
               result.job_id, result.worker_id,
               result.trials, result.hits, result.estimate);

        total_estimate += result.estimate;
        total_trials += result.trials;
        total_hits += result.hits;
    }

    double avg = total_estimate / n_jobs;
    double true_val = sim_true_value(sim_type);
    double abs_err = fabs(avg - true_val);
    double rel_err = (true_val != 0.0) ? (abs_err / true_val * 100.0) : 0.0;

    printf("\n--- Results: %s ---\n", sim_type_name(sim_type));
    printf("Total trials:     %lu\n", (unsigned long)total_trials);
    printf("Total hits:       %lu\n", (unsigned long)total_hits);
    printf("Avg estimate:     %.10f\n", avg);
    printf("True value:       %.10f\n", true_val);
    printf("Absolute error:   %.10f\n", abs_err);
    printf("Relative error:   %.6f%%\n", rel_err);
    printf("---\n");
}

// sends shutdown signal (job_id=0) to all workers
static void shutdown_workers(worker_info_t *workers, int n_workers) {
    job_msg_t shutdown_msg;
    memset(&shutdown_msg, 0, sizeof(shutdown_msg));
    shutdown_msg.job_id = 0;

    for (int i = 0; i < n_workers; i++) {
        write_all(workers[i].task_fd, &shutdown_msg, sizeof(shutdown_msg));
        close(workers[i].task_fd);
        workers[i].task_fd = -1;
    }
}

static void reap_workers(worker_info_t *workers, int n_workers) {
    for (int i = 0; i < n_workers; i++) {
        int status;
        pid_t p = waitpid(workers[i].pid, &status, 0);
        if (p < 0) {
            fprintf(stderr, "waitpid failed for worker %d: %s\n", i, strerror(errno));
        } else if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
            fprintf(stderr, "worker %d exited with status %d\n", i, WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            fprintf(stderr, "worker %d killed by signal %d\n", i, WTERMSIG(status));
        }
        close(workers[i].result_fd);
        workers[i].result_fd = -1;
    }
}

static void print_usage(const char *prog) {
    printf("Usage: %s [-w workers] [-t trials] [-j jobs] [-s sim_type]\n\n", prog);
    printf("  -w workers    number of worker processes (1-%d, default %d)\n",
           MAX_WORKERS, DEFAULT_WORKERS);
    printf("  -t trials     trials per job (default %d)\n", DEFAULT_TRIALS);
    printf("  -j jobs       total jobs to run (default workers*3)\n");
    printf("  -s sim_type   0=pi, 1=e, 2=sqrt(2), 3=all (default 0)\n");
    printf("  -h            show this message\n");
}
