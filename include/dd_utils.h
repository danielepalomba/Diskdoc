#ifndef DD_UTILS_H
#define DD_UTILS_H

#include <stddef.h>

/* System-wide config installed by install.sh */
#define DD_SYSTEM_ENV_PATH "/etc/diskdoc/.env"

void load_env_file(const char *filepath);

int find_dotenv_path(const char *filename, char *out_path, size_t out_size);

int resolve_dotenv_path(char *out_path, size_t out_size);

#endif
