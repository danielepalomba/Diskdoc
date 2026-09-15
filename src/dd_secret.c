#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "dd_secret.h"
#include "dd_utils.h"

#define DD_COMMAND_TIMEOUT 15
#define DD_FILE_MAX 65536

static const char *const provider_names[DD_PROVIDER_COUNT] = {
    "openai", "anthropic", "gemini"
};

static const char *const provider_env[DD_PROVIDER_COUNT] = {
    "OPENAI_API_KEY", "ANTHROPIC_API_KEY", "GEMINI_API_KEY"
};

const char *dd_provider_name(ai_provider provider) {
    if (provider < 0 || provider >= DD_PROVIDER_COUNT) return "none";
    return provider_names[provider];
}

ai_provider dd_provider_from_name(const char *name) {
    if (name == NULL) return PROVIDER_NONE;

    for (int i = 0; i < DD_PROVIDER_COUNT; i++)
        if (strcasecmp(name, provider_names[i]) == 0) return (ai_provider)i;

    return PROVIDER_NONE;
}

/* A key travels inside an HTTP header, so anything outside printable ASCII
   is rejected rather than sanitised: a stray CR or LF would let whatever
   wrote the file append headers of its own to the request. */
static int key_is_valid(const char *key) {
    if (key[0] == '\0') return 0;

    for (const unsigned char *p = (const unsigned char *)key; *p != '\0'; p++)
        if (*p < 0x20 || *p > 0x7e) return 0;

    return 1;
}

int dd_bundle_has_key(const dd_key_bundle *bundle) {
    for (int i = 0; i < DD_PROVIDER_COUNT; i++)
        if (bundle->keys[i][0] != '\0') return 1;

    return 0;
}

/* Keeps only the three names diskdoc knows about: every other assignment in
   the file is dropped instead of reaching the process. */
static void bundle_sink(const char *key, const char *value, void *ctx) {
    dd_key_bundle *bundle = ctx;

    if (strcmp(key, "DISKDOC_PROVIDER") == 0) {
        snprintf(bundle->provider, sizeof bundle->provider, "%s", value);
        return;
    }

    for (int i = 0; i < DD_PROVIDER_COUNT; i++) {
        if (strcmp(key, provider_env[i]) != 0) continue;

        if (value[0] == '\0') return;

        if (!key_is_valid(value)) {
            fprintf(stderr, "[Error] %s contains invalid characters, ignoring it\n", key);
            return;
        }

        snprintf(bundle->keys[i], DD_SECRET_MAX, "%s", value);
        return;
    }
}

int dd_credentials_path(char *out, size_t out_size) {
    char dir[PATH_MAX];

    if (!dd_user_config_dir(dir, sizeof dir)) return 0;

    return snprintf(out, out_size, "%s/%s", dir, DD_CREDENTIALS_NAME) < (int)out_size;
}

int dd_secret_load_file(const char *path, uid_t owner_uid, dd_key_bundle *bundle) {
    FILE *file = dd_open_private_file(path, owner_uid);
    if (file == NULL) return 0;

    dd_parse_env_stream(file, bundle_sink, bundle);
    fclose(file);

    return dd_bundle_has_key(bundle);
}

/* Copies a file into fd with the caller's identity, used by the child that
   already gave up root. */
static int copy_file_to_fd(const char *path, uid_t uid, int fd) {
    FILE *file = dd_open_private_file(path, uid);
    if (file == NULL) return 0;

    char chunk[4096];
    size_t n;

    while ((n = fread(chunk, 1, sizeof chunk, file)) > 0)
        if (write(fd, chunk, n) != (ssize_t)n) break;

    dd_wipe(chunk, sizeof chunk);
    fclose(file);
    return 1;
}

/* Reads the credentials file with the real user's identity: a path under
   someone else's home, or on a network mount, is never opened with root's
   privileges. The child copies the bytes into a pipe and the parent parses
   them, so the file itself is only ever touched unprivileged. */
