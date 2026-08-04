/* Unit tests for the static-file resolver, mostly for the path
 * sanitization: traversal handling is the kind of thing that must be
 * pinned down by tests that can enumerate hostile inputs cheaply,
 * without a socket or a server process in the way.
 *
 * Two roots are used. The repository's own public/ checks the files the
 * server actually ships (so a renamed file or a lost extension mapping
 * shows up here), and a scratch directory built at runtime covers what
 * can't be committed to git: symlinks that escape the root, FIFOs, and
 * a sibling directory whose name shares a prefix with the root.
 *
 * Same house style as tests/test_http_parser.c: no framework, just
 * CHECK. */

#define _XOPEN_SOURCE 700

#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "static_files.h"

static int g_tests_run = 0;
static int g_tests_failed = 0;

#define CHECK(cond)                                                                                                  \
    do {                                                                                                             \
        g_tests_run++;                                                                                               \
        if (!(cond)) {                                                                                               \
            g_tests_failed++;                                                                                        \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                        \
        }                                                                                                            \
    } while (0)

#define RUN(test_fn)                                                                                                 \
    do {                                                                                                             \
        printf("%s\n", #test_fn);                                                                                    \
        test_fn();                                                                                                   \
    } while (0)

/* Resolves path and immediately closes whatever came back, for the many
 * tests that only care about the verdict. */
static static_file_result_t resolve(const char *path) {
    static_file_t file;
    static_file_result_t result = static_file_open(path, strlen(path), &file);
    static_file_close(&file);
    return result;
}

/* --- tests against the repository's public/ ------------------------- */

static void test_existing_files_are_served_with_a_content_type(void) {
    static_file_t file;

    CHECK(static_file_open("/hello.txt", strlen("/hello.txt"), &file) == STATIC_FILE_OK);
    CHECK(file.fd >= 0);
    CHECK(file.size > 0);
    CHECK(file.content_type != NULL && strcmp(file.content_type, "text/plain; charset=utf-8") == 0);
    static_file_close(&file);
    CHECK(file.fd == -1); /* close must clear the fd, callers use it as the "is there a body" flag */

    CHECK(static_file_open("/index.html", strlen("/index.html"), &file) == STATIC_FILE_OK);
    CHECK(strcmp(file.content_type, "text/html; charset=utf-8") == 0);
    static_file_close(&file);

    /* Nested path, and a type that is neither of the two obvious ones. */
    CHECK(static_file_open("/assets/style.css", strlen("/assets/style.css"), &file) == STATIC_FILE_OK);
    CHECK(strcmp(file.content_type, "text/css; charset=utf-8") == 0);
    static_file_close(&file);
}

static void test_missing_file_is_not_found(void) {
    CHECK(resolve("/does-not-exist.txt") == STATIC_FILE_NOT_FOUND);
    CHECK(resolve("/assets/nope.css") == STATIC_FILE_NOT_FOUND);
    /* A missing intermediate directory makes open() fail with ENOTDIR
     * rather than ENOENT, same answer either way. */
    CHECK(resolve("/hello.txt/deeper") == STATIC_FILE_NOT_FOUND);
}

static void test_directories_are_not_served(void) {
    CHECK(resolve("/assets") == STATIC_FILE_NOT_FOUND);
    CHECK(resolve("/assets/") == STATIC_FILE_NOT_FOUND);
    CHECK(resolve("/") == STATIC_FILE_NOT_FOUND);
    /* A trailing slash names a directory, so it must not quietly serve
     * the file of the same name: one resource, one URL. */
    CHECK(resolve("/hello.txt/") == STATIC_FILE_NOT_FOUND);
}

static void test_query_string_and_fragment_are_ignored(void) {
    CHECK(resolve("/hello.txt?v=2") == STATIC_FILE_OK);
    CHECK(resolve("/hello.txt?") == STATIC_FILE_OK);
    CHECK(resolve("/hello.txt#top") == STATIC_FILE_OK);
}

static void test_redundant_path_syntax_is_normalized(void) {
    CHECK(resolve("/./hello.txt") == STATIC_FILE_OK);
    CHECK(resolve("//hello.txt") == STATIC_FILE_OK);
    CHECK(resolve("/assets/./style.css") == STATIC_FILE_OK);
    CHECK(resolve("//assets//style.css") == STATIC_FILE_OK);
}

static void test_percent_encoding_is_decoded(void) {
    CHECK(resolve("/hello%2Etxt") == STATIC_FILE_OK);  /* '.' */
    CHECK(resolve("/assets%2fstyle.css") == STATIC_FILE_OK); /* an encoded separator is still a separator */
}

/* The core of layer 5's security story: none of these may reach a file. */
static void test_path_traversal_is_rejected(void) {
    CHECK(resolve("/../Makefile") == STATIC_FILE_INVALID_PATH);
    CHECK(resolve("/../../etc/passwd") == STATIC_FILE_INVALID_PATH);
    CHECK(resolve("/assets/../hello.txt") == STATIC_FILE_INVALID_PATH);
    CHECK(resolve("/..") == STATIC_FILE_INVALID_PATH);
    CHECK(resolve("/a/b/../../../etc/passwd") == STATIC_FILE_INVALID_PATH);
}

/* Decoding happens before the ".." check, so encoding the dots buys the
 * attacker nothing. A resolver that checked the raw target first would
 * let every one of these through. */
static void test_encoded_path_traversal_is_rejected(void) {
    CHECK(resolve("/%2e%2e/Makefile") == STATIC_FILE_INVALID_PATH);
    CHECK(resolve("/%2E%2E/Makefile") == STATIC_FILE_INVALID_PATH);
    CHECK(resolve("/..%2fMakefile") == STATIC_FILE_INVALID_PATH);
    CHECK(resolve("/%2e%2e%2fMakefile") == STATIC_FILE_INVALID_PATH);
    CHECK(resolve("/assets/%2e%2e/%2e%2e/Makefile") == STATIC_FILE_INVALID_PATH);
    /* Double encoding: "%252e" decodes to the literal text "%2e", which
     * is a filename, not a dot. One decoding pass only, so this is an
     * ordinary miss rather than a traversal. */
    CHECK(resolve("/%252e%252e/Makefile") == STATIC_FILE_NOT_FOUND);
}

static void test_malformed_percent_encoding_is_rejected(void) {
    CHECK(resolve("/hello%2") == STATIC_FILE_INVALID_PATH);  /* truncated escape */
    CHECK(resolve("/hello%") == STATIC_FILE_INVALID_PATH);
    CHECK(resolve("/hello%zz.txt") == STATIC_FILE_INVALID_PATH); /* not hex */
    CHECK(resolve("/hello%2g.txt") == STATIC_FILE_INVALID_PATH);
}

/* "%00" is the classic truncation trick: length-based checks see the
 * full name, open() stops at the NUL and gets a different file. */
static void test_encoded_nul_is_rejected(void) {
    CHECK(resolve("/hello.txt%00.png") == STATIC_FILE_INVALID_PATH);
    CHECK(resolve("/%00") == STATIC_FILE_INVALID_PATH);
}

static void test_non_absolute_path_is_rejected(void) {
    CHECK(resolve("hello.txt") == STATIC_FILE_INVALID_PATH);
    CHECK(resolve("") == STATIC_FILE_INVALID_PATH);
    /* Absolute-form targets ("GET http://host/x") aren't origin-form and
     * this server doesn't accept them as file paths either. */
    CHECK(resolve("http://example.com/hello.txt") == STATIC_FILE_INVALID_PATH);
}

static void test_overlong_path_is_a_miss_not_an_error(void) {
    char path[PATH_MAX * 2];
    memset(path, 'a', sizeof(path) - 1);
    path[0] = '/';
    path[sizeof(path) - 1] = '\0';
    /* Nothing hostile about it, it simply cannot name anything under a
     * root that leaves less room than this. */
    CHECK(resolve(path) == STATIC_FILE_NOT_FOUND);
}

/* --- tests against a scratch root built at runtime ------------------ */

/* The scratch root is always a mkdtemp() of this one template, and the
 * sibling is that same name plus a short suffix, so both have a length
 * known at compile time. Sizing them at PATH_MAX instead would be a
 * claim the code never makes good on, and it costs something real: the
 * compiler then has to assume a 4095-character root, which makes every
 * "%s/<name>" below look like it might not fit its PATH_MAX destination
 * (-Wformat-truncation, visible once optimization is on). Right-sizing
 * these is what lets it prove the concatenations fit. */
#define SCRATCH_TEMPLATE "/tmp/http-server-static-XXXXXX"
#define SCRATCH_SIBLING_SUFFIX "evil"
#define SCRATCH_ROOT_SIZE (sizeof(SCRATCH_TEMPLATE) + sizeof(SCRATCH_SIBLING_SUFFIX))

static char g_scratch[SCRATCH_ROOT_SIZE];
static char g_scratch_sibling[SCRATCH_ROOT_SIZE];

static void scratch_path(char *out, size_t out_size, const char *name) {
    snprintf(out, out_size, "%s/%s", g_scratch, name);
}

static bool write_file(const char *full_path, const char *contents) {
    FILE *f = fopen(full_path, "w");
    if (f == NULL) {
        perror("fopen");
        return false;
    }
    fputs(contents, f);
    return fclose(f) == 0;
}

/* Builds:
 *   <scratch>/plain.dat          regular file, unknown extension
 *   <scratch>/inside.txt         regular file
 *   <scratch>/link-inside        symlink -> inside.txt        (must be served)
 *   <scratch>/link-escape        symlink -> /etc/hostname     (must not be)
 *   <scratch>/link-sibling       symlink -> <scratch>evil/f   (prefix trap)
 *   <scratch>/fifo               FIFO, which sendfile() cannot read
 *   <scratch>evil/secret.txt     outside the root, shares its name prefix
 */
static bool setup_scratch_root(void) {
    char template[] = SCRATCH_TEMPLATE;
    if (mkdtemp(template) == NULL) {
        perror("mkdtemp");
        return false;
    }
    snprintf(g_scratch, sizeof(g_scratch), "%s", template);
    snprintf(g_scratch_sibling, sizeof(g_scratch_sibling), "%s" SCRATCH_SIBLING_SUFFIX, template);
    if (mkdir(g_scratch_sibling, 0700) < 0) {
        perror("mkdir(sibling)");
        return false;
    }

    char path[PATH_MAX];
    char target[PATH_MAX];

    scratch_path(path, sizeof(path), "plain.dat");
    if (!write_file(path, "binary-ish\n")) {
        return false;
    }
    scratch_path(path, sizeof(path), "inside.txt");
    if (!write_file(path, "inside the root\n")) {
        return false;
    }
    snprintf(path, sizeof(path), "%s/secret.txt", g_scratch_sibling);
    if (!write_file(path, "outside the root\n")) {
        return false;
    }

    scratch_path(path, sizeof(path), "link-inside");
    if (symlink("inside.txt", path) < 0) {
        perror("symlink(inside)");
        return false;
    }
    scratch_path(path, sizeof(path), "link-escape");
    if (symlink("/etc/hostname", path) < 0) {
        perror("symlink(escape)");
        return false;
    }
    scratch_path(path, sizeof(path), "link-sibling");
    snprintf(target, sizeof(target), "%s/secret.txt", g_scratch_sibling);
    if (symlink(target, path) < 0) {
        perror("symlink(sibling)");
        return false;
    }
    scratch_path(path, sizeof(path), "fifo");
    if (mkfifo(path, 0600) < 0) {
        perror("mkfifo");
        return false;
    }
    return true;
}

static void remove_scratch_root(void) {
    static const char *names[] = {"plain.dat", "inside.txt", "link-inside", "link-escape", "link-sibling", "fifo"};
    char path[PATH_MAX];
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        scratch_path(path, sizeof(path), names[i]);
        unlink(path);
    }
    rmdir(g_scratch);

    snprintf(path, sizeof(path), "%s/secret.txt", g_scratch_sibling);
    unlink(path);
    rmdir(g_scratch_sibling);
}

