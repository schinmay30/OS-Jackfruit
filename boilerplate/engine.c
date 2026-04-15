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
    char command[256];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    char message[256];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[256];
    int nice_value;
    int log_write_fd;
} child_config_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

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

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING: return "starting";
    case CONTAINER_RUNNING: return "running";
    case CONTAINER_STOPPED: return "stopped";
    case CONTAINER_KILLED: return "killed";
    case CONTAINER_EXITED: return "exited";
    default: return "unknown";
    }
}

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    memset(buffer, 0, sizeof(*buffer));
    pthread_mutex_init(&buffer->mutex, NULL);
    pthread_cond_init(&buffer->not_empty, NULL);
    pthread_cond_init(&buffer->not_full, NULL);
    return 0;
}

static void bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);
    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);
    }
    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return;
    }
    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;
    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);
}

static int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);
    while (buffer->count == 0 && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);
    }
    if (buffer->shutting_down && buffer->count == 0) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }
    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;
    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

static void bounded_buffer_shutdown(bounded_buffer_t *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

static void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;
    
    while (1) {
        if (bounded_buffer_pop(&ctx->log_buffer, &item) != 0)
            break;
        
        char logpath[PATH_MAX];
        snprintf(logpath, sizeof(logpath), "%s/%s.log", LOG_DIR, item.container_id);
        int fd = open(logpath, O_CREAT | O_WRONLY | O_APPEND, 0644);
        if (fd >= 0) {
            write(fd, item.data, item.length);
            close(fd);
        }
    }
    return NULL;
}

static int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;
    
    /* Set nice value if specified */
    if (cfg->nice_value != 0)
        nice(cfg->nice_value);
    
    /* Redirect stdout/stderr to pipe */
    dup2(cfg->log_write_fd, STDOUT_FILENO);
    dup2(cfg->log_write_fd, STDERR_FILENO);
    close(cfg->log_write_fd);
    
    /* Mount proc */
    mount("proc", "/proc", "proc", 0, NULL);
    
    /* Chroot into container rootfs */
    if (chroot(cfg->rootfs) != 0) {
        perror("chroot");
        return 1;
    }
    chdir("/");
    
    /* Execute the command */
    execlp(cfg->command, cfg->command, NULL);
    perror("execlp");
    return 1;
}

static int register_with_monitor(int monitor_fd, const char *container_id, pid_t host_pid,
                                  unsigned long soft_bytes, unsigned long hard_bytes)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_bytes;
    req.hard_limit_bytes = hard_bytes;
    strncpy(req.container_id, container_id, MONITOR_NAME_LEN - 1);
    return ioctl(monitor_fd, MONITOR_REGISTER, &req);
}

static int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, MONITOR_NAME_LEN - 1);
    return ioctl(monitor_fd, MONITOR_UNREGISTER, &req);
}

/* Structure to track pipe fds for each container */
typedef struct {
    int fd;
    char container_id[CONTAINER_ID_LEN];
} pipe_info_t;

