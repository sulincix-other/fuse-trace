/*
 * fuse-trace - a passthrough FUSE filesystem that logs every operation.
 *
 * All requests are forwarded to a backing directory given with --src=<dir>.
 * Every callback is written to stderr so the filesystem can be used to see
 * exactly which operations an application performs.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#define FUSE_USE_VERSION 31

#include <fuse.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>

#define TRACE_VERSION "1.0.0"

struct trace_config {
    char *source_dir;
    int verbose;
    int allow_other;
    int color;
};

static struct trace_config trace_cfg;

enum {
    KEY_HELP,
    KEY_VERSION,
};

#define TRACE_OPT(t, p, v) { t, offsetof(struct trace_config, p), v }

static struct fuse_opt trace_opts[] = {
    TRACE_OPT("--src=%s", source_dir, 0),
    TRACE_OPT("--verbose", verbose, 1),
    TRACE_OPT("-v", verbose, 1),
    TRACE_OPT("--allow-other", allow_other, 1),
    TRACE_OPT("--color", color, 1),
    TRACE_OPT("--no-color", color, 0),
    FUSE_OPT_KEY("-h", KEY_HELP),
    FUSE_OPT_KEY("--help", KEY_HELP),
    FUSE_OPT_KEY("-V", KEY_VERSION),
    FUSE_OPT_KEY("--version", KEY_VERSION),
    FUSE_OPT_END
};

/* --- Pretty colorized logging -------------------------------------------- */

#define CLR_RESET   "\033[0m"
#define CLR_GRAY    "\033[90m"
#define CLR_RED     "\033[31m"
#define CLR_GREEN   "\033[32m"
#define CLR_YELLOW  "\033[33m"
#define CLR_BLUE    "\033[34m"
#define CLR_MAGENTA "\033[35m"
#define CLR_CYAN    "\033[36m"
#define CLR_WHITE   "\033[37m"

struct op_color {
    const char *name;
    const char *color;
};

static const struct op_color op_colors[] = {
    { "getattr",     CLR_BLUE },
    { "readdir",     CLR_BLUE },
    { "readlink",    CLR_BLUE },
    { "access",      CLR_BLUE },
    { "statfs",      CLR_BLUE },
    { "opendir",     CLR_BLUE },
    { "open",        CLR_GREEN },
    { "read",        CLR_GREEN },
    { "lseek",       CLR_GREEN },
    { "write",       CLR_YELLOW },
    { "create",      CLR_YELLOW },
    { "mkdir",       CLR_YELLOW },
    { "mknod",       CLR_YELLOW },
    { "symlink",     CLR_YELLOW },
    { "link",        CLR_YELLOW },
    { "fallocate",   CLR_YELLOW },
    { "unlink",      CLR_RED },
    { "rmdir",       CLR_RED },
    { "rename",      CLR_RED },
    { "chmod",       CLR_MAGENTA },
    { "chown",       CLR_MAGENTA },
    { "truncate",    CLR_MAGENTA },
    { "utimens",     CLR_MAGENTA },
    { "setxattr",    CLR_CYAN },
    { "getxattr",    CLR_CYAN },
    { "listxattr",   CLR_CYAN },
    { "removexattr", CLR_CYAN },
    { "fsync",       CLR_GRAY },
    { "flush",       CLR_GRAY },
    { "release",     CLR_GRAY },
    { "releasedir",  CLR_GRAY },
    { NULL,         NULL }
};

static const char *op_color(const char *op)
{
    for (int i = 0; op_colors[i].name; i++)
        if (strcmp(op, op_colors[i].name) == 0)
            return op_colors[i].color;
    return CLR_WHITE;
}

