/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Intentionally partial starter:
 *   - command-line shape is defined
 *   - key runtime data structures are defined
 *   - bounded-buffer skeleton is defined
 *   - supervisor / client split is outlined
 *
 * Students are expected to design:
 *   - the control-plane IPC implementation
 *   - container lifecycle and metadata synchronization
 *   - clone + namespace setup for each container
 *   - producer/consumer behavior for log buffering
 *   - signal handling and graceful shutdown
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    char log_path[PATH_MAX];
    struct container_record *next;

    /* Additional runtime fields: */
    int log_fd;                 /* open file descriptor for log output */
    int stop_requested;         /* flag set when a stop command is issued */
    pthread_cond_t exit_cond;   /* signaled when the container exits */
    void *stack;                /* pointer to allocated stack for clone */
    struct child_config *child_cfg;  /* configuration passed to the child process */
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct child_config {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;
} child_config_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    int should_stop;
    pthread_t logger_thread;
    pthread_t signal_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

/* Structure used to pass arguments to client handler threads. */
typedef struct client_handler_args {
    int fd;
    supervisor_ctx_t *ctx;
} client_handler_args_t;

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag,
                          const char *value,
                          unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;

    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }

    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }

    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req,
                                int argc,
                                char *argv[],
                                int start_index)
{
    int i;

    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }

        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr,
                        "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }

        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }

    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }

    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING:
        return "starting";
    case CONTAINER_RUNNING:
        return "running";
    case CONTAINER_STOPPED:
        return "stopped";
    case CONTAINER_KILLED:
        return "killed";
    case CONTAINER_EXITED:
        return "exited";
    default:
        return "unknown";
    }
}

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;

    memset(buffer, 0, sizeof(*buffer));

    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0)
        return rc;

    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) {
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buffer->not_empty);
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer)
{
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

/*
 * TODO:
 * Implement producer-side insertion into the bounded buffer.
 *
 * Requirements:
 *   - block or fail according to your chosen policy when the buffer is full
 *   - wake consumers correctly
 *   - stop cleanly if shutdown begins
 */
int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    int rc = 0;
    if (!buffer || !item)
        return -1;
    pthread_mutex_lock(&buffer->mutex);
    while (!buffer->shutting_down && buffer->count == LOG_BUFFER_CAPACITY) {
        /* Buffer full: wait until a consumer removes an item or
         * shutdown begins.  Use cond_wait on not_full.
         */
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);
    }
    if (buffer->shutting_down) {
        rc = -1;
    } else {
        buffer->items[buffer->tail] = *item;
        buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
        buffer->count++;
        pthread_cond_signal(&buffer->not_empty);
        rc = 0;
    }
    pthread_mutex_unlock(&buffer->mutex);
    return rc;
}

/*
 * TODO:
 * Implement consumer-side removal from the bounded buffer.
 *
 * Requirements:
 *   - wait correctly while the buffer is empty
 *   - return a useful status when shutdown is in progress
 *   - avoid races with producers and shutdown
 */
int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    int rc = 0;
    if (!buffer || !item)
        return -1;
    pthread_mutex_lock(&buffer->mutex);
    while (buffer->count == 0 && !buffer->shutting_down) {
        /* Buffer empty: wait until a producer inserts an item or
         * shutdown begins.  Use cond_wait on not_empty.
         */
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);
    }
    if (buffer->count == 0 && buffer->shutting_down) {
        rc = -1;
    } else {
        *item = buffer->items[buffer->head];
        buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
        buffer->count--;
        pthread_cond_signal(&buffer->not_full);
        rc = 0;
    }
    pthread_mutex_unlock(&buffer->mutex);
    return rc;
}

/*
 * TODO:
 * Implement the logging consumer thread.
 *
 * Suggested responsibilities:
 *   - remove log chunks from the bounded buffer
 *   - route each chunk to the correct per-container log file
 *   - exit cleanly when shutdown begins and pending work is drained
 */
