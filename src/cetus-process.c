#include <signal.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include "glib-ext.h"
#include "cetus-channel.h"
#include "cetus-process.h"
#include "network-socket.h"
#include "cetus-process-cycle.h"

typedef struct {
    int     signo;
    char   *signame;
    char   *name;
    void (*handler)(int signo, siginfo_t *siginfo, void *ucontext);
} cetus_signal_t;


static void cetus_execute_proc(cetus_cycle_t *cycle, void *data);
static void cetus_signal_handler(int signo, siginfo_t *siginfo, void *ucontext);
static void cetus_process_get_status(void);


int              cetus_argc;
char           **cetus_argv;
char           **cetus_os_argv;

int              cetus_process_slot;
int              cetus_channel;
int              cetus_last_process;
struct event     cetus_channel_event;
cetus_process_t  cetus_processes[CETUS_MAX_PROCESSES];


cetus_signal_t  signals[] = {
    { cetus_signal_value(CETUS_RECONFIGURE_SIGNAL),
      "SIG" cetus_value(CETUS_RECONFIGURE_SIGNAL),
      "reload",
      cetus_signal_handler },

    { cetus_signal_value(CETUS_REOPEN_SIGNAL),
      "SIG" cetus_value(CETUS_REOPEN_SIGNAL),
      "reopen",
      cetus_signal_handler },

    { cetus_signal_value(CETUS_NOACCEPT_SIGNAL),
      "SIG" cetus_value(CETUS_NOACCEPT_SIGNAL),
      "",
      cetus_signal_handler },

    { cetus_signal_value(CETUS_TERMINATE_SIGNAL),
      "SIG" cetus_value(CETUS_TERMINATE_SIGNAL),
      "stop",
      cetus_signal_handler },

    { cetus_signal_value(CETUS_SHUTDOWN_SIGNAL),
      "SIG" cetus_value(CETUS_SHUTDOWN_SIGNAL),
      "quit",
      cetus_signal_handler },

    { cetus_signal_value(CETUS_CHANGEBIN_SIGNAL),
      "SIG" cetus_value(CETUS_CHANGEBIN_SIGNAL),
      "",
      cetus_signal_handler },

    { SIGALRM, "SIGALRM", "", cetus_signal_handler },

    { SIGINT, "SIGINT", "", cetus_signal_handler },

    { SIGIO, "SIGIO", "", cetus_signal_handler },

    { SIGCHLD, "SIGCHLD", "", cetus_signal_handler },

    { SIGSYS, "SIGSYS, SIG_IGN", "", NULL },

    { SIGPIPE, "SIGPIPE, SIG_IGN", "", NULL },

    { 0, NULL, "", NULL }
};


