#ifndef STATIC_FILES_H
#define STATIC_FILES_H

#include <stddef.h>
#include <sys/types.h>

/* Resolves request paths to files under a single document root, with the
 * sanitization needed to make "open whatever the client named" safe.
 *
 * The module deliberately stops at opening the file: it hands back a fd
 * plus the metadata needed to build headers, and lets the event loop own
 * the sending. That keeps the traversal rules testable without a socket,
 * and keeps sendfile()'s backpressure handling in the one place that
 * already understands EPOLLOUT. */

typedef enum {
    STATIC_FILE_OK,
    STATIC_FILE_NOT_FOUND,   /* caller answers 404 */
    STATIC_FILE_INVALID_PATH /* caller answers 400, see static_files.c for why the two differ */
} static_file_result_t;

typedef struct {
    int fd;                   /* < 0 unless the result was STATIC_FILE_OK */
    off_t size;               /* from fstat(), and what Content-Length will advertise */
    const char *content_type; /* static string, never owned by the caller */
} static_file_t;

/* Canonicalizes root and remembers it as the document root every later
 * lookup is confined to. Must be called once before static_file_open(),
 * and returns -1 (after printing why) if root doesn't resolve to a
 * readable directory, so a misconfigured deployment fails at startup
 * instead of turning into a 404 on every request. Calling it again just
 * replaces the root; tests rely on that to point at a scratch directory.
 */
int static_files_init(const char *root);

/* Resolves a request-target path (as parsed, still percent-encoded and
 * possibly carrying a query string) to an open file under the document
 * root. On STATIC_FILE_OK the caller owns out->fd and must eventually
 * pass it to static_file_close(); on any other result *out is left with
 * fd < 0 and nothing needs cleanup. */
static_file_result_t static_file_open(const char *path, size_t path_len, static_file_t *out);

/* Idempotent, and safe on a zeroed-but-never-opened static_file_t only
 * if its fd was set to -1 first, see the fd == 0 trap noted in
 * event_loop.c. */
void static_file_close(static_file_t *file);

#endif
