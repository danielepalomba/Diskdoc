#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "dd_utils.h"

/* Drops leading and trailing blanks in place and returns the new start. */
static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;

    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t')) end--;
    *end = '\0';

    return s;
}

/* Removes one matching pair of surrounding quotes. A lone or unbalanced quote
   is left alone instead of being consumed, which used to write before the
   start of the value. */
static char *unquote(char *s) {
    size_t len = strlen(s);

    if (len >= 2 && (s[0] == '"' || s[0] == '\'') && s[len - 1] == s[0]) {
        s[len - 1] = '\0';
        return s + 1;
    }

    return s;
}

/* Opens a credentials file only when nobody but its owner can read it: a
   regular file, owned by root or by owner_uid, with no group or other bits.
   O_NOFOLLOW stops a symlink planted by someone else from redirecting the
   read. Returns NULL, silently for a missing file, otherwise with a reason. */
FILE *dd_open_private_file(const char *path, uid_t owner_uid) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ELOOP)
            fprintf(stderr, "[Error] %s is a symbolic link, refusing to read it\n", path);
        else if (errno != ENOENT && errno != ENOTDIR)
            fprintf(stderr, "[Error] Cannot open %s: %s\n", path, strerror(errno));
        return NULL;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        fprintf(stderr, "[Error] Cannot stat %s: %s\n", path, strerror(errno));
        close(fd);
        return NULL;
    }

    if (!S_ISREG(st.st_mode)) {
        fprintf(stderr, "[Error] %s is not a regular file, refusing to read it\n", path);
        close(fd);
        return NULL;
    }

    if (st.st_uid != 0 && st.st_uid != owner_uid) {
        fprintf(stderr, "[Error] %s is owned by uid %u, refusing to read it\n",
                path, (unsigned)st.st_uid);
        close(fd);
        return NULL;
    }

    if (st.st_mode & (S_IRWXG | S_IRWXO)) {
        fprintf(stderr, "[Error] %s is readable by other users (mode %04o), "
                        "refusing to read it: run 'chmod 600 %s'\n",
                path, (unsigned)(st.st_mode & 07777), path);
        close(fd);
        return NULL;
    }

    FILE *file = fdopen(fd, "r");
    if (file == NULL) close(fd);

    return file;
}

/* Reads KEY=VALUE lines and hands each pair to the sink, which decides what to
   keep. Nothing is ever exported to the environment, so an unexpected name in
   the file cannot reach the process or its children. */
void dd_parse_env_stream(FILE *file, dd_env_sink sink, void *ctx) {
    char line[DD_ENV_LINE_MAX];

    while (fgets(line, sizeof line, file)) {
        size_t len = strlen(line);
        int overflowed = (len == sizeof line - 1 && line[len - 1] != '\n');

        line[strcspn(line, "\r\n")] = '\0';

        if (overflowed) {
            int c;
            while ((c = fgetc(file)) != '\n' && c != EOF) { }
            continue;
        }

        char *key = trim(line);
        if (key[0] == '\0' || key[0] == '#') continue;

        char *delimiter = strchr(key, '=');
        if (delimiter == NULL) continue;

        *delimiter = '\0';
        char *value = unquote(trim(delimiter + 1));
        key = trim(key);

        if (key[0] != '\0') sink(key, value, ctx);
    }
}

/* Overwrites a buffer through a volatile pointer so the compiler cannot drop
   the writes as dead stores. */
void dd_wipe(void *buf, size_t size) {
    volatile unsigned char *p = buf;
    while (size--) *p++ = 0;
}

/* Resolves the human behind the process: under sudo that is SUDO_UID, not
   root. Returns 1 when running as root on behalf of another user, which is
   the only case where there is anything to drop. */
int dd_real_user(uid_t *uid, gid_t *gid) {
    const char *sudo_uid = getenv("SUDO_UID");
    const char *sudo_gid = getenv("SUDO_GID");

    if (geteuid() == 0 && sudo_uid != NULL && sudo_gid != NULL) {
        char *end;
        unsigned long u = strtoul(sudo_uid, &end, 10);

        if (*end == '\0' && u != 0 && u <= UINT_MAX) {
            unsigned long g = strtoul(sudo_gid, &end, 10);

            if (*end == '\0' && g <= UINT_MAX) {
                *uid = (uid_t)u;
                *gid = (gid_t)g;
                return 1;
            }
        }
    }

    *uid = getuid();
    *gid = getgid();
    return 0;
}

/* Gives up root for good, groups first and uid last, then checks that the
   privileges cannot be taken back. Returns 0 when there was nothing to drop
   or the drop succeeded, -1 when it could not be completed. */
int dd_drop_privileges(void) {
    uid_t uid;
    gid_t gid;

    if (!dd_real_user(&uid, &gid)) return 0;

    if (setgroups(1, &gid) != 0) return -1;
    if (setresgid(gid, gid, gid) != 0) return -1;
    if (setresuid(uid, uid, uid) != 0) return -1;
    if (setresuid(0, 0, 0) == 0) return -1;

    return 0;
}

/* Builds the config directory of the real user. Under sudo the passwd entry
   wins over HOME and XDG_CONFIG_HOME, which still belong to root there. */
int dd_user_config_dir(char *out, size_t out_size) {
    uid_t uid;
    gid_t gid;
    int elevated = dd_real_user(&uid, &gid);

    if (!elevated) {
        const char *xdg = getenv("XDG_CONFIG_HOME");
        if (xdg != NULL && xdg[0] == '/')
            return snprintf(out, out_size, "%s/diskdoc", xdg) < (int)out_size;
    }

    struct passwd *pw = getpwuid(uid);
    if (pw == NULL || pw->pw_dir == NULL || pw->pw_dir[0] != '/') {
        fprintf(stderr, "[Error] Cannot resolve the home directory of uid %u\n", (unsigned)uid);
        return 0;
    }

    return snprintf(out, out_size, "%s/.config/diskdoc", pw->pw_dir) < (int)out_size;
}