pid_t
cetus_spawn_process(cetus_cycle_t *cycle, cetus_spawn_proc_pt proc, void *data,
    char *name, int respawn)
{
    u_long       on;
    pid_t  pid;
    int  s;

    if (respawn >= 0) {
        s = respawn;

    } else {
        for (s = 0; s < cetus_last_process; s++) {
            if (cetus_processes[s].pid == -1) {
                break;
            }
        }

        if (s == CETUS_MAX_PROCESSES) {
            g_critical("%s: no more than %d processes can be spawned",
                    G_STRLOC, CETUS_MAX_PROCESSES);
            return CETUS_INVALID_PID;
        }
    }


    if (respawn != CETUS_PROCESS_DETACHED) {

        if (socketpair(AF_UNIX, SOCK_STREAM, 0, cetus_processes[s].parent_child_channel) == -1)
        {
            g_critical("%s: socketpair() failed while spawning \"%s\"",
                    G_STRLOC, name);
            return CETUS_INVALID_PID;
        }

        g_debug("%s: index:%d, channel %d:%d, cycle:%p", G_STRLOC, s,
                       cetus_processes[s].parent_child_channel[0],
                       cetus_processes[s].parent_child_channel[1], cycle );

        if (fcntl(cetus_processes[s].parent_child_channel[0], F_SETFL, O_NONBLOCK | O_RDWR) != 0) {
            g_critical("%s: nonblock failed while spawning \"%s\": %s (%d)",
                    G_STRLOC, name, g_strerror(errno), errno);
            cetus_close_channel(cetus_processes[s].parent_child_channel);
            return CETUS_INVALID_PID;
        }

        if (fcntl(cetus_processes[s].parent_child_channel[1], F_SETFL, O_NONBLOCK | O_RDWR) != 0) {
            g_critical("%s: nonblock failed while spawning \"%s\": %s (%d)",
                    G_STRLOC, name, g_strerror(errno), errno);
            cetus_close_channel(cetus_processes[s].parent_child_channel);
            return CETUS_INVALID_PID;
        }

        on = 1;
        if (ioctl(cetus_processes[s].parent_child_channel[0], FIOASYNC, &on) == -1) {
            g_critical("%s: ioctl(FIOASYNC) failed while spawning \"%s\"",
                    G_STRLOC, name);
            cetus_close_channel(cetus_processes[s].parent_child_channel);
            return CETUS_INVALID_PID;
        }

        if (fcntl(cetus_processes[s].parent_child_channel[0], F_SETOWN, cetus_pid) == -1) {
            g_critical("%s: fcntl(F_SETOWN) failed while spawning \"%s\"",
                    G_STRLOC, name);
            cetus_close_channel(cetus_processes[s].parent_child_channel);
            return CETUS_INVALID_PID;
        }

        if (fcntl(cetus_processes[s].parent_child_channel[0], F_SETFD, FD_CLOEXEC) == -1) {
            g_critical("%s: fcntl(FD_CLOEXEC) failed while spawning \"%s\"",
                    G_STRLOC, name);
            cetus_close_channel(cetus_processes[s].parent_child_channel);
            return CETUS_INVALID_PID;
        }

        if (fcntl(cetus_processes[s].parent_child_channel[1], F_SETFD, FD_CLOEXEC) == -1) {
            g_critical("%s: fcntl(FD_CLOEXEC) failed while spawning \"%s\"",
                    G_STRLOC, name);
            cetus_close_channel(cetus_processes[s].parent_child_channel);
            return CETUS_INVALID_PID;
        }

        cetus_channel = cetus_processes[s].parent_child_channel[1];

    } else {
        cetus_processes[s].parent_child_channel[0] = -1;
        cetus_processes[s].parent_child_channel[1] = -1;
    }

    cetus_process_slot = s;


    g_message("%s: before call fork, channel:%d", G_STRLOC, s);
    pid = fork();

    g_message("%s: after call fork, pid:%d", G_STRLOC, pid);
    switch (pid) {

    case -1:
        g_critical("%s: fork() failed while spawning \"%s\"",
                    G_STRLOC, name);
        cetus_close_channel(cetus_processes[s].parent_child_channel);
        return CETUS_INVALID_PID;

    case 0:
        cetus_parent = cetus_pid;
        cetus_pid = getpid();
        proc(cycle, data);
        break;

    default:
        break;
    }

    g_message("%s: start %s %d, respawn:%d", G_STRLOC, name, pid, respawn);

    cetus_processes[s].pid = pid;
    cetus_processes[s].exited = 0;

    if (respawn >= 0) {
        return pid;
    }

    cetus_processes[s].proc = proc;
    cetus_processes[s].data = data;
    cetus_processes[s].name = name;
    cetus_processes[s].exiting = 0;

    switch (respawn) {

    case CETUS_PROCESS_NORESPAWN:
        cetus_processes[s].respawn = 0;
        cetus_processes[s].just_spawn = 0;
        cetus_processes[s].detached = 0;
        break;

    case CETUS_PROCESS_JUST_SPAWN:
        cetus_processes[s].respawn = 0;
        cetus_processes[s].just_spawn = 1;
        cetus_processes[s].detached = 0;
        break;

    case CETUS_PROCESS_RESPAWN:
        cetus_processes[s].respawn = 1;
        cetus_processes[s].just_spawn = 0;
        cetus_processes[s].detached = 0;
        break;

    case CETUS_PROCESS_JUST_RESPAWN:
        cetus_processes[s].respawn = 1;
        cetus_processes[s].just_spawn = 1;
        cetus_processes[s].detached = 0;
        break;

    case CETUS_PROCESS_DETACHED:
        cetus_processes[s].respawn = 0;
        cetus_processes[s].just_spawn = 0;
        cetus_processes[s].detached = 1;
        break;
    }

    if (s == cetus_last_process) {
        g_message("%s: cetus_last_process add,orig:%d", G_STRLOC, cetus_last_process);
        cetus_last_process++;
    }

    return pid;
}