static int load_file_as_user(const char *path, dd_key_bundle *bundle) {
    uid_t uid;
    gid_t gid;

    if (!dd_real_user(&uid, &gid) || geteuid() != 0)
        return dd_secret_load_file(path, uid, bundle);

    int pipefd[2];
    if (pipe(pipefd) != 0) return 0;

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return 0;
    }

    if (pid == 0) {
        close(pipefd[0]);

        if (dd_drop_privileges() != 0) _exit(1);

        int ok = copy_file_to_fd(path, uid, pipefd[1]);
        close(pipefd[1]);
        _exit(ok ? 0 : 1);
    }

    close(pipefd[1]);

    char buffer[DD_FILE_MAX];
    size_t len = 0;
    ssize_t n;

    while (len < sizeof buffer - 1 &&
           (n = read(pipefd[0], buffer + len, sizeof buffer - 1 - len)) > 0)
        len += (size_t)n;

    buffer[len] = '\0';
    close(pipefd[0]);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        dd_wipe(buffer, sizeof buffer);
        return 0;
    }

    FILE *stream = fmemopen(buffer, len, "r");
    if (stream != NULL) {
        dd_parse_env_stream(stream, bundle_sink, bundle);
        fclose(stream);
    }

    dd_wipe(buffer, sizeof buffer);
    return dd_bundle_has_key(bundle);
}

/* Picks up keys the caller already exported, for CI and for 'sudo -E'. These
   are the only variables diskdoc reads, and it never writes any. */
static int load_from_environment(dd_key_bundle *bundle) {
    for (int i = 0; i < DD_PROVIDER_COUNT; i++) {
        const char *value = getenv(provider_env[i]);
        if (value != NULL && value[0] != '\0' && key_is_valid(value))
            snprintf(bundle->keys[i], DD_SECRET_MAX, "%s", value);
    }

    return dd_bundle_has_key(bundle);
}

/* Runs the helper named by DISKDOC_API_KEY_CMD and takes the first line of
   its output as the key, so the secret can stay in pass, gpg or any other
   manager. It runs unprivileged and under a hard time limit, and diskdoc
   keeps nothing on disk. */
int dd_secret_load_command(const char *command, char *out, size_t out_size) {
    if (command == NULL || command[0] == '\0') return 0;

    int pipefd[2];
    if (pipe(pipefd) != 0) return 0;

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return 0;
    }

    if (pid == 0) {
        close(pipefd[0]);

        if (dd_drop_privileges() != 0) _exit(127);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0) _exit(127);

        close(pipefd[1]);
        alarm(DD_COMMAND_TIMEOUT);
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }

    close(pipefd[1]);

    char buffer[DD_SECRET_MAX * 4];
    size_t len = 0;
    ssize_t n;

    while ((n = read(pipefd[0], buffer + len, sizeof buffer - 1 - len)) > 0) {
        len += (size_t)n;
        if (len >= sizeof buffer - 1) break;
    }

    buffer[len] = '\0';
    close(pipefd[0]);

    int status = 0;
    int failed = (waitpid(pid, &status, 0) < 0) || !WIFEXITED(status) || WEXITSTATUS(status) != 0;

    if (failed) {
        fprintf(stderr, "[Error] DISKDOC_API_KEY_CMD failed\n");
        dd_wipe(buffer, sizeof buffer);
        return 0;
    }

    buffer[strcspn(buffer, "\r\n")] = '\0';

    if (!key_is_valid(buffer)) {
        fprintf(stderr, "[Error] DISKDOC_API_KEY_CMD produced no usable key\n");
        dd_wipe(buffer, sizeof buffer);
        return 0;
    }

    snprintf(out, out_size, "%s", buffer);
    dd_wipe(buffer, sizeof buffer);
    return 1;
}

/* Resolves which provider to talk to: an explicit DISKDOC_PROVIDER wins,
   otherwise the single key that was found. Several keys with no choice made
   is an error, not a guess based on the order of a file. */
