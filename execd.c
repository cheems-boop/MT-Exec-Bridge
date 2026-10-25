/* execd.c v2 —— root 命令桥（MT Exec Bridge）
 *
 * v2 修复清单：
 *  1. flock 单实例锁，防重复守护进程
 *  2. in/ 下任意 *.sh 按文件名排序成任务队列，多客户端并发不再互相覆盖
 *  3. 动态读取整个脚本（上限 8MB），超限/写入竞态显式报错并隔离，不再静默截断
 *  4. fork+exec+setpgid+waitpid，超时看门狗杀整个进程组（默认 600s，#!timeout=N 覆盖，0=不限）
 *  5. 输出上限 8MB，超限截断并标注，防止 result.txt 无限膨胀
 *  6. 每任务前自动 source in/env.sh（持久环境变量），内置 PATH/HOME/TMPDIR 合理默认
 *  7. #!async 首行指令 → 双 fork 后台运行，日志写 out/<任务名>.log，不阻塞队列
 *  8. out/history.log 审计：时间/任务/来源uid/耗时/退出码/输出量/命令首行（>4MB 自动轮转）
 *  9. 结果文件 .tmp+rename 原子发布，读者永远看不到半截结果
 * 10. 客户端协议：写临时名（.开头）后 rename 成 in/job-*.sh 提交，杜绝半截脚本竞态
 *
 * 编译：Termux  clang -O2 -o execd execd.c
 *       NDK     aarch64-linux-android24-clang -O2 -o execd execd.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <dirent.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/file.h>

#define MOD         "/data/adb/modules/mt_exec_bridge"
#define IN_DIR      MOD "/in"
#define OUT_DIR     MOD "/out"
#define RESULT      OUT_DIR "/result.txt"
#define RESULT_TMP  OUT_DIR "/.result.tmp"
#define HISTORY     OUT_DIR "/history.log"
#define HISTORY_1   OUT_DIR "/history.log.1"
#define PIDFILE     OUT_DIR "/execd.pid"
#define LOCKFILE    OUT_DIR "/execd.lock"
#define ENV_FILE    IN_DIR "/env.sh"
#define ENVPREFIX   "[ -f \"" ENV_FILE "\" ] && . \"" ENV_FILE "\"\n"

#define MAX_SCRIPT  (8L*1024*1024)
#define MAX_OUTPUT  (8L*1024*1024)
#define HIST_ROTATE (4L*1024*1024)
#define DEFAULT_TIMEOUT 600
#define POLL_MS     200

static long g_out_bytes;
static long g_out_cap;
static int  g_truncated;

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void human_time(char *buf, size_t n) {
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(buf, n, "%Y-%m-%d %H:%M:%S", &tmv);
}

static void ensure_dirs(void) {
    mkdir(IN_DIR, 0700); mkdir(OUT_DIR, 0700);
    chmod(IN_DIR, 0700); chmod(OUT_DIR, 0700);
}

static void setup_env(void) {
    setenv("PATH",
        "/data/data/com.termux/files/usr/bin:"
        "/data/adb/ksu/bin:"
        "/system/bin:/system/xbin:/vendor/bin", 1);
    setenv("TERM", "dumb", 0);
    struct stat st;
    if (stat("/data/data/com.termux/files/home", &st) == 0 && S_ISDIR(st.st_mode)) {
        setenv("HOME",   "/data/data/com.termux/files/home", 0);
        setenv("TMPDIR", "/data/data/com.termux/files/usr/tmp", 0);
    } else {
        setenv("HOME",   "/data/local/tmp", 0);
        setenv("TMPDIR", "/data/local/tmp", 0);
    }
}

static void out_write(int fd, const char *buf, size_t n) {
    if (g_truncated || n == 0) return;
    if ((long)n > g_out_cap - g_out_bytes) {
        n = (size_t)(g_out_cap - g_out_bytes);
        g_truncated = 1;
    }
    if (n > 0 && write(fd, buf, n) > 0) g_out_bytes += (long)n;
    if (g_truncated) {
        const char *msg = "\n[OUTPUT TRUNCATED: hit 8MB cap]\n";
        ssize_t ig = write(fd, msg, strlen(msg)); (void)ig;
    }
}

static void hist_rotate_if_needed(void) {
    struct stat st;
    if (stat(HISTORY, &st) == 0 && st.st_size > HIST_ROTATE)
        rename(HISTORY, HISTORY_1);
}

static void audit(const char *job, uid_t wuid, int exitcode, double ms,
                  long bytes, const char *firstline) {
    hist_rotate_if_needed();
    FILE *h = fopen(HISTORY, "a");
    if (!h) return;
    char tbuf[32]; human_time(tbuf, sizeof tbuf);
    char line[161];
    snprintf(line, sizeof line, "%s", firstline ? firstline : "");
    for (char *p = line; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
    fprintf(h, "%s | %s | uid=%d | %.0fms | exit=%d | out=%ldB | %s\n",
            tbuf, job, (int)wuid, ms, exitcode, bytes, line);
    fclose(h);
}

static void first_line(const char *s, char *buf, size_t n) {
    const char *eol = strchr(s, '\n');
    size_t len = eol ? (size_t)(eol - s) : strlen(s);
    if (len >= n) len = n - 1;
    memcpy(buf, s, len); buf[len] = 0;
}

typedef struct { int timeout; int async; const char *body; } directives;

static void parse_directives(const char *script, directives *d) {
    d->timeout = DEFAULT_TIMEOUT; d->async = 0; d->body = script;
    if (strncmp(script, "#!", 2) != 0) return;
    const char *eol = strchr(script, '\n');
    if (!eol) eol = script + strlen(script);
    char line[256];
    size_t len = (size_t)(eol - script);
    if (len > sizeof(line) - 1) len = sizeof(line) - 1;
    memcpy(line, script, len); line[len] = 0;
    if (strstr(line, "async")) d->async = 1;
    const char *t = strstr(line, "timeout=");
    if (t) { int v = atoi(t + 8); if (v >= 0) d->timeout = v; }
    d->body = (*eol == '\n') ? eol + 1 : eol;
}

static int read_script(const char *path, char **out, char *err, size_t errsz) {
    *out = NULL;
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, errsz, "open: %s", strerror(errno)); return -1; }
    struct stat st;
    if (fstat(fileno(f), &st) != 0 || st.st_size < 0) {
        fclose(f); snprintf(err, errsz, "fstat: %s", strerror(errno)); return -1;
    }
    if (st.st_size > MAX_SCRIPT) {
        fclose(f);
        snprintf(err, errsz, "script %ldB exceeds %ldB cap",
                 (long)st.st_size, (long)MAX_SCRIPT);
        return -2;
    }
    size_t cap = (size_t)st.st_size + 2;
    char *buf = malloc(cap);
    if (!buf) { fclose(f); snprintf(err, errsz, "oom"); return -1; }
    size_t n = fread(buf, 1, cap - 2, f);
    int extra = fgetc(f);
    fclose(f);
    if (extra != EOF) {
        free(buf);
        snprintf(err, errsz, "file grew while reading (writer race?)");
        return -2;
    }
    if (n == 0) { free(buf); snprintf(err, errsz, "empty script"); return -2; }
    buf[n] = 0;
    *out = buf;
    return 0;
}

static int run_async(const char *body, const char *logpath) {
    fflush(NULL);
    pid_t mid = fork();
    if (mid < 0) return -1;
    if (mid == 0) {
        pid_t kid = fork();
        if (kid < 0) _exit(127);
        if (kid == 0) {
            setsid();
            int dn = open("/dev/null", O_RDONLY);
            if (dn >= 0) { dup2(dn, 0); if (dn > 2) close(dn); }
            int lf = open(logpath, O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (lf >= 0) { dup2(lf, 1); dup2(lf, 2); if (lf > 2) close(lf); }
            execl("/system/bin/sh", "sh", "-c", body, (char *)NULL);
            _exit(127);
        }
        _exit(0);
    }
    int st;
    while (waitpid(mid, &st, 0) < 0 && errno == EINTR) {}
    return 0;
}

static int run_sync(int ofd, const char *body, int timeout,
                    int *exitcode, int *timed_out) {
    int pfd[2];
    *timed_out = 0; *exitcode = -1;
    if (pipe(pfd) != 0) return -1;
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); return -1; }
    if (pid == 0) {
        setpgid(0, 0);
        int dn = open("/dev/null", O_RDONLY);
        if (dn >= 0) { dup2(dn, 0); if (dn > 2) close(dn); }
        dup2(pfd[1], 1); dup2(pfd[1], 2);
        close(pfd[0]); close(pfd[1]);
        execl("/system/bin/sh", "sh", "-c", body, (char *)NULL);
        _exit(127);
    }
    close(pfd[1]);
    setpgid(pid, pid);
    {   /* v2.1 fix: 非阻塞读，防止排空管道时被静默子进程卡死导致看门狗失效 */
        int flg = fcntl(pfd[0], F_GETFL);
        if (flg >= 0) fcntl(pfd[0], F_SETFL, flg | O_NONBLOCK);
    }
    int status = 0, reaped = 0, to = 0;
    time_t deadline = 0;
    if (timeout > 0) deadline = time(NULL) + (time_t)timeout;
    char buf[65536];
    for (;;) {
        struct pollfd pf; pf.fd = pfd[0]; pf.events = POLLIN; pf.revents = 0;
        int pr = poll(&pf, 1, 500);
        if (pr > 0 && (pf.revents & (POLLIN | POLLHUP))) {
            ssize_t r;
            while ((r = read(pfd[0], buf, sizeof(buf))) > 0)
                out_write(ofd, buf, (size_t)r);
            if (r == 0) usleep(10000);
        }
        if (!reaped) {
            pid_t w = waitpid(pid, &status, WNOHANG);
            if (w == pid) reaped = 1;
        }
        if (reaped || to) {
            int flg = fcntl(pfd[0], F_GETFL);
            if (flg >= 0) fcntl(pfd[0], F_SETFL, flg | O_NONBLOCK);
            ssize_t r;
            while ((r = read(pfd[0], buf, sizeof(buf))) > 0)
                out_write(ofd, buf, (size_t)r);
            break;
        }
        if (timeout > 0 && time(NULL) >= deadline) {
            kill(-pid, SIGKILL); kill(pid, SIGKILL);
            to = 1;
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        }
    }
    close(pfd[0]);
    if (to) { *timed_out = 1; *exitcode = -1; }
    else if (WIFEXITED(status)) *exitcode = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) *exitcode = 128 + WTERMSIG(status);
    return 0;
}