pid_t
cetus_execute(cetus_cycle_t *cycle, cetus_exec_ctx_t *ctx)
{
    return cetus_spawn_process(cycle, cetus_execute_proc, ctx, ctx->name,
                             CETUS_PROCESS_DETACHED);
}


static void
cetus_execute_proc(cetus_cycle_t *cycle, void *data)
{
    cetus_exec_ctx_t  *ctx = data;

    if (execve(ctx->path, ctx->argv, ctx->envp) == -1) {
        g_critical("%s: execve() failed while executing %s \"%s\"",
                G_STRLOC, ctx->name, ctx->path);
    }

    exit(1);
}


int
cetus_init_signals()
{
    cetus_signal_t      *sig;
    struct sigaction     sa;

    for (sig = signals; sig->signo != 0; sig++) {
        memset(&sa, 0, sizeof(struct sigaction));

        if (sig->handler) {
            sa.sa_sigaction = sig->handler;
            sa.sa_flags = SA_SIGINFO;

        } else {
            sa.sa_handler = SIG_IGN;
        }

        sigemptyset(&sa.sa_mask);
        if (sigaction(sig->signo, &sa, NULL) == -1) {
            g_critical("%s: sigaction(%s) failed", G_STRLOC, sig->signame);
            return -1;
        }
    }

    return 0;
}