static void test_unknown_extension_falls_back_to_octet_stream(void) {
    static_file_t file;
    CHECK(static_file_open("/plain.dat", strlen("/plain.dat"), &file) == STATIC_FILE_OK);
    CHECK(strcmp(file.content_type, "application/octet-stream") == 0);
    static_file_close(&file);
}

/* Symlinks are fine as long as what they point at stays inside: the
 * containment check looks at the file that was actually opened, not at
 * the name the client typed. */
static void test_symlink_inside_the_root_is_served(void) {
    CHECK(resolve("/link-inside") == STATIC_FILE_OK);
}

/* The one escape the ".." rejection cannot catch, since nothing in the
 * request path is suspicious at all. */
static void test_symlink_escaping_the_root_is_not_found(void) {
    /* 404 rather than 400 here: the request was well formed, and
     * answering "not found" declines to confirm that /etc/hostname
     * exists. */
    CHECK(resolve("/link-escape") == STATIC_FILE_NOT_FOUND);
}

/* Catches a containment check written as a bare strncmp(): "/tmp/xevil"
 * starts with "/tmp/x" but is not under it. */
static void test_sibling_directory_sharing_the_root_prefix_is_not_found(void) {
    CHECK(resolve("/link-sibling") == STATIC_FILE_NOT_FOUND);
}