static void run_job(const char *path, const char *name) {
    char *script = NULL; char err[256];
    struct stat st; uid_t wuid = 0;
    if (stat(path, &st) == 0) wuid = st.st_uid;

    if (read_script(path, &script, err, sizeof(err)) != 0) {
        char rej[600];
        snprintf(rej, sizeof rej, OUT_DIR "/rejected_%s", name);
        rename(path, rej);
        int ofd = open(RESULT_TMP, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (ofd >= 0) {
            char tbuf[32]; human_time(tbuf, sizeof tbuf);
            char hdr[1024];
            int hl = snprintf(hdr, sizeof hdr,
                "=== %s (%ld) | job: %s ===\nERROR: %s\n(job moved to %s)\n=== exit -1 ===\n",
                tbuf, (long)time(NULL), name, err, rej);
            if (hl > 0) { ssize_t ig = write(ofd, hdr, (size_t)hl); (void)ig; }
            close(ofd);
            rename(RESULT_TMP, RESULT);
        }
        audit(name, wuid, -1, 0, 0, err);
        return;
    }

    directives d;
    parse_directives(script, &d);
    double t0 = now_ms();
    g_out_bytes = 0; g_out_cap = MAX_OUTPUT; g_truncated = 0;

    int ofd = open(RESULT_TMP, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (ofd < 0) { free(script); return; }
    {
        char tbuf[32]; human_time(tbuf, sizeof tbuf);
        char hdr[512];
        int hl = snprintf(hdr, sizeof hdr, "=== %s (%ld) | job: %s ===\n",
                          tbuf, (long)time(NULL), name);
        if (hl > 0) { ssize_t ig = write(ofd, hdr, (size_t)hl); (void)ig; }
    }

    size_t plen = strlen(ENVPREFIX);
    char *full = malloc(plen + strlen(d.body) + 1);
    if (!full) { free(script); close(ofd); return; }
    memcpy(full, ENVPREFIX, plen);
    strcpy(full + plen, d.body);

    int exitcode = -1, timed_out = 0;
    if (d.async) {
        char base[256]; snprintf(base, sizeof base, "%s", name);
        size_t bl = strlen(base);
        if (bl > 3 && strcmp(base + bl - 3, ".sh") == 0) base[bl - 3] = 0;
        char logpath[600];
        snprintf(logpath, sizeof logpath, OUT_DIR "/%s.log", base);
        if (run_async(full, logpath) == 0) {
            char msg[700];
            int ml = snprintf(msg, sizeof msg,
                "[ASYNC] started in background.\nlog: %s\n(no result here; poll the log file)\n",
                logpath);
            if (ml > 0) { ssize_t ig = write(ofd, msg, (size_t)ml); (void)ig; }
            exitcode = 0;
        }
    } else {
        run_sync(ofd, full, d.timeout, &exitcode, &timed_out);
    }
    free(full);

    double ms = now_ms() - t0;
    char foot[256];
    int flen = timed_out
        ? snprintf(foot, sizeof foot,
            "\n=== TIMEOUT after %ds (process group killed) ===\n", d.timeout)
        : snprintf(foot, sizeof foot, "\n=== exit %d | %.0fms ===\n", exitcode, ms);
    if (flen > 0) { ssize_t ig = write(ofd, foot, (size_t)flen); (void)ig; }
    close(ofd);
    rename(RESULT_TMP, RESULT);
    unlink(path);

    char fl[161]; first_line(d.body, fl, sizeof fl);
    audit(name, wuid, exitcode, ms, g_out_bytes, fl);
    free(script);
}

static int namecmp(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void process_queue(void) {
    DIR *dp = opendir(IN_DIR);
    if (!dp) return;
    char *names[256];
    int n = 0;
    struct dirent *de;
    while (n < 256 && (de = readdir(dp)) != NULL) {
        size_t l = strlen(de->d_name);
        if (de->d_name[0] == '.' || l <= 3 || l >= 200) continue;
        if (strcmp(de->d_name + l - 3, ".sh") != 0) continue;
        if (strcmp(de->d_name, "env.sh") == 0) continue;
        names[n] = strdup(de->d_name);
        if (names[n]) n++;
    }
    closedir(dp);
    qsort(names, (size_t)n, sizeof(char *), namecmp);
    for (int i = 0; i < n; i++) {
        char path[512];
        snprintf(path, sizeof path, IN_DIR "/%s", names[i]);
        run_job(path, names[i]);
        free(names[i]);
    }
}

int main(void) {
    ensure_dirs();
    int lockfd = open(LOCKFILE, O_RDWR | O_CREAT, 0600);
    if (lockfd >= 0 && flock(lockfd, LOCK_EX | LOCK_NB) != 0) {
        return 0;   /* 已有实例 */
    }
    FILE *pf = fopen(PIDFILE, "w");
    if (pf) { fprintf(pf, "%d\n", (int)getpid()); fclose(pf); }
    setup_env();
    {
        char tbuf[32]; human_time(tbuf, sizeof tbuf);
        hist_rotate_if_needed();
        FILE *h = fopen(HISTORY, "a");
        if (h) { fprintf(h, "%s | execd v2 started, pid=%d\n", tbuf, (int)getpid()); fclose(h); }
    }
    for (;;) {
        process_queue();
        usleep(POLL_MS * 1000);
    }
}
