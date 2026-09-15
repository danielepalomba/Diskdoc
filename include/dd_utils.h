#ifndef DD_UTILS_H
#define DD_UTILS_H

#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>

/* Only kept so old installations can be migrated away from it: diskdoc
   no longer creates this file and reading it is deprecated. */
#define DD_LEGACY_ENV_PATH "/etc/diskdoc/.env"

#define DD_ENV_LINE_MAX 512

typedef void (*dd_env_sink)(const char *key, const char *value, void *ctx);

FILE *dd_open_private_file(const char *path, uid_t owner_uid);

void dd_parse_env_stream(FILE *file, dd_env_sink sink, void *ctx);

void dd_wipe(void *buf, size_t size);

int dd_real_user(uid_t *uid, gid_t *gid);

int dd_drop_privileges(void);

int dd_user_config_dir(char *out, size_t out_size);

#endif
