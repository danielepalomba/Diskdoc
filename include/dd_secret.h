#ifndef DD_SECRET_H
#define DD_SECRET_H

#include <stddef.h>
#include <sys/types.h>

#include "dd_ai.h"

#define DD_SECRET_MAX 256
#define DD_PROVIDER_COUNT 3
#define DD_CREDENTIALS_NAME "credentials"

typedef struct {
    char keys[DD_PROVIDER_COUNT][DD_SECRET_MAX];
    char provider[32];
} dd_key_bundle;

typedef struct {
    ai_provider provider;
    char key[DD_SECRET_MAX];
} dd_secret;

const char *dd_provider_name(ai_provider provider);

ai_provider dd_provider_from_name(const char *name);

int dd_credentials_path(char *out, size_t out_size);

int dd_secret_resolve(dd_secret *out);

void dd_secret_clear(dd_secret *secret);

int dd_secret_store(ai_provider provider, const char *key);

int dd_secret_prompt(const char *label, char *out, size_t out_size);

int dd_secret_load_file(const char *path, uid_t owner_uid, dd_key_bundle *bundle);

int dd_secret_load_command(const char *command, char *out, size_t out_size);

ai_provider dd_provider_pick(const dd_key_bundle *bundle, const char *requested);

int dd_bundle_has_key(const dd_key_bundle *bundle);

#endif