static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    pipe_info_t pipes[32];
    int pipe_count = 0;
    int i;
    
    (void)rootfs;
    memset(&ctx, 0, sizeof(ctx));
    memset(pipes, 0, sizeof(pipes));
    
    pthread_mutex_init(&ctx.metadata_lock, NULL);
    bounded_buffer_init(&ctx.log_buffer);
    mkdir(LOG_DIR, 0755);
    
    /* Open kernel monitor */
    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0)
        perror("open /dev/container_monitor");
    
    /* Create control socket */
    unlink(CONTROL_PATH);
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strcpy(addr.sun_path, CONTROL_PATH);
    bind(ctx.server_fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(ctx.server_fd, 5);
    
    /* Start logging thread */
    pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);
    
    signal(SIGCHLD, SIG_IGN);
    signal(SIGINT, SIG_IGN);
    
    ctx.should_stop = 0;
    
    while (!ctx.should_stop) {
        fd_set read_fds;
        int max_fd = ctx.server_fd;
        
        FD_ZERO(&read_fds);
        FD_SET(ctx.server_fd, &read_fds);
        
        /* Add all pipe fds */
        for (i = 0; i < pipe_count; i++) {
            if (pipes[i].fd > 0) {
                FD_SET(pipes[i].fd, &read_fds);
                if (pipes[i].fd > max_fd) max_fd = pipes[i].fd;
            }
        }
        
        struct timeval tv = {0, 100000};
        select(max_fd + 1, &read_fds, NULL, NULL, &tv);
        
        /* Handle new client connections */
        if (FD_ISSET(ctx.server_fd, &read_fds)) {
            int client = accept(ctx.server_fd, NULL, NULL);
            if (client >= 0) {
                control_request_t req;
                read(client, &req, sizeof(req));
                control_response_t resp = {0};
                
                pthread_mutex_lock(&ctx.metadata_lock);
                
                if (req.kind == CMD_START || req.kind == CMD_RUN) {
                    int pipefd[2];
                    pipe(pipefd);
                    
                    child_config_t cfg;
                    memset(&cfg, 0, sizeof(cfg));
                    strcpy(cfg.id, req.container_id);
                    strcpy(cfg.rootfs, req.rootfs);
                    strcpy(cfg.command, req.command);
                    cfg.nice_value = req.nice_value;
                    cfg.log_write_fd = pipefd[1];
                    
                    char *stack = malloc(STACK_SIZE);
                    pid_t pid = clone(child_fn, stack + STACK_SIZE,
                                      CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | SIGCHLD,
                                      &cfg);
                    free(stack);
                    
                    if (pid == -1) {
                        snprintf(resp.message, sizeof(resp.message), "ERROR clone failed");
                        resp.status = -1;
                        close(pipefd[0]);
                        close(pipefd[1]);
                    } else {
                        container_record_t *rec = calloc(1, sizeof(container_record_t));
                        strcpy(rec->id, req.container_id);
                        rec->host_pid = pid;
                        rec->started_at = time(NULL);
                        rec->state = CONTAINER_RUNNING;
                        rec->soft_limit_bytes = req.soft_limit_bytes;
                        rec->hard_limit_bytes = req.hard_limit_bytes;
                        snprintf(rec->log_path, sizeof(rec->log_path), "%s/%s.log", LOG_DIR, req.container_id);
                        rec->next = ctx.containers;
                        ctx.containers = rec;
                        
                        if (ctx.monitor_fd >= 0)
                            register_with_monitor(ctx.monitor_fd, req.container_id, pid,
                                                  req.soft_limit_bytes, req.hard_limit_bytes);
                        
                        close(pipefd[1]); /* Close write end in supervisor */
                        
                        /* Track the read end */
                        if (pipe_count < 32) {
                            pipes[pipe_count].fd = pipefd[0];
                            strcpy(pipes[pipe_count].container_id, req.container_id);
                            pipe_count++;
                        }
                        
                        snprintf(resp.message, sizeof(resp.message), "OK %d", pid);
                        resp.status = 0;
                    }
                }
                else if (req.kind == CMD_PS) {
                    char buf[4096] = "ID\tPID\tSTATE\tSOFT\tHARD\n";
                    for (container_record_t *r = ctx.containers; r; r = r->next) {
                        char line[256];
                        snprintf(line, sizeof(line), "%s\t%d\t%s\t%lu\t%lu\n",
                                 r->id, r->host_pid, state_to_string(r->state),
                                 r->soft_limit_bytes >> 20, r->hard_limit_bytes >> 20);
                        strcat(buf, line);
                    }
                    write(client, buf, strlen(buf));
                    close(client);
                    pthread_mutex_unlock(&ctx.metadata_lock);
                    continue;
                }
                else if (req.kind == CMD_STOP) {
                    for (container_record_t *r = ctx.containers; r; r = r->next) {
                        if (strcmp(r->id, req.container_id) == 0 && r->state == CONTAINER_RUNNING) {
                            kill(r->host_pid, SIGTERM);
                            r->state = CONTAINER_STOPPED;
                            snprintf(resp.message, sizeof(resp.message), "Stopped %s", req.container_id);
                            break;
                        }
                    }
                    resp.status = 0;
                }
                else if (req.kind == CMD_LOGS) {
                    char logpath[PATH_MAX];
                    snprintf(logpath, sizeof(logpath), "%s/%s.log", LOG_DIR, req.container_id);
                    FILE *f = fopen(logpath, "r");
                    if (f) {
                        char ch;
                        while ((ch = fgetc(f)) != EOF)
                            write(client, &ch, 1);
                        fclose(f);
                    } else {
                        write(client, "No logs found\n", 14);
                    }
                    close(client);
                    pthread_mutex_unlock(&ctx.metadata_lock);
                    continue;
                }
                
                write(client, &resp, sizeof(resp));
                close(client);
                pthread_mutex_unlock(&ctx.metadata_lock);
            }
        }
        
        /* Read data from container pipes */
        for (i = 0; i < pipe_count; i++) {
            if (pipes[i].fd > 0 && FD_ISSET(pipes[i].fd, &read_fds)) {
                char buffer[LOG_CHUNK_SIZE];
                ssize_t n = read(pipes[i].fd, buffer, LOG_CHUNK_SIZE);
                
                if (n > 0) {
                    log_item_t item;
                    memset(&item, 0, sizeof(item));
                    strcpy(item.container_id, pipes[i].container_id);
                    item.length = n;
                    memcpy(item.data, buffer, n);
                    bounded_buffer_push(&ctx.log_buffer, &item);
                } else {
                    /* Pipe closed - container exited */
                    close(pipes[i].fd);
                    pipes[i].fd = -1;
                    
                    /* Update container state */
                    for (container_record_t *r = ctx.containers; r; r = r->next) {
                        if (strcmp(r->id, pipes[i].container_id) == 0) {
                            r->state = CONTAINER_EXITED;
                            break;
                        }
                    }
                }
            }
        }
    }
    
    bounded_buffer_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);
    close(ctx.server_fd);
    if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
    pthread_mutex_destroy(&ctx.metadata_lock);
    
    return 0;
}