/* Log an operation with timestamp, PID and colorized output. */
static void trace_log(const char *fmt, ...)
{
    va_list ap;
    char buf[4096];
    char op[32];
    const char *color;
    struct timeval tv;
    struct tm tm;
    int ms, i, n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (n < 0)
        return;
    if ((size_t)n >= sizeof(buf))
        n = (int)sizeof(buf) - 1;
    buf[n] = '\0';

    /* Extract the operation name (first word). */
    for (i = 0; i < (int)sizeof(op) - 1 && buf[i] && buf[i] != ' '; i++)
        op[i] = buf[i];
    op[i] = '\0';

    color = op_color(op);

    gettimeofday(&tv, NULL);
    localtime_r(&tv.tv_sec, &tm);
    ms = (int)(tv.tv_usec / 1000);

    if (trace_cfg.color && isatty(STDERR_FILENO)) {
        fprintf(stderr,
                "%s%02d:%02d:%02d.%03d%s %s[%d]%s %s%s%s\n",
                CLR_GRAY, tm.tm_hour, tm.tm_min, tm.tm_sec, ms, CLR_RESET,
                CLR_CYAN, (int) getpid(), CLR_RESET,
                color, buf, CLR_RESET);
    } else {
        fprintf(stderr,
                "%02d:%02d:%02d.%03d [%d] %s\n",
                tm.tm_hour, tm.tm_min, tm.tm_sec, ms,
                (int) getpid(), buf);
    }
    fflush(stderr);
}

static void show_help(const char *progname)
{
    printf("Usage: %s [options] --src=<source_dir> <mountpoint>\n\n", progname);
    printf("Options:\n");
    printf("    --src=<source_dir>  backing directory to expose (required)\n");
    printf("    -v, --verbose       also log read/write byte counts\n");
    printf("    --allow-other      allow other users to access the mount\n");
    printf("    --color            colorized output (auto-detected by default)\n");
    printf("    --no-color         disable colorized output\n");
    printf("    -h, --help          show this help and exit\n");
    printf("    -V, --version       show version and exit\n");
    printf("\nFUSE options:\n");
    printf("    -f                  run in the foreground\n");
    printf("    -d                  enable FUSE debug output\n");
    printf("    -s                  run single threaded\n");
}

/* Map a FUSE path (always absolute) onto the backing directory. */
static void get_source_path(char fpath[PATH_MAX], const char *path)
{
    snprintf(fpath, PATH_MAX, "%s%s", trace_cfg.source_dir, path);
}

/* 1. getattr -------------------------------------------------------------- */
static int trace_getattr(const char *path, struct stat *stbuf,
                          struct fuse_file_info *fi)
{
    char fpath[PATH_MAX];
    int res;

    get_source_path(fpath, path);

    if (fi != NULL)
        res = fstat(fi->fh, stbuf);
    else
        res = lstat(fpath, stbuf);

    if (res == -1)
        return -errno;

    trace_log("getattr %s", path);
    return 0;
}

/* 2. readlink ------------------------------------------------------------- */
static int trace_readlink(const char *path, char *buf, size_t size)
{
    char fpath[PATH_MAX];
    ssize_t res;

    get_source_path(fpath, path);

    res = readlink(fpath, buf, size - 1);
    if (res == -1)
        return -errno;

    /* FUSE expects a NUL terminated string. */
    buf[res] = '\0';

    trace_log("readlink %s -> %s", path, buf);
    return 0;
}

/* 3. mknod ---------------------------------------------------------------- */
static int trace_mknod(const char *path, mode_t mode, dev_t rdev)
{
    char fpath[PATH_MAX];
    int res;

    get_source_path(fpath, path);

    /* On Linux the regular file, FIFO and device special cases are split. */
    if (S_ISREG(mode)) {
        res = open(fpath, O_CREAT | O_EXCL | O_WRONLY, mode);
        if (res >= 0)
            res = close(res);
    } else if (S_ISFIFO(mode)) {
        res = mkfifo(fpath, mode);
    } else {
        res = mknod(fpath, mode, rdev);
    }

    if (res == -1)
        return -errno;

    trace_log("mknod %s", path);
    return 0;
}

/* 4. mkdir ---------------------------------------------------------------- */
static int trace_mkdir(const char *path, mode_t mode)
{
    char fpath[PATH_MAX];

    get_source_path(fpath, path);

    if (mkdir(fpath, mode) == -1)
        return -errno;

    trace_log("mkdir %s", path);
    return 0;
}

/* 5. unlink --------------------------------------------------------------- */
static int trace_unlink(const char *path)
{
    char fpath[PATH_MAX];

    get_source_path(fpath, path);

    if (unlink(fpath) == -1)
        return -errno;

    trace_log("unlink %s", path);
    return 0;
}

/* 6. rmdir ---------------------------------------------------------------- */
static int trace_rmdir(const char *path)
{
    char fpath[PATH_MAX];

    get_source_path(fpath, path);

    if (rmdir(fpath) == -1)
        return -errno;

    trace_log("rmdir %s", path);
    return 0;
}