ai_provider dd_provider_pick(const dd_key_bundle *bundle, const char *requested) {
    if (requested != NULL && requested[0] != '\0') {
        ai_provider provider = dd_provider_from_name(requested);

        if (provider == PROVIDER_NONE) {
            fprintf(stderr, "[Error] Unknown provider '%s': expected openai, anthropic or gemini\n",
                    requested);
            return PROVIDER_NONE;
        }

        if (bundle->keys[provider][0] == '\0') {
            fprintf(stderr, "[Error] No key stored for %s: run 'diskdoc --set-key %s'\n",
                    requested, dd_provider_name(provider));
            return PROVIDER_NONE;
        }

        return provider;
    }

    ai_provider found = PROVIDER_NONE;
    int count = 0;

    for (int i = 0; i < DD_PROVIDER_COUNT; i++) {
        if (bundle->keys[i][0] == '\0') continue;
        count++;
        if (found == PROVIDER_NONE) found = (ai_provider)i;
    }

    if (count > 1) {
        fprintf(stderr, "[Error] Several keys are available: set DISKDOC_PROVIDER to one of");
        for (int i = 0; i < DD_PROVIDER_COUNT; i++)
            if (bundle->keys[i][0] != '\0') fprintf(stderr, " %s", provider_names[i]);
        fprintf(stderr, "\n");
        return PROVIDER_NONE;
    }

    return found;
}

/* Fills out with the key to use, trying the sources from the most explicit to
   the most implicit: helper command, per-user credentials file, already
   exported variables, and finally the deprecated system-wide file.
   Returns 1 when a usable key was found. */
int dd_secret_resolve(dd_secret *out) {
    memset(out, 0, sizeof *out);
    out->provider = PROVIDER_NONE;

    const char *requested = getenv("DISKDOC_PROVIDER");
    const char *command = getenv("DISKDOC_API_KEY_CMD");

    if (command != NULL && command[0] != '\0') {
        ai_provider provider = dd_provider_from_name(requested);

        if (provider == PROVIDER_NONE) {
            fprintf(stderr, "[Error] DISKDOC_API_KEY_CMD needs DISKDOC_PROVIDER "
                            "set to openai, anthropic or gemini\n");
            return 0;
        }

        if (!dd_secret_load_command(command, out->key, sizeof out->key)) return 0;

        out->provider = provider;
        return 1;
    }

    dd_key_bundle bundle;
    memset(&bundle, 0, sizeof bundle);

    char path[PATH_MAX];
    int found = dd_credentials_path(path, sizeof path) && load_file_as_user(path, &bundle);

    if (!found) found = load_from_environment(&bundle);

    if (!found) {
        found = dd_secret_load_file(DD_LEGACY_ENV_PATH, 0, &bundle);
        if (found)
            fprintf(stderr, "[Warning] %s is deprecated: move the key with "
                            "'diskdoc --set-key <provider>' and delete that file\n",
                    DD_LEGACY_ENV_PATH);
    }

    if (!found) {
        fprintf(stderr, "[Error] No API key found: run 'diskdoc --set-key <provider>'\n");
        return 0;
    }

    if (requested == NULL || requested[0] == '\0') requested = bundle.provider;

    ai_provider provider = dd_provider_pick(&bundle, requested);

    if (provider != PROVIDER_NONE) {
        out->provider = provider;
        snprintf(out->key, sizeof out->key, "%s", bundle.keys[provider]);
    }

    dd_wipe(&bundle, sizeof bundle);
    return provider != PROVIDER_NONE;
}

void dd_secret_clear(dd_secret *secret) {
    dd_wipe(secret, sizeof *secret);
    secret->provider = PROVIDER_NONE;
}

/* Creates a directory and every missing parent with 0700, the way the config
   directory of a user is expected to look. */
static int make_dir_chain(const char *path) {
    char buffer[PATH_MAX];

    if (snprintf(buffer, sizeof buffer, "%s", path) >= (int)sizeof buffer) return 0;

    for (char *p = buffer + 1; *p != '\0'; p++) {
        if (*p != '/') continue;

        *p = '\0';
        if (mkdir(buffer, 0700) != 0 && errno != EEXIST) return 0;
        *p = '/';
    }

    return mkdir(buffer, 0700) == 0 || errno == EEXIST;
}

/* Rewrites the credentials file from the keys already there plus the new one,
   through a temporary file in the same directory and a rename, so a failed
   write never leaves a half-written or world-readable file behind. */
