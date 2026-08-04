/* -std=c11 alone exposes only ISO C, which has no realpath(), readlink(),
 * O_CLOEXEC or strcasecmp(). Nothing here is a GNU extension, so this asks
 * for X/Open 7 (= POSIX.1-2008 plus the XSI option) instead of _GNU_SOURCE:
 * glibc gates realpath() on __USE_XOPEN_EXTENDED specifically, so a plain
 * _POSIX_C_SOURCE=200809L would leave it undeclared. */
#define _XOPEN_SOURCE 700

#include "static_files.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strcasecmp: filename extensions are matched case-insensitively */
#include <sys/stat.h>
#include <unistd.h>

/* Canonical document root, everything served must live under it. Filled
 * once by static_files_init() so that no request ever pays for a
 * realpath() of the root, and more importantly so the value a lookup is
 * compared against can't be changed by anything a client sends.
 *
 * "Once" is also what makes this module usable from the worker threads
 * of layer 6 without a lock: main() calls static_files_init() before the
 * first thread exists, and every lookup afterwards only reads. Nothing
 * else in here holds state across a call, every working buffer below is
 * a local, so concurrent static_file_open()s cannot interfere. */
static char g_root[PATH_MAX];
static size_t g_root_len;

/* Outcome of turning a request path into a filesystem path, before the
 * filesystem is touched at all. Kept separate from static_file_result_t
 * because "the client asked for something structurally illegal" and "we
 * looked and there is nothing there" are different answers, see
 * static_file_open(). */
typedef enum {
    RESOLVE_OK,
    RESOLVE_ILLEGAL,  /* traversal attempt or malformed encoding */
    RESOLVE_TOO_LONG, /* can't name anything under our root, so it's a miss, not an attack */
} resolve_result_t;