/* 7. symlink -------------------------------------------------------------- */
static int trace_symlink(const char *target, const char *linkpath)
{
    char fpath[PATH_MAX];

    get_source_path(fpath, linkpath);

    if (symlink(target, fpath) == -1)
        return -errno;

    trace_log("symlink %s -> %s", linkpath, target);
    return 0;
}

/* 8. rename --------------------------------------------------------------- */
static int trace_rename(const char *from, const char *to, unsigned int flags)
{
    char ffrom[PATH_MAX];
    char fto[PATH_MAX];

    get_source_path(ffrom, from);
    get_source_path(fto, to);

    if (flags) {
        if (renameat2(AT_FDCWD, ffrom, AT_FDCWD, fto, flags) == -1)
            return -errno;
    } else {
        if (rename(ffrom, fto) == -1)
            return -errno;
    }

    trace_log("rename %s -> %s", from, to);
    return 0;
}

/* 9. link ----------------------------------------------------------------- */
static int trace_link(const char *from, const char *to)
{
    char ffrom[PATH_MAX];
    char fto[PATH_MAX];

    get_source_path(ffrom, from);
    get_source_path(fto, to);

    if (link(ffrom, fto) == -1)
        return -errno;

    trace_log("link %s -> %s", from, to);
    return 0;
}

/* 10. chmod --------------------------------------------------------------- */
static int trace_chmod(const char *path, mode_t mode,
                       struct fuse_file_info *fi)
{
    char fpath[PATH_MAX];
    int res;

    get_source_path(fpath, path);

    if (fi != NULL)
        res = fchmod(fi->fh, mode);
    else
        res = chmod(fpath, mode);

    if (res == -1)
        return -errno;

    trace_log("chmod %s 0%03o", path, mode);
    return 0;
}

/* 11. chown --------------------------------------------------------------- */
static int trace_chown(const char *path, uid_t uid, gid_t gid,
                       struct fuse_file_info *fi)
{
    char fpath[PATH_MAX];
    int res;

    get_source_path(fpath, path);

    if (fi != NULL)
        res = fchown(fi->fh, uid, gid);
    else
        res = lchown(fpath, uid, gid);

    if (res == -1)
        return -errno;

    trace_log("chown %s %u:%u", path, (unsigned) uid, (unsigned) gid);
    return 0;
}

/* 12. truncate ------------------------------------------------------------ */
static int trace_truncate(const char *path, off_t size,
                          struct fuse_file_info *fi)
{
    char fpath[PATH_MAX];
    int res;

    get_source_path(fpath, path);

    if (fi != NULL)
        res = ftruncate(fi->fh, size);
    else
        res = truncate(fpath, size);

    if (res == -1)
        return -errno;

    trace_log("truncate %s %lld", path, (long long) size);
    return 0;
}

/* 13. open ---------------------------------------------------------------- */
static int trace_open(const char *path, struct fuse_file_info *fi)
{
    char fpath[PATH_MAX];
    int fd;

    get_source_path(fpath, path);

    fd = open(fpath, fi->flags);
    if (fd == -1)
        return -errno;

    fi->fh = (uint64_t) fd;

    trace_log("open %s (flags=0x%x, fd=%d)", path, fi->flags, fd);
    return 0;
}

/* 14. read ---------------------------------------------------------------- */
static int trace_read(const char *path, char *buf, size_t size, off_t offset,
                      struct fuse_file_info *fi)
{
    ssize_t res;

    res = pread(fi->fh, buf, size, offset);
    if (res == -1)
        return -errno;

    if (trace_cfg.verbose)
        trace_log("read %s %zu bytes @%lld", path, (size_t) res,
                  (long long) offset);

    return (int) res;
}

/* 15. write --------------------------------------------------------------- */
static int trace_write(const char *path, const char *buf, size_t size,
                       off_t offset, struct fuse_file_info *fi)
{
    ssize_t res;

    res = pwrite(fi->fh, buf, size, offset);
    if (res == -1)
        return -errno;

    if (trace_cfg.verbose)
        trace_log("write %s %zu bytes @%lld", path, (size_t) res,
                  (long long) offset);

    return (int) res;
}

/* 16. flush --------------------------------------------------------------- */
static int trace_flush(const char *path, struct fuse_file_info *fi)
{
    int fd;

    (void) path;

    /* dup() and close() so the flush does not disturb the original fd. */
    fd = dup(fi->fh);
    if (fd == -1) {
        if (errno == EBADF)
            return 0;
        return -errno;
    }

    if (close(fd) == -1)
        return -errno;

    return 0;
}