void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;
    while (1) {
        if (bounded_buffer_pop(&ctx->log_buffer, &item) != 0) {
            /* Shutting down and buffer drained. */
            break;
        }
        /* Locate container record by ID to obtain its log_fd. */
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *cur = ctx->containers;
        int fd = -1;
        while (cur) {
            if (strncmp(cur->id, item.container_id, CONTAINER_ID_LEN) == 0) {
                fd = cur->log_fd;
                break;
            }
            cur = cur->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
        if (fd >= 0) {
            size_t remaining = item.length;
            const char *ptr = item.data;
            while (remaining > 0) {
                ssize_t w = write(fd, ptr, remaining);
                if (w <= 0) {
                    /* ignore errors */
                    break;
                }
                ptr += w;
                remaining -= (size_t)w;
            }
        }
    }
    return NULL;
}

/*
 * TODO:
 * Implement the clone child entrypoint.
 *
 * Required outcomes:
 *   - isolated PID / UTS / mount context
 *   - chroot or pivot_root into rootfs
 *   - working /proc inside container
 *   - stdout / stderr redirected to the supervisor logging path
 *   - configured command executed inside the container
 */
int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;
    /* Apply nice value adjustment if requested. */
    if (cfg->nice_value != 0) {
        nice(cfg->nice_value);
    }
    /* Redirect stdout and stderr to the logging pipe. */
    if (dup2(cfg->log_write_fd, STDOUT_FILENO) < 0 ||
        dup2(cfg->log_write_fd, STDERR_FILENO) < 0) {
        perror("dup2");
        _exit(1);
    }
    close(cfg->log_write_fd);
    /* Change into the new root filesystem and perform chroot. */
    if (chroot(cfg->rootfs) != 0) {
    perror("chroot rootfs");
    _exit(1);
}
if (chdir("/") != 0) {
    perror("chdir /");
    _exit(1);
}
    /* Mount proc inside container; ignore errors. */
    (void)mount("proc", "/proc", "proc", 0, NULL);
    /* Execute the target command via /bin/sh -c. */
    execl("/bin/sh", "sh", "-c", cfg->command, (char *)NULL);
    perror("exec");
    _exit(1);
}

/* Forward declarations for helper threads and functions. */
static void *signal_thread_fn(void *arg);
static void *client_handler_fn(void *arg);

/* Producer thread for reading container output and pushing into the
 * bounded buffer.  Each container has one producer thread that reads
 * from the read end of its logging pipe and packages chunks into
 * log_item_t structures.  The thread exits when EOF is detected or
 * when the log buffer begins shutdown.
 */
typedef struct producer_args {
    supervisor_ctx_t *ctx;
    char container_id[CONTAINER_ID_LEN];
    int read_fd;
} producer_args_t;

static void *producer_thread_fn(void *arg)
{
    producer_args_t *pargs = (producer_args_t *)arg;
    supervisor_ctx_t *ctx = pargs->ctx;
    char buf[LOG_CHUNK_SIZE];
    while (1) {
        ssize_t n = read(pargs->read_fd, buf, sizeof(buf));
        if (n > 0) {
            log_item_t item;
            memset(&item, 0, sizeof(item));
            strncpy(item.container_id, pargs->container_id, sizeof(item.container_id) - 1);
            item.length = (size_t)n;
            memcpy(item.data, buf, (size_t)n);
            if (bounded_buffer_push(&ctx->log_buffer, &item) != 0) {
                /* shutdown in progress */
                break;
            }
        } else {
            break;
        }
    }
    close(pargs->read_fd);
    free(pargs);
    return NULL;
}

/* Find a container record by ID.  Must be called with ctx->metadata_lock held. */
static container_record_t *find_container_locked(supervisor_ctx_t *ctx, const char *id)
{
    container_record_t *cur = ctx->containers;
    while (cur) {
        if (strncmp(cur->id, id, CONTAINER_ID_LEN) == 0)
            return cur;
        cur = cur->next;
    }
    return NULL;
}

/* Start a new container.  On success, returns 0 and stores a pointer to
 * the newly created container_record_t in *rec_out (with metadata_lock
 * still held by the caller).  On failure, returns non-zero and
 * *rec_out is left unchanged.
 */
