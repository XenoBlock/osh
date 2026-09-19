/* jobs.c - job table queries used by the builtins */
#include "osh.h"
#include <errno.h>

/* the job table is defined here, shared via osh.h */
Job jobs[MAX_JOBS];
int next_job = 1;

void jobs_print_all(void) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (!jobs[i].pid) continue;
        const char *state = jobs[i].running == -1 ? "Done" :
                            jobs[i].running == 0  ? "Stopped" : "Running";
        printf("[%d] %s\t%s\n", jobs[i].job, state, jobs[i].cmd ? jobs[i].cmd : "");
        if (jobs[i].running == -1) { jobs[i].pid = 0; }
    }
}

pid_t job_pid(int job) {
    for (int i = 0; i < MAX_JOBS; i++)
        if (jobs[i].pid && jobs[i].job == job) return jobs[i].pid;
    return -1;
}

static void give_terminal_to(pid_t pgid) {
    if (!g_interactive) return;
    pid_t my_pgid = getpgrp();
    tcsetpgrp(0, pgid);
    (void)my_pgid;
}

void fg_job(int job) {
    pid_t pid = job_pid(job);
    if (pid <= 0) { fprintf(stderr, "osh: fg: no such job\n"); return; }
    give_terminal_to(pid);
    kill(-pid, SIGCONT);
    int st;
    sigchld_block();
    waitpid(pid, &st, WUNTRACED);
    sigchld_unblock();
    if (WIFEXITED(st)) g_status = WEXITSTATUS(st);
    else if (WIFSIGNALED(st)) g_status = 128 + WTERMSIG(st);
    else g_status = 128;
}

void bg_job(int job) {
    pid_t pid = job_pid(job);
    if (pid <= 0) { fprintf(stderr, "osh: bg: no such job\n"); return; }
    kill(-pid, SIGCONT);
}

void wait_for_jobs(void) {
    for (;;) {
        int any = 0;
        for (int i = 0; i < MAX_JOBS; i++)
            if (jobs[i].pid && jobs[i].running == 1) { any = 1; break; }
        if (!any) break;
        sigchld_block();
        jobs_reap();
        sigchld_unblock();
    }
}