static int send_control_request(const control_request_t *req)
{
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strcpy(addr.sun_path, CONTROL_PATH);
    
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect to supervisor (is it running?)");
        return 1;
    }
    
    write(sock, req, sizeof(*req));
    
    if (req->kind == CMD_PS || req->kind == CMD_LOGS) {
        char buf[4096];
        int n = read(sock, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            printf("%s", buf);
        }
    } else {
        control_response_t resp;
        read(sock, &resp, sizeof(resp));
        printf("%s\n", resp.message);
    }
    
    close(sock);
    return 0;
}

static int parse_mib(const char *str, unsigned long *bytes)
{
    char *end;
    unsigned long mib = strtoul(str, &end, 10);
    if (end == str || *end != '\0')
        return -1;
    *bytes = mib << 20;
    return 0;
}

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    
    if (argc < 5) return 1;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    req.nice_value = 0;
    
    for (int i = 5; i < argc; i++) {
        if (strcmp(argv[i], "--soft-mib") == 0 && i + 1 < argc)
            parse_mib(argv[++i], &req.soft_limit_bytes);
        else if (strcmp(argv[i], "--hard-mib") == 0 && i + 1 < argc)
            parse_mib(argv[++i], &req.hard_limit_bytes);
        else if (strcmp(argv[i], "--nice") == 0 && i + 1 < argc)
            req.nice_value = atoi(argv[++i]);
    }
    
    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    
    if (argc < 5) return 1;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    req.nice_value = 0;
    
    for (int i = 5; i < argc; i++) {
        if (strcmp(argv[i], "--soft-mib") == 0 && i + 1 < argc)
            parse_mib(argv[++i], &req.soft_limit_bytes);
        else if (strcmp(argv[i], "--hard-mib") == 0 && i + 1 < argc)
            parse_mib(argv[++i], &req.hard_limit_bytes);
        else if (strcmp(argv[i], "--nice") == 0 && i + 1 < argc)
            req.nice_value = atoi(argv[++i]);
    }
    
    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    if (argc < 3) return 1;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    if (argc < 3) return 1;
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
        if (argc < 3) return 1;
        return run_supervisor(argv[2]);
    }
    if (strcmp(argv[1], "start") == 0) return cmd_start(argc, argv);
    if (strcmp(argv[1], "run") == 0) return cmd_run(argc, argv);
    if (strcmp(argv[1], "ps") == 0) return cmd_ps();
    if (strcmp(argv[1], "logs") == 0) return cmd_logs(argc, argv);
    if (strcmp(argv[1], "stop") == 0) return cmd_stop(argc, argv);
    
    usage(argv[0]);
    return 1;
}