static int start_container(supervisor_ctx_t *ctx,
                           const control_request_t *req,
                           container_record_t **rec_out)
{
    int pipefd[2];
    container_record_t *rec = NULL;
    child_config_t *child_cfg = NULL;
    void *stack = NULL;
    int log_fd = -1;
    int status = -1;

    /* Prepare log file path and open for append. */
    rec = calloc(1, sizeof(*rec));
    if (!rec)
        goto fail;
    strncpy(rec->id, req->container_id, sizeof(rec->id) - 1);
    rec->soft_limit_bytes = req->soft_limit_bytes;
    rec->hard_limit_bytes = req->hard_limit_bytes;
    rec->state = CONTAINER_STARTING;
    rec->started_at = time(NULL);
    rec->exit_code = 0;
    rec->exit_signal = 0;
    rec->stop_requested = 0;
    rec->next = NULL;
    rec->stack = NULL;
    rec->child_cfg = NULL;
    snprintf(rec->log_path, sizeof(rec->log_path), "%s/%s.log", LOG_DIR, rec->id);
    log_fd = open(rec->log_path, O_CREAT | O_WRONLY | O_APPEND, 0644);
    if (log_fd < 0) {
        perror("open log file");
        goto fail;
    }
    rec->log_fd = log_fd;
    /* Initialize exit condition variable. */
    if (pthread_cond_init(&rec->exit_cond, NULL) != 0) {
        perror("pthread_cond_init");
        goto fail;
    }
    /* Allocate child configuration and stack. */
    child_cfg = calloc(1, sizeof(*child_cfg));
    if (!child_cfg)
        goto fail;
    strncpy(child_cfg->id, req->container_id, sizeof(child_cfg->id) - 1);
    strncpy(child_cfg->rootfs, req->rootfs, sizeof(child_cfg->rootfs) - 1);
    strncpy(child_cfg->command, req->command, sizeof(child_cfg->command) - 1);
    child_cfg->nice_value = req->nice_value;
    child_cfg->log_write_fd = -1;

    stack = malloc(STACK_SIZE);
    if (!stack)
        goto fail;
    /* Create pipe for logging. */
    if (pipe(pipefd) < 0) {
        perror("pipe");
        goto fail;
    }
    child_cfg->log_write_fd = pipefd[1];
    /* Clone flags for PID, UTS, and mount namespaces and deliver SIGCHLD on exit. */
    int clone_flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD;
    pid_t pid = clone(child_fn, (char *)stack + STACK_SIZE, clone_flags, child_cfg);
    if (pid < 0) {
        perror("clone");
        close(pipefd[0]);
        close(pipefd[1]);
        goto fail;
    }
    /* Parent: close write end and save read end. */
    close(pipefd[1]);
    rec->host_pid = pid;
    rec->stack = stack;
    rec->child_cfg = child_cfg;
    /* Spawn producer thread for reading from pipe. */
    producer_args_t *pargs = malloc(sizeof(*pargs));
    if (!pargs) {
        /* best effort: still proceed without logging */
        close(pipefd[0]);
    } else {
        pargs->ctx = ctx;
        strncpy(pargs->container_id, rec->id, sizeof(pargs->container_id) - 1);
        pargs->container_id[sizeof(pargs->container_id) - 1] = '\0';
        pargs->read_fd = pipefd[0];
        pthread_t prod_th;
        if (pthread_create(&prod_th, NULL, producer_thread_fn, pargs) == 0) {
            pthread_detach(prod_th);
        } else {
            /* If thread creation fails, close the fd and free args. */
            free(pargs);
            close(pipefd[0]);
        }
    }
    /* Register with kernel monitor.  Ignore errors but warn. */
    if (register_with_monitor(ctx->monitor_fd, rec->id, rec->host_pid,
                              rec->soft_limit_bytes, rec->hard_limit_bytes) < 0) {
        perror("register_with_monitor");
    }
    /* Insert container into metadata list.  Caller holds metadata_lock. */
    rec->next = ctx->containers;
    ctx->containers = rec;
    /* Update state to running. */
    rec->state = CONTAINER_RUNNING;
    if (rec_out)
        *rec_out = rec;
    return 0;
fail:
    if (stack)
        free(stack);
    if (child_cfg) {
        if (child_cfg->log_write_fd >= 0)
            close(child_cfg->log_write_fd);
        free(child_cfg);
    }
    if (rec) {
        if (rec->log_fd >= 0)
            close(rec->log_fd);
        /* Destroy condition variable if initialised. */
        pthread_cond_destroy(&rec->exit_cond);
        free(rec);
    }
    return -1;
}

