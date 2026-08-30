#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>

#include "dd_utils.h"

#define DD_DOTENV_MAX_DEPTH 32

/* Walks up from the current working directory looking for a file named
   `filename`, so it can be found from any subdirectory of the
   project. */
int find_dotenv_path(const char *filename, char *out_path, size_t out_size) {
    char dir[PATH_MAX];
    if (getcwd(dir, sizeof dir) == NULL) return 0;

    for (int depth = 0; depth < DD_DOTENV_MAX_DEPTH; depth++) {
        char candidate[PATH_MAX + NAME_MAX + 2];
        snprintf(candidate, sizeof candidate, "%s/%s", dir, filename);

        if (access(candidate, F_OK) == 0) {
            snprintf(out_path, out_size, "%s", candidate);
            return 1;
        }

        char *slash = strrchr(dir, '/');
        if (slash == NULL || slash == dir) break;
        *slash = '\0';
    }

    return 0;
}

/* Resolves which .env to use: a project-local .env found by walking up from
   the current directory takes priority, falling back to DD_SYSTEM_ENV_PATH 
   so the tool still finds its configuration when run from anywhere else. */
int resolve_dotenv_path(char *out_path, size_t out_size) {
    if (find_dotenv_path(".env", out_path, out_size)) return 1;

    if (access(DD_SYSTEM_ENV_PATH, F_OK) == 0) {
        snprintf(out_path, out_size, "%s", DD_SYSTEM_ENV_PATH);
        return 1;
    }

    return 0;
}

void load_env_file(const char *filepath) {
    char resolved[PATH_MAX];
    const char *path_to_use = filepath;

    if (strchr(filepath, '/') == NULL && resolve_dotenv_path(resolved, sizeof resolved))
        path_to_use = resolved;

    FILE *file = fopen(path_to_use, "r");
    if (!file) return;

    char line[512];
    while (fgets(line, sizeof(line), file)) {

        line[strcspn(line, "\r\n")] = '\0';

        if (line[0] == '\0' || line[0] == '#') continue;

        char *delimiter = strchr(line, '=');
        if (delimiter) {
            *delimiter = '\0';
            char *key = line;
            char *val = delimiter + 1;

            if (val[0] == '"' || val[0] == '\'') {
                val++;
                val[strlen(val) - 1] = '\0';
            }

            setenv(key, val, 1);

        }
    }
    fclose(file);
}