/* sendfile(2) needs an in_fd that supports mmap-style reads, which rules
 * out FIFOs; opening one would also have blocked the event loop if the
 * resolver hadn't passed O_NONBLOCK. */
static void test_fifo_is_not_served(void) {
    CHECK(resolve("/fifo") == STATIC_FILE_NOT_FOUND);
}

/* --- root configuration --------------------------------------------- */

static void test_init_rejects_a_root_that_is_not_a_directory(void) {
    CHECK(static_files_init("public/hello.txt") < 0);
    CHECK(static_files_init("public/definitely-not-here") < 0);
}

int main(void) {
    /* Relative to the working directory, so this must run from the
     * repository root, which is how the Makefile invokes it. */
    if (static_files_init("public") < 0) {
        fprintf(stderr, "could not init static root 'public' (run from the repository root)\n");
        return 1;
    }

    RUN(test_existing_files_are_served_with_a_content_type);
    RUN(test_missing_file_is_not_found);
    RUN(test_directories_are_not_served);
    RUN(test_query_string_and_fragment_are_ignored);
    RUN(test_redundant_path_syntax_is_normalized);
    RUN(test_percent_encoding_is_decoded);
    RUN(test_path_traversal_is_rejected);
    RUN(test_encoded_path_traversal_is_rejected);
    RUN(test_malformed_percent_encoding_is_rejected);
    RUN(test_encoded_nul_is_rejected);
    RUN(test_non_absolute_path_is_rejected);
    RUN(test_overlong_path_is_a_miss_not_an_error);
    RUN(test_init_rejects_a_root_that_is_not_a_directory);

    if (!setup_scratch_root()) {
        fprintf(stderr, "could not build the scratch root\n");
        remove_scratch_root();
        return 1;
    }
    if (static_files_init(g_scratch) < 0) {
        remove_scratch_root();
        return 1;
    }

    RUN(test_unknown_extension_falls_back_to_octet_stream);
    RUN(test_symlink_inside_the_root_is_served);
    RUN(test_symlink_escaping_the_root_is_not_found);
    RUN(test_sibling_directory_sharing_the_root_prefix_is_not_found);
    RUN(test_fifo_is_not_served);

    remove_scratch_root();

    printf("\n%d checks run, %d failed\n", g_tests_run, g_tests_failed);
    return g_tests_failed == 0 ? 0 : 1;
}