/* Update container metadata upon receiving a child exit.  Called from
 * the signal-handling thread when waitpid indicates a child has
 * terminated.  The metadata_lock must be held when calling this
 * function.
 */
static void handle_child_exit(supervisor_ctx_t *ctx, pid_t pid, int status)
{
    container_record_t *rec = ctx->containers;
    while (rec) {
        if (rec->host_pid == pid)
            break;
        rec = rec->next;
    }
    if (!rec)
        return;
    /* Determine exit code and signal. */
    if (WIFEXITED(status)) {
        rec->exit_code = WEXITSTATUS(status);
        rec->exit_signal = 0;
    } else if (WIFSIGNALED(status)) {
        rec->exit_code = 0;
        rec->exit_signal = WTERMSIG(status);
    }
    /* Unregister from the kernel monitor.  Ignore errors. */
    unregister_from_monitor(ctx->monitor_fd, rec->id, rec->host_pid);
    /* Close log file descriptor. */
    if (rec->log_fd >= 0) {
        close(rec->log_fd);
        rec->log_fd = -1;
    }
    /* Determine final state. */
    if (rec->stop_requested) {
        rec->state = CONTAINER_STOPPED;
    } else if (rec->exit_signal == SIGKILL) {
        rec->state = CONTAINER_KILLED;
    } else {
        rec->state = CONTAINER_EXITED;
    }
    /* Signal any waiting threads on this record. */
    pthread_cond_broadcast(&rec->exit_cond);
}

/* Signal-handling thread.  Waits synchronously for SIGCHLD, SIGINT,
 * and SIGTERM.  On SIGCHLD, reaps children and updates metadata.  On
 * SIGINT or SIGTERM, initiates supervisor shutdown by setting
 * ctx->should_stop and sending SIGTERM to all running containers.
 */
static void *signal_thread_fn(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    sigset_t sigset;
    sigemptyset(&sigset);
    sigaddset(&sigset, SIGCHLD);
    sigaddset(&sigset, SIGINT);
    sigaddset(&sigset, SIGTERM);
    while (1) {
        int sig;
        if (sigwait(&sigset, &sig) != 0)
            continue;
        if (sig == SIGCHLD) {
            /* Reap all exited children. */
            while (1) {
                int status;
                pid_t pid = waitpid(-1, &status, WNOHANG);
                if (pid <= 0)
                    break;
                pthread_mutex_lock(&ctx->metadata_lock);
                handle_child_exit(ctx, pid, status);
                pthread_mutex_unlock(&ctx->metadata_lock);
            }
        } else if (sig == SIGINT || sig == SIGTERM) {
            /* Initiate shutdown. */
            pthread_mutex_lock(&ctx->metadata_lock);
            ctx->should_stop = 1;
            /* Mark stop_requested and send SIGTERM to running containers. */
            container_record_t *cur = ctx->containers;
            while (cur) {
                if (cur->state == CONTAINER_RUNNING || cur->state == CONTAINER_STARTING) {
                    cur->stop_requested = 1;
                    kill(cur->host_pid, SIGTERM);
                }
                cur = cur->next;
            }
            pthread_mutex_unlock(&ctx->metadata_lock);
            /* Wake up accept loop. */
            close(ctx->server_fd);
            break;
        }
    }
    return NULL;
}

/* Handle a single client connection.  Reads a control_request_t,
 * executes the appropriate action, and writes a control_response_t
 * back to the client.  The handler frees its argument and closes
 * the client socket when finished.
 */