/* 17. release ------------------------------------------------------------- */
static int trace_release(const char *path, struct fuse_file_info *fi)
{
    (void) path;

    close((int) fi->fh);
    return 0;
}

/* 18. fsync --------------------------------------------------------------- */
static int trace_fsync(const char *path, int datasync,
                       struct fuse_file_info *fi)
{
    int res;

    (void) path;

    if (datasync)
        res = fdatasync(fi->fh);
    else
        res = fsync(fi->fh);

    if (res == -1)
        return -errno;

    return 0;
}

/* 19. xattr --------------------------------------------------------------- */
static int trace_setxattr(const char *path, const char *name, const char *value,
                          size_t size, int flags)
{
    char fpath[PATH_MAX];

    get_source_path(fpath, path);

    if (lsetxattr(fpath, name, value, size, flags) == -1)
        return -errno;

    trace_log("setxattr %s %s", path, name);
    return 0;
}

static int trace_getxattr(const char *path, const char *name, char *value,
                          size_t size)
{
    char fpath[PATH_MAX];
    ssize_t res;

    get_source_path(fpath, path);

    res = lgetxattr(fpath, name, value, size);
    if (res == -1)
        return -errno;

    return (int) res;
}

static int trace_listxattr(const char *path, char *list, size_t size)
{
    char fpath[PATH_MAX];
    ssize_t res;

    get_source_path(fpath, path);

    res = llistxattr(fpath, list, size);
    if (res == -1)
        return -errno;

    return (int) res;
}

static int trace_removexattr(const char *path, const char *name)
{
    char fpath[PATH_MAX];

    get_source_path(fpath, path);

    if (lremovexattr(fpath, name) == -1)
        return -errno;

    trace_log("removexattr %s %s", path, name);
    return 0;
}

/* 20. opendir / readdir / releasedir -------------------------------------- */
static int trace_opendir(const char *path, struct fuse_file_info *fi)
{
    char fpath[PATH_MAX];
    DIR *dp;

    get_source_path(fpath, path);

    dp = opendir(fpath);
    if (dp == NULL)
        return -errno;

    fi->fh = (uint64_t) (uintptr_t) dp;

    trace_log("opendir %s", path);
    return 0;
}

static int trace_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                         off_t offset, struct fuse_file_info *fi,
                         enum fuse_readdir_flags flags)
{
    DIR *dp = (DIR *) (uintptr_t) fi->fh;
    struct dirent *de;

    (void) path;
    (void) offset;
    (void) flags;

    if (dp == NULL)
        return -EBADF;

    while ((de = readdir(dp)) != NULL) {
        struct stat st;

        memset(&st, 0, sizeof(st));
        st.st_ino = de->d_ino;
        st.st_mode = de->d_type << 12;

        if (filler(buf, de->d_name, &st, 0, 0))
            break;
    }

    return 0;
}

static int trace_releasedir(const char *path, struct fuse_file_info *fi)
{
    (void) path;

    if (fi->fh != 0) {
        closedir((DIR *) (uintptr_t) fi->fh);
        fi->fh = 0;
    }

    return 0;
}

/* 21. create -------------------------------------------------------------- */
static int trace_create(const char *path, mode_t mode,
                        struct fuse_file_info *fi)
{
    char fpath[PATH_MAX];
    int fd;

    get_source_path(fpath, path);

    fd = open(fpath, fi->flags, mode);
    if (fd == -1)
        return -errno;

    fi->fh = (uint64_t) fd;

    trace_log("create %s (mode=0%03o)", path, mode);
    return 0;
}

/* 22. utimens ------------------------------------------------------------- */
static int trace_utimens(const char *path, const struct timespec tv[2],
                         struct fuse_file_info *fi)
{
    char fpath[PATH_MAX];
    int res;

    get_source_path(fpath, path);

    if (fi != NULL)
        res = futimens(fi->fh, tv);
    else
        res = utimensat(AT_FDCWD, fpath, tv, AT_SYMLINK_NOFOLLOW);

    if (res == -1)
        return -errno;

    trace_log("utimens %s", path);
    return 0;
}

