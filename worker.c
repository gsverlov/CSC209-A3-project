/*
 * worker.c – Child (worker) process logic.
 *
 * Each worker is an independent process forked by the parent.
 * It reads job_msg_t messages from its task pipe, runs the
 * requested Monte Carlo simulation, and writes result_msg_t
 * messages back on its result pipe.
 *
 * The worker exits cleanly when it receives a job with job_id == 0
 * (the shutdown sentinel) or when the task pipe is closed (EOF).
 */

#include "montecarlo.h"

void worker_main(int worker_id, int task_read_fd, int result_write_fd) {
    job_msg_t    job;
    result_msg_t result;

    /* Worker event loop: read tasks until shutdown or pipe closed */
    while (1) {
        int rc = read_all(task_read_fd, &job, sizeof(job));

        if (rc == 1) {
            /* EOF on task pipe – parent closed it, exit cleanly */
            break;
        }
        if (rc < 0) {
            fprintf(stderr, "Worker %d: read error: %s\n",
                    worker_id, strerror(errno));
            break;
        }

        /* Check for shutdown sentinel */
        if (job.job_id == 0) {
            break;
        }

        /* Run the simulation */
        uint32_t hits = 0;
        double est = run_simulation(job.sim_type, job.trials,
                                    job.seed, &hits);

        /* Build result message */
        memset(&result, 0, sizeof(result));
        result.job_id    = job.job_id;
        result.worker_id = (uint32_t)worker_id;
        result.trials    = job.trials;
        result.hits      = hits;
        result.estimate  = est;

        /* Send result back to parent */
        if (write_all(result_write_fd, &result, sizeof(result)) < 0) {
            fprintf(stderr, "Worker %d: write error: %s\n",
                    worker_id, strerror(errno));
            break;
        }
    }

    /* Clean up file descriptors before exiting */
    close(task_read_fd);
    close(result_write_fd);
    exit(0);
}