static int hex_digit(unsigned char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* Percent-decodes the path portion of a request-target (RFC 3986 section
 * 2.1) into out.
 *
 * Decoding has to happen BEFORE any ".." check, not after: a filter that
 * looks for ".." in the raw target is trivially bypassed with "%2e%2e",
 * which the kernel then sees as ".." anyway once open() gets the decoded
 * bytes. Every path-traversal CVE of this shape comes from checking the
 * wrong representation. Conversely, decoding "%2f" into a real '/' is
 * safe here precisely because the segment scan in join_under_root() runs
 * afterwards and treats it as an ordinary separator.
 *
 * A malformed escape or a decoded NUL is RESOLVE_ILLEGAL. The NUL is
 * rejected rather than truncated at because C string APIs would stop
 * there while any length-based check upstream would not, and that
 * disagreement is exactly what "file.php%00.txt" style attacks exploit. */
static resolve_result_t decode_path(const char *path, size_t path_len, char *out, size_t out_size, size_t *out_len) {
    size_t w = 0;

    for (size_t i = 0; i < path_len; i++) {
        /* The query string (and any fragment a broken client leaks into
         * the request line) names no file, it just selects within one. */
        if (path[i] == '?' || path[i] == '#') {
            break;
        }

        unsigned char c = (unsigned char)path[i];
        if (c == '%') {
            if (i + 2 >= path_len) {
                return RESOLVE_ILLEGAL; /* truncated escape */
            }
            int hi = hex_digit((unsigned char)path[i + 1]);
            int lo = hex_digit((unsigned char)path[i + 2]);
            if (hi < 0 || lo < 0) {
                return RESOLVE_ILLEGAL;
            }
            c = (unsigned char)((hi << 4) | lo);
            if (c == '\0') {
                return RESOLVE_ILLEGAL;
            }
            i += 2;
        }

        if (w + 1 >= out_size) {
            return RESOLVE_TOO_LONG;
        }
        out[w++] = (char)c;
    }

    out[w] = '\0';
    *out_len = w;
    return RESOLVE_OK;
}

/* Appends the decoded path onto the document root one segment at a time,
 * dropping "." and empty segments and rejecting "..".
 *
 * Rejecting ".." instead of resolving it (popping the previous segment)
 * is the deliberate choice. Resolving is strictly more permissive and
 * only correct if nothing along the path is a symlink, since "a/.." is
 * the parent of a's *target*, not of a's directory, so a lexical pop
 * silently disagrees with what the kernel will do. Rejecting needs no
 * such assumption: a path with no ".." segment cannot climb, period.
 * Legitimate clients don't send ".." in a request-target anyway, browsers
 * normalize it away before the request is ever made. */
static resolve_result_t join_under_root(const char *decoded, size_t decoded_len, char *out, size_t out_size) {
    if (decoded_len == 0 || decoded[0] != '/') {
        /* origin-form request-targets are always absolute (RFC 7230
         * section 5.3.1); anything else is not addressing our root. */
        return RESOLVE_ILLEGAL;
    }

    /* realpath() leaves a trailing '/' only for the filesystem root, and
     * every segment below appends its own separator, so starting from an
     * empty prefix in that one case avoids building "//index.html". */
    size_t out_len = (g_root_len == 1 && g_root[0] == '/') ? 0 : g_root_len;
    if (out_len >= out_size) {
        return RESOLVE_TOO_LONG;
    }
    memcpy(out, g_root, out_len);

    size_t i = 0;
    while (i < decoded_len) {
        while (i < decoded_len && decoded[i] == '/') {
            i++; /* collapse "//" and skip the leading separator */
        }
        size_t start = i;
        while (i < decoded_len && decoded[i] != '/') {
            i++;
        }
        size_t seg_len = i - start;

        if (seg_len == 0) {
            break; /* trailing slash, nothing more to append */
        }
        if (seg_len == 1 && decoded[start] == '.') {
            continue;
        }
        if (seg_len == 2 && decoded[start] == '.' && decoded[start + 1] == '.') {
            return RESOLVE_ILLEGAL;
        }
        if (out_len + 1 + seg_len >= out_size) {
            return RESOLVE_TOO_LONG;
        }
        out[out_len++] = '/';
        memcpy(out + out_len, decoded + start, seg_len);
        out_len += seg_len;
    }

    if (out_len == 0) {
        out[out_len++] = '/'; /* root is "/" and the request was "/" */
    }
    out[out_len] = '\0';
    return RESOLVE_OK;
}

/* Prefix test with an explicit boundary check: a plain strncmp() would
 * accept "/srv/www-backup" as living under "/srv/www". */
static bool is_within_root(const char *canonical) {
    if (strncmp(canonical, g_root, g_root_len) != 0) {
        return false;
    }
    if (g_root_len == 1 && g_root[0] == '/') {
        return canonical[1] != '\0';
    }
    return canonical[g_root_len] == '/';
}

/* Confirms that the file we actually have open sits under the document
 * root, catching the one escape join_under_root() cannot: a symlink
 * inside public/ pointing outside it.
 *
 * The check runs on /proc/self/fd/N rather than on the path string via
 * realpath(), because readlink() there names the very inode behind fd.
 * Canonicalizing the string instead leaves a TOCTOU window, an attacker
 * who can write to the root could swap a directory for a symlink between
 * the realpath() and the open() and get a file that was never validated.
 * Here there is nothing left to swap: the fd is already pinned.
 *
 * Note this permits symlinks that stay inside the root, which is what
 * makes it better than a blanket O_NOFOLLOW. */
static bool fd_is_within_root(int fd) {
    char link_path[64];
    snprintf(link_path, sizeof(link_path), "/proc/self/fd/%d", fd);

    char canonical[PATH_MAX];
    ssize_t n = readlink(link_path, canonical, sizeof(canonical) - 1);
    if (n < 0) {
        /* /proc not mounted, or the fd vanished. Fail closed: an
         * unverifiable path is not a path we serve. */
        perror("readlink(/proc/self/fd)");
        return false;
    }
    if ((size_t)n >= sizeof(canonical) - 1) {
        return false; /* readlink() truncates silently, a full buffer means we can't trust the result */
    }
    canonical[n] = '\0';
    return is_within_root(canonical);
}

/* Guessing beyond the extension is out of scope on purpose: sniffing
 * content is how a text/plain upload ends up executed as HTML. Unknown
 * extensions get application/octet-stream so the browser downloads
 * rather than interprets. */
static const char *content_type_for(const char *fs_path) {
    static const struct {
        const char *ext;
        const char *type;
    } table[] = {
        {".html", "text/html; charset=utf-8"},  {".htm", "text/html; charset=utf-8"},
        {".txt", "text/plain; charset=utf-8"},  {".css", "text/css; charset=utf-8"},
        {".js", "text/javascript; charset=utf-8"}, {".json", "application/json"},
        {".svg", "image/svg+xml"},              {".png", "image/png"},
        {".jpg", "image/jpeg"},                 {".jpeg", "image/jpeg"},
        {".gif", "image/gif"},                  {".ico", "image/vnd.microsoft.icon"},
    };

    const char *slash = strrchr(fs_path, '/');
    const char *name = (slash != NULL) ? slash + 1 : fs_path;
    const char *dot = strrchr(name, '.');
    if (dot != NULL) {
        for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
            if (strcasecmp(dot, table[i].ext) == 0) {
                return table[i].type;
            }
        }
    }
    return "application/octet-stream";
}