static void *client_handler_fn(void *arg)
{
    client_handler_args_t *handler_args = (client_handler_args_t *)arg;
    int client_fd = handler_args->fd;
    supervisor_ctx_t *ctx = handler_args->ctx;
    free(handler_args);
    control_request_t req;
    control_response_t resp;
    memset(&req, 0, sizeof(req));
    memset(&resp, 0, sizeof(resp));
    /* Read full request. */
    ssize_t n = read(client_fd, &req, sizeof(req));
    if (n != sizeof(req)) {
        resp.status = 1;
        snprintf(resp.message, sizeof(resp.message), "Invalid request size");
        write(client_fd, &resp, sizeof(resp));
        close(client_fd);
        return NULL;
    }
    switch (req.kind) {
    case CMD_START:
    case CMD_RUN: {
        pthread_mutex_lock(&ctx->metadata_lock);
        /* Ensure no container with the same ID is currently running. */
        container_record_t *existing = find_container_locked(ctx, req.container_id);
        if (existing && (existing->state == CONTAINER_RUNNING || existing->state == CONTAINER_STARTING)) {
            resp.status = 1;
            snprintf(resp.message, sizeof(resp.message), "Container %s already running", req.container_id);
            pthread_mutex_unlock(&ctx->metadata_lock);
            break;
        }
        container_record_t *rec = NULL;
        if (start_container(ctx, &req, &rec) != 0) {
            resp.status = 1;
            snprintf(resp.message, sizeof(resp.message), "Failed to start container %s", req.container_id);
            pthread_mutex_unlock(&ctx->metadata_lock);
            break;
        }
        if (req.kind == CMD_START) {
            resp.status = 0;
            snprintf(resp.message, sizeof(resp.message), "Started container %s", req.container_id);
            pthread_mutex_unlock(&ctx->metadata_lock);
            break;
        }
        /* CMD_RUN: wait until the container has exited. */
        while (rec->state == CONTAINER_STARTING || rec->state == CONTAINER_RUNNING) {
            pthread_cond_wait(&rec->exit_cond, &ctx->metadata_lock);
        }
        int exit_status;
        if (rec->exit_signal != 0) {
            exit_status = 128 + rec->exit_signal;
        } else {
            exit_status = rec->exit_code;
        }
        resp.status = exit_status;
        snprintf(resp.message, sizeof(resp.message), "Container %s exited with status %d", req.container_id, exit_status);
        pthread_mutex_unlock(&ctx->metadata_lock);
        break;
    }
    case CMD_PS: {
        pthread_mutex_lock(&ctx->metadata_lock);
        char *ptr = resp.message;
        size_t remaining = sizeof(resp.message);
        container_record_t *cur = ctx->containers;
        while (cur) {
            int len = snprintf(ptr, remaining, "%s\t%d\t%s\t%lu\t%lu\t%s\n",
                               cur->id, cur->host_pid,
                               state_to_string(cur->state),
                               cur->soft_limit_bytes >> 20,
                               cur->hard_limit_bytes >> 20,
                               ctime(&cur->started_at));
            if (len < 0 || (size_t)len >= remaining) {
                /* Truncate output */
                break;
            }
            ptr += len;
            remaining -= (size_t)len;
            cur = cur->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
        resp.status = 0;
        break;
    }
    case CMD_LOGS: {
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *rec = find_container_locked(ctx, req.container_id);
        if (!rec) {
            resp.status = 1;
            snprintf(resp.message, sizeof(resp.message), "Container %s not found", req.container_id);
        } else {
            resp.status = 0;
            strncpy(resp.message, rec->log_path, sizeof(resp.message) - 1);
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
        break;
    }
    case CMD_STOP: {
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *rec = find_container_locked(ctx, req.container_id);
        if (!rec || (rec->state != CONTAINER_RUNNING && rec->state != CONTAINER_STARTING)) {
            resp.status = 1;
            snprintf(resp.message, sizeof(resp.message), "Container %s not running", req.container_id);
            pthread_mutex_unlock(&ctx->metadata_lock);
            break;
        }
        rec->stop_requested = 1;
        kill(rec->host_pid, SIGTERM);
        resp.status = 0;
        snprintf(resp.message, sizeof(resp.message), "Sent stop signal to %s", req.container_id);
        pthread_mutex_unlock(&ctx->metadata_lock);
        break;
    }
    default:
        resp.status = 1;
        snprintf(resp.message, sizeof(resp.message), "Unknown command");
        break;
    }
    /* Write response. */
    write(client_fd, &resp, sizeof(resp));
    close(client_fd);
    return NULL;
}

int register_with_monitor(int monitor_fd,
                          const char *container_id,
                          pid_t host_pid,
                          unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;

    return 0;
}

int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;

    return 0;
}

/*
 * TODO:
 * Implement the long-running supervisor process.
 *
 * Suggested responsibilities:
 *   - create and bind the control-plane IPC endpoint
 *   - initialize shared metadata and the bounded buffer
 *   - start the logging thread
 *   - accept control requests and update container state
 *   - reap children and respond to signals
 */
static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    int rc;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) {
        errno = rc;
        perror("pthread_mutex_init");
        return 1;
    }

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) {
        errno = rc;
        perror("bounded_buffer_init");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    /* Open the kernel monitor device. */
    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0) {
        perror("open /dev/container_monitor");
        bounded_buffer_destroy(&ctx.log_buffer);
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    /* Create logs directory if it does not exist. */
    if (mkdir(LOG_DIR, 0755) < 0 && errno != EEXIST) {
        perror("mkdir logs");
        close(ctx.monitor_fd);
        bounded_buffer_destroy(&ctx.log_buffer);
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    /* Setup UNIX domain socket for control-plane IPC.  Remove any
     * existing socket file and bind to the defined path.  Then
     * listen for incoming connections.
     */
    unlink(CONTROL_PATH);
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) {
        perror("socket AF_UNIX");
        close(ctx.monitor_fd);
        bounded_buffer_destroy(&ctx.log_buffer);
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);
    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind control socket");
        close(ctx.server_fd);
        close(ctx.monitor_fd);
        bounded_buffer_destroy(&ctx.log_buffer);
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }
    if (listen(ctx.server_fd, 10) < 0) {
        perror("listen control socket");
        close(ctx.server_fd);
        close(ctx.monitor_fd);
        bounded_buffer_destroy(&ctx.log_buffer);
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    /* Block SIGCHLD, SIGINT, and SIGTERM in this and all new threads.
     * A dedicated signal thread will handle these signals via sigwait().
     */
    sigset_t sigset;
    sigemptyset(&sigset);
    sigaddset(&sigset, SIGCHLD);
    sigaddset(&sigset, SIGINT);
    sigaddset(&sigset, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &sigset, NULL) != 0) {
        perror("pthread_sigmask");
        close(ctx.server_fd);
        close(ctx.monitor_fd);
        bounded_buffer_destroy(&ctx.log_buffer);
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    /* Start the logging thread. */
    rc = pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);
    if (rc != 0) {
        errno = rc;
        perror("pthread_create logger");
        close(ctx.server_fd);
        close(ctx.monitor_fd);
        bounded_buffer_destroy(&ctx.log_buffer);
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }
    /* Start the signal-handling thread. */
    rc = pthread_create(&ctx.signal_thread, NULL, signal_thread_fn, &ctx);
    if (rc != 0) {
        errno = rc;
        perror("pthread_create signal");
        /* request logger shutdown */
        bounded_buffer_begin_shutdown(&ctx.log_buffer);
        pthread_join(ctx.logger_thread, NULL);
        close(ctx.server_fd);
        close(ctx.monitor_fd);
        bounded_buffer_destroy(&ctx.log_buffer);
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    /* Main accept loop.  Each connection is handled by a detached
     * thread.  The loop terminates when ctx.should_stop is set by
     * signal_thread_fn.
     */
    while (!ctx.should_stop) {
        int client_fd = accept(ctx.server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR)
                continue;
            perror("accept");
            break;
        }
        client_handler_args_t *handler_args = malloc(sizeof(*handler_args));
        if (!handler_args) {
            perror("malloc handler_args");
            close(client_fd);
            continue;
        }
        handler_args->fd = client_fd;
        handler_args->ctx = &ctx;
        pthread_t th;
        rc = pthread_create(&th, NULL, client_handler_fn, handler_args);
        if (rc != 0) {
            errno = rc;
            perror("pthread_create client_handler");
            close(client_fd);
            free(handler_args);
            continue;
        }
        pthread_detach(th);
    }

    /* Begin shutdown of logging pipeline. */
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    /* Join logger and signal threads. */
    pthread_join(ctx.logger_thread, NULL);
    pthread_join(ctx.signal_thread, NULL);

    /* Clean up control socket. */
    close(ctx.server_fd);
    unlink(CONTROL_PATH);
    /* Close monitor device. */
    close(ctx.monitor_fd);

    /* Free all container metadata and associated resources. */
    pthread_mutex_lock(&ctx.metadata_lock);
    container_record_t *cur = ctx.containers;
    while (cur) {
        container_record_t *next = cur->next;
        if (cur->log_fd >= 0)
            close(cur->log_fd);
        pthread_cond_destroy(&cur->exit_cond);
        free(cur->stack);
        free(cur->child_cfg);
        free(cur);
        cur = next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    /* Destroy data structures. */
    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);
    return 0;
}