static int write_credentials(ai_provider provider, const char *key, uid_t uid) {
    char dir[PATH_MAX];
    char path[PATH_MAX];
    char tmp[PATH_MAX];

    if (!dd_user_config_dir(dir, sizeof dir)) return 0;
    if (snprintf(path, sizeof path, "%s/%s", dir, DD_CREDENTIALS_NAME) >= (int)sizeof path) return 0;
    if (snprintf(tmp, sizeof tmp, "%s/%s.XXXXXX", dir, DD_CREDENTIALS_NAME) >= (int)sizeof tmp) return 0;

    if (!make_dir_chain(dir)) {
        fprintf(stderr, "[Error] Cannot create %s: %s\n", dir, strerror(errno));
        return 0;
    }

    dd_key_bundle bundle;
    memset(&bundle, 0, sizeof bundle);
    dd_secret_load_file(path, uid, &bundle);

    snprintf(bundle.keys[provider], DD_SECRET_MAX, "%s", key);

    int stored_keys = 0;
    for (int i = 0; i < DD_PROVIDER_COUNT; i++)
        if (bundle.keys[i][0] != '\0') stored_keys++;

    int fd = mkstemp(tmp);
    if (fd < 0) {
        fprintf(stderr, "[Error] Cannot write in %s: %s\n", dir, strerror(errno));
        dd_wipe(&bundle, sizeof bundle);
        return 0;
    }

    int ok = (fchmod(fd, S_IRUSR | S_IWUSR) == 0);
    FILE *file = ok ? fdopen(fd, "w") : NULL;

    if (file == NULL) {
        close(fd);
        unlink(tmp);
        dd_wipe(&bundle, sizeof bundle);
        return 0;
    }

    fprintf(file, "# diskdoc credentials, keep this file readable by you only\n");

    if (bundle.provider[0] != '\0')
        fprintf(file, "DISKDOC_PROVIDER=%s\n", bundle.provider);

    for (int i = 0; i < DD_PROVIDER_COUNT; i++)
        if (bundle.keys[i][0] != '\0')
            fprintf(file, "%s=%s\n", provider_env[i], bundle.keys[i]);

    ok = (fflush(file) == 0) && (fsync(fileno(file)) == 0);
    fclose(file);
    dd_wipe(&bundle, sizeof bundle);

    if (!ok || rename(tmp, path) != 0) {
        fprintf(stderr, "[Error] Cannot update %s: %s\n", path, strerror(errno));
        unlink(tmp);
        return 0;
    }

    printf("Key stored in %s\n", path);

    if (stored_keys > 1 && bundle.provider[0] == '\0')
        printf("More than one key is stored: add a DISKDOC_PROVIDER=<provider> line "
               "to that file to choose which one -i uses.\n");

    return 1;
}

/* Stores the key as the real user, never as root, so the file lands in the
   right home with the right owner. */
int dd_secret_store(ai_provider provider, const char *key) {
    if (provider == PROVIDER_NONE) return 0;

    if (!key_is_valid(key)) {
        fprintf(stderr, "[Error] The key contains invalid characters\n");
        return 0;
    }

    uid_t uid;
    gid_t gid;

    if (!dd_real_user(&uid, &gid) || geteuid() != 0)
        return write_credentials(provider, key, uid);

    pid_t pid = fork();
    if (pid < 0) return 0;

    if (pid == 0) {
        if (dd_drop_privileges() != 0) _exit(1);
        _exit(write_credentials(provider, key, uid) ? 0 : 1);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return 0;

    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/* Reads the key from stdin with the echo off when it is a terminal, so it
   never shows on screen nor in the shell history. A piped stdin is read as
   is, which is what a script needs. */
int dd_secret_prompt(const char *label, char *out, size_t out_size) {
    struct termios saved;
    int hidden = 0;

    if (isatty(STDIN_FILENO)) {
        struct termios quiet;

        if (tcgetattr(STDIN_FILENO, &saved) == 0) {
            quiet = saved;
            quiet.c_lflag &= (unsigned)~ECHO;
            hidden = (tcsetattr(STDIN_FILENO, TCSAFLUSH, &quiet) == 0);
        }

        fprintf(stderr, "%s", label);
        fflush(stderr);
    }

    char line[DD_SECRET_MAX];
    char *got = fgets(line, sizeof line, stdin);

    if (hidden) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved);
        fprintf(stderr, "\n");
    }

    if (got == NULL) return 0;

    line[strcspn(line, "\r\n")] = '\0';

    if (!key_is_valid(line)) {
        dd_wipe(line, sizeof line);
        fprintf(stderr, "[Error] Empty or invalid key, nothing was stored\n");
        return 0;
    }

    snprintf(out, out_size, "%s", line);
    dd_wipe(line, sizeof line);
    return 1;
}