int static_files_init(const char *root) {
    char resolved[PATH_MAX];
    if (realpath(root, resolved) == NULL) {
        fprintf(stderr, "static root '%s': %s\n", root, strerror(errno));
        return -1;
    }

    struct stat st;
    if (stat(resolved, &st) < 0) {
        perror("stat(static root)");
        return -1;
    }
    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "static root '%s' is not a directory\n", resolved);
        return -1;
    }

    size_t len = strlen(resolved);
    if (len >= sizeof(g_root)) {
        fprintf(stderr, "static root '%s' is too long\n", resolved);
        return -1;
    }
    memcpy(g_root, resolved, len + 1);
    g_root_len = len;
    return 0;
}

static_file_result_t static_file_open(const char *path, size_t path_len, static_file_t *out) {
    out->fd = -1;
    out->size = 0;
    out->content_type = NULL;

    if (g_root_len == 0) {
        fprintf(stderr, "static_file_open() called before static_files_init()\n");
        return STATIC_FILE_NOT_FOUND;
    }

    char decoded[PATH_MAX];
    size_t decoded_len = 0;
    resolve_result_t decoded_result = decode_path(path, path_len, decoded, sizeof(decoded), &decoded_len);
    if (decoded_result == RESOLVE_ILLEGAL) {
        return STATIC_FILE_INVALID_PATH;
    }
    if (decoded_result == RESOLVE_TOO_LONG) {
        return STATIC_FILE_NOT_FOUND;
    }

    char fs_path[PATH_MAX];
    switch (join_under_root(decoded, decoded_len, fs_path, sizeof(fs_path))) {
    case RESOLVE_ILLEGAL:
        /* 400, not 404, and the difference is deliberate. A ".." segment
         * is a defect in the request itself: no legitimate client emits
         * one, and we never touched the filesystem, so answering "not
         * found" would be inventing a fact about a resource we never
         * looked for. 400 also refuses to play along with the probe
         * instead of implying the attacker's path was merely a miss.
         * Note the opposite call is made below for a symlink that
         * escapes the root, where the request was well formed and 404 is
         * what keeps us from confirming that something exists out
         * there. */
        return STATIC_FILE_INVALID_PATH;
    case RESOLVE_TOO_LONG:
        return STATIC_FILE_NOT_FOUND;
    case RESOLVE_OK:
        break;
    }

    /* A trailing slash names a directory, and this server has no
     * directory listings and no implicit index file. Stopping here keeps
     * "/hello.txt/" from quietly serving "/hello.txt", which would give
     * one resource two URLs (and two cache entries, and two of whatever
     * an access-control layer above would key on). Checked after the
     * join so that "/../" is still reported as the traversal it is. */
    if (decoded[decoded_len - 1] == '/') {
        return STATIC_FILE_NOT_FOUND;
    }

    /* O_NONBLOCK matters even though it does nothing for regular files:
     * without it, opening a FIFO that happens to sit in the document root
     * blocks until a writer shows up, and an event loop parked in open()
     * stops serving every connection it owns. A pool of them softens that
     * (the other workers keep going) without fixing it: one FIFO per
     * worker still takes the whole server down, and the loop that blocked
     * was the only one that could answer its own clients. The fstat()
     * below then rejects it anyway. */
    int fd = open(fs_path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        if (errno != ENOENT && errno != ENOTDIR && errno != EACCES && errno != ELOOP && errno != ENAMETOOLONG) {
            perror("open(static file)");
        }
        /* Every failure collapses into 404, including EACCES. Letting a
         * client tell "exists but you may not read it" apart from "isn't
         * there" hands over a free map of the filesystem, one request at
         * a time. */
        return STATIC_FILE_NOT_FOUND;
    }

    if (!fd_is_within_root(fd)) {
        close(fd);
        return STATIC_FILE_NOT_FOUND;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        perror("fstat(static file)");
        close(fd);
        return STATIC_FILE_NOT_FOUND;
    }
    if (!S_ISREG(st.st_mode)) {
        /* Directories (we serve no listings and no implicit index),
         * devices, FIFOs, sockets. Also a hard requirement of sendfile(2):
         * in_fd must support mmap-like reads, which only regular files
         * and a few special cases do. */
        close(fd);
        return STATIC_FILE_NOT_FOUND;
    }

    out->fd = fd;
    out->size = st.st_size;
    out->content_type = content_type_for(fs_path);
    return STATIC_FILE_OK;
}

void static_file_close(static_file_t *file) {
    if (file->fd >= 0) {
        close(file->fd);
        file->fd = -1;
    }
}