/*
 * TODO:
 * Implement the client-side control request path.
 *
 * The CLI commands should use a second IPC mechanism distinct from the
 * logging pipe. A UNIX domain socket is the most direct option, but a
 * FIFO or shared memory design is also acceptable if justified.
 */
static int send_control_request(const control_request_t *req)
{
    int sockfd;
    struct sockaddr_un addr;
    control_response_t resp;
    ssize_t n;

    sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket AF_UNIX");
        return 1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);
    if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect control socket");
        close(sockfd);
        return 1;
    }
    /* Send the request. */
    n = write(sockfd, req, sizeof(*req));
    if (n != sizeof(*req)) {
        perror("write control request");
        close(sockfd);
        return 1;
    }
    /* Read the response. */
    n = read(sockfd, &resp, sizeof(resp));
    if (n != sizeof(resp)) {
        perror("read control response");
        close(sockfd);
        return 1;
    }
    int rc = 0;
    switch (req->kind) {
    case CMD_PS:
        /* Print the supervisor's container listing. */
        printf("%s", resp.message);
        rc = resp.status;
        break;
    case CMD_LOGS:
        /* For logs command: if status non-zero, print error.  Otherwise,
         * resp.message contains the log file path; read and print it.
         */
        if (resp.status != 0) {
            fprintf(stderr, "%s\n", resp.message);
            rc = 1;
        } else {
            FILE *fp = fopen(resp.message, "r");
            if (!fp) {
                perror("fopen log file");
                rc = 1;
            } else {
                char buf[4096];
                size_t len;
                while ((len = fread(buf, 1, sizeof(buf), fp)) > 0) {
                    fwrite(buf, 1, len, stdout);
                }
                fclose(fp);
            }
        }
        break;
    case CMD_RUN:
        /* Status for run is the container exit status.  Print message
         * and return the exit status for use as process exit code.
         */
        printf("%s\n", resp.message);
        rc = resp.status;
        break;
    default:
        /* Generic handling for start/stop and unknown commands. */
        if (resp.status != 0) {
            fprintf(stderr, "%s\n", resp.message);
            rc = 1;
        } else {
            printf("%s\n", resp.message);
            rc = 0;
        }
        break;
    }
    close(sockfd);
    return rc;
}

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;

    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;

    /* Send the PS request to the supervisor.  The supervisor will
     * format and return the metadata for all tracked containers.
     */
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "start") == 0)
        return cmd_start(argc, argv);

    if (strcmp(argv[1], "run") == 0)
        return cmd_run(argc, argv);

    if (strcmp(argv[1], "ps") == 0)
        return cmd_ps();

    if (strcmp(argv[1], "logs") == 0)
        return cmd_logs(argc, argv);

    if (strcmp(argv[1], "stop") == 0)
        return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