static void 
cetus_signal_handler(int signo, siginfo_t *siginfo, void *ucontext)
{
    char            *action;
    int              ignore;
    int              err;
    cetus_signal_t  *sig;

    ignore = 0;

    err =  errno;

    for (sig = signals; sig->signo != 0; sig++) {
        if (sig->signo == signo) {
            break;
        }
    }

    g_debug("%s: cetus_signal_handler is called:%d, errno:%d", G_STRLOC, signo, errno);
    action = "";

    switch (cetus_process) {

    case CETUS_PROCESS_MASTER:
    case CETUS_PROCESS_SINGLE:
        switch (signo) {

        case cetus_signal_value(CETUS_SHUTDOWN_SIGNAL):
            cetus_quit = 1;
            action = ", shutting down";
            break;

        case cetus_signal_value(CETUS_TERMINATE_SIGNAL):
        case SIGINT:
            cetus_terminate = 1;
            action = ", exiting";
            break;

        case cetus_signal_value(CETUS_NOACCEPT_SIGNAL):
            if (cetus_daemonized) {
                cetus_noaccept = 1;
                action = ", stop accepting connections";
            }
            break;

        case cetus_signal_value(CETUS_RECONFIGURE_SIGNAL):
            cetus_reconfigure = 1;
            action = ", reconfiguring";
            break;

        case cetus_signal_value(CETUS_REOPEN_SIGNAL):
            cetus_reopen = 1;
            action = ", reopening logs";
            break;

        case SIGALRM:
            cetus_sigalrm = 1;
            break;

        case SIGIO:
            cetus_sigio = 1;
            break;

        case SIGCHLD:
            cetus_reap = 1;
            break;
        }

        break;

    case CETUS_PROCESS_WORKER:
    case CETUS_PROCESS_HELPER:
        switch (signo) {

        case cetus_signal_value(CETUS_NOACCEPT_SIGNAL):
            if (!cetus_daemonized) {
                break;
            }
            cetus_debug_quit = 1;
            /* fall through */
        case cetus_signal_value(CETUS_SHUTDOWN_SIGNAL):
            cetus_quit = 1;
            action = ", shutting down";
            break;

        case cetus_signal_value(CETUS_TERMINATE_SIGNAL):
        case SIGINT:
            g_message("%s: call here:%d", G_STRLOC, signo);
            cetus_terminate = 1;
            action = ", exiting";
            break;

        case cetus_signal_value(CETUS_REOPEN_SIGNAL):
            cetus_reopen = 1;
            action = ", reopening logs";
            break;

        case cetus_signal_value(CETUS_RECONFIGURE_SIGNAL):
        case cetus_signal_value(CETUS_CHANGEBIN_SIGNAL):
        case SIGIO:
            action = ", ignoring";
            break;
        }

        break;
    }

    if (siginfo && siginfo->si_pid) {
        g_message("%s: signal %d (%s) received from %d %s", G_STRLOC,
                signo, sig->signame, siginfo->si_pid, action);
    } else {
        g_message("%s: signal %d (%s) received %s, err:%d", G_STRLOC,
                signo, sig->signame, action, err);
    }

    if (ignore) {
        g_critical("%s: the changing binary signal is ignored: you should shutdown or terminate\
                before either old or new binary's process", G_STRLOC);
    }

    if (signo == SIGCHLD) {
        cetus_process_get_status();
    }

    errno = err;
}


static void
cetus_process_get_status(void)
{
    int            status;
    char          *process;
    pid_t          pid;
    int            err;
    int            i;
    unsigned int   one;

    one = 0;

    for ( ;; ) {
        pid = waitpid(-1, &status, WNOHANG);

        if (pid == 0) {
            return;
        }

        if (pid == -1) {
            err = errno;

            if (err == EINTR) {
                g_message("%s: EINTR met when waitpid:%d", G_STRLOC, err);
                continue;
            }

            if (err == ECHILD && one) {
                return;
            }

            if (err == ECHILD) {
                g_message("%s: waitpid() failed:%d", G_STRLOC, err);
                return;
            }

            g_critical("%s: waitpid() failed:%d", G_STRLOC, err);
            return;
        }


        one = 1;
        process = "unknown process";

        for (i = 0; i < cetus_last_process; i++) {
            if (cetus_processes[i].pid == pid) {
                cetus_processes[i].status = status;
                cetus_processes[i].exited = 1;
                process = cetus_processes[i].name;
                break;
            }
        }

        if (WTERMSIG(status)) {
            g_critical("%s: %s exited on signal %d",
                    G_STRLOC, process, WTERMSIG(status));
        } else {
            g_message("%s: %s exited on code %d",
                    G_STRLOC, process, WEXITSTATUS(status));
        }

        if (WEXITSTATUS(status) == 2 && cetus_processes[i].respawn) {
            g_critical("%s: cannot be respawned and %P exited with fatal code %d",
                    G_STRLOC, process, pid, WEXITSTATUS(status));
            cetus_processes[i].respawn = 0;
        }

    }
}


int
cetus_os_signal_process(cetus_cycle_t *cycle, char *name, pid_t pid)
{
    cetus_signal_t  *sig;

    for (sig = signals; sig->signo != 0; sig++) {
        if (strcmp(name, sig->name) == 0) {
            if (kill(pid, sig->signo) != -1) {
                return 0;
            }

            g_critical("%s: kill(%d, %d) failed", G_STRLOC, pid, sig->signo);
        }
    }

    return 1;
}