/* 23. statfs -------------------------------------------------------------- */
static int trace_statfs(const char *path, struct statvfs *stbuf)
{
    char fpath[PATH_MAX];

    get_source_path(fpath, path);

    if (statvfs(fpath, stbuf) == -1)
        return -errno;

    return 0;
}

/* 24. access -------------------------------------------------------------- */
static int trace_access(const char *path, int mask)
{
    char fpath[PATH_MAX];

    get_source_path(fpath, path);

    if (access(fpath, mask) == -1)
        return -errno;

    return 0;
}

/* 25. fallocate ----------------------------------------------------------- */
static int trace_fallocate(const char *path, int mode, off_t offset,
                           off_t length, struct fuse_file_info *fi)
{
    (void) path;

    if (fallocate(fi->fh, mode, offset, length) == -1)
        return -errno;

    return 0;
}

/* 26. lseek --------------------------------------------------------------- */
static off_t trace_lseek(const char *path, off_t off, int whence,
                         struct fuse_file_info *fi)
{
    off_t res;

    (void) path;

    res = lseek(fi->fh, off, whence);
    if (res == -1)
        return -errno;

    return res;
}

/* 27. copy_file_range ----------------------------------------------------- */
static ssize_t trace_copy_file_range(const char *path_in,
                                     struct fuse_file_info *fi_in,
                                     off_t offset_in, const char *path_out,
                                     struct fuse_file_info *fi_out,
                                     off_t offset_out, size_t size, int flags)
{
    ssize_t res;

    (void) path_in;
    (void) path_out;

    res = copy_file_range(fi_in->fh, &offset_in, fi_out->fh, &offset_out, size,
                          flags);
    if (res == -1)
        return -errno;

    return res;
}

static const struct fuse_operations trace_oper = {
    .getattr         = trace_getattr,
    .readlink        = trace_readlink,
    .mknod           = trace_mknod,
    .mkdir           = trace_mkdir,
    .unlink          = trace_unlink,
    .rmdir           = trace_rmdir,
    .symlink         = trace_symlink,
    .rename          = trace_rename,
    .link            = trace_link,
    .chmod           = trace_chmod,
    .chown           = trace_chown,
    .truncate        = trace_truncate,
    .open            = trace_open,
    .read            = trace_read,
    .write           = trace_write,
    .flush           = trace_flush,
    .release         = trace_release,
    .fsync           = trace_fsync,
    .setxattr        = trace_setxattr,
    .getxattr        = trace_getxattr,
    .listxattr       = trace_listxattr,
    .removexattr     = trace_removexattr,
    .opendir         = trace_opendir,
    .readdir         = trace_readdir,
    .releasedir      = trace_releasedir,
    .create          = trace_create,
    .utimens         = trace_utimens,
    .statfs          = trace_statfs,
    .access          = trace_access,
    .fallocate       = trace_fallocate,
    .lseek           = trace_lseek,
    .copy_file_range = trace_copy_file_range,
};

static int option_proc(void *data, const char *arg, int key,
                       struct fuse_args *outargs)
{
    (void) data;
    (void) arg;
    (void) outargs;

    switch (key) {
    case KEY_HELP:
        show_help(outargs->argv[0]);
        exit(0);
    case KEY_VERSION:
        printf("fuse-trace %s\n", TRACE_VERSION);
        exit(0);
    }

    return 1;
}

int main(int argc, char *argv[])
{
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
    size_t len;

    memset(&trace_cfg, 0, sizeof(trace_cfg));
    trace_cfg.color = 1;
    trace_cfg.color = 1;

    if (fuse_opt_parse(&args, &trace_cfg, trace_opts, option_proc) == -1)
        return 1;

    if (trace_cfg.source_dir == NULL) {
        fprintf(stderr, "Error: --src=<source_dir> is required.\n");
        show_help(argv[0]);
        return 1;
    }

    /* Drop a trailing slash unless the source is the filesystem root. */
    len = strlen(trace_cfg.source_dir);
    while (len > 1 && trace_cfg.source_dir[len - 1] == '/') {
        trace_cfg.source_dir[len - 1] = '\0';
        len--;
    }

    /* Let users other than the mounter access the mount. Requires either
     * root privileges or "user_allow_other" in /etc/fuse.conf. */
    if (trace_cfg.allow_other) {
        fuse_opt_add_arg(&args, "-o");
        fuse_opt_add_arg(&args, "allow_other");
    }

    return fuse_main(args.argc, args.argv, &trace_oper, &trace_cfg);
}
