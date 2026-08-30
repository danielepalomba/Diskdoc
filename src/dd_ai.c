#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <curl/curl.h>
#include <limits.h>
#include <cJSON.h>

#include "dd_ai.h"
#include "dd_utils.h"

/* Loads the API keys set in the environment (populated from .env by load_env_file)
   and infers the provider to use from whichever key appears first in the .env file. */
ai_provider load_keys(key_store *ks) {
    memset(ks, 0, sizeof(key_store));

    const char *oa = getenv("OPENAI_API_KEY");
    const char *an = getenv("ANTHROPIC_API_KEY");
    const char *ge = getenv("GEMINI_API_KEY");

    if (oa) strncpy(ks->openai_key, oa, sizeof(ks->openai_key) - 1);
    if (an) strncpy(ks->anthropic_key, an, sizeof(ks->anthropic_key) - 1);
    if (ge) strncpy(ks->gemini_key, ge, sizeof(ks->gemini_key) - 1);

    ai_provider provider = PROVIDER_NONE;

    char env_path[PATH_MAX];
    FILE *file = resolve_dotenv_path(env_path, sizeof env_path)
                 ? fopen(env_path, "r") : NULL;
    if (file) {
        char line[512];
        while (fgets(line, sizeof(line), file)) {
            line[strcspn(line, "\r\n")] = '\0';
            if (line[0] == '\0' || line[0] == '#') continue;

            if (strlen(ks->openai_key) > 0 && strncmp(line, "OPENAI_API_KEY", 14) == 0) {
                provider = PROVIDER_OPENAI;
                break;
            }
            if (strlen(ks->anthropic_key) > 0 && strncmp(line, "ANTHROPIC_API_KEY", 17) == 0) {
                provider = PROVIDER_ANTHROPIC;
                break;
            }
            if (strlen(ks->gemini_key) > 0 && strncmp(line, "GEMINI_API_KEY", 14) == 0) {
                provider = PROVIDER_GEMINI;
                break;
            }
        }
        fclose(file);
    }

    /* .env could not be read: fall back
       to whichever key made it into the environment, in a fixed order. */
    if (provider == PROVIDER_NONE) {
        if (strlen(ks->openai_key) > 0) provider = PROVIDER_OPENAI;
        else if (strlen(ks->anthropic_key) > 0) provider = PROVIDER_ANTHROPIC;
        else if (strlen(ks->gemini_key) > 0) provider = PROVIDER_GEMINI;
    }

    return provider;
}

typedef struct {
    char *data;
    size_t len;
} curl_buffer;

static size_t write_callback(char *ptr, size_t size, size_t nmemb, void *userdata){
    size_t chunk_size = size * nmemb;
    curl_buffer *buf = (curl_buffer *)userdata;

    char *new_data = realloc(buf->data, buf->len + chunk_size + 1);
    if (new_data == NULL) return 0;

    buf->data = new_data;
    memcpy(buf->data + buf->len, ptr, chunk_size);
    buf->len += chunk_size;
    buf->data[buf->len] = '\0';

    return chunk_size;
}

/* Pulls just the model's answer out of the provider's response envelope and
   prints it, instead of the raw JSON. */
static void print_ai_response(ai_provider provider, const char *raw_json){
    if (raw_json == NULL) return;

    cJSON *response = cJSON_Parse(raw_json);
    if (response == NULL) {
        printf("%s\n", raw_json);
        return;
    }

    cJSON *error = cJSON_GetObjectItemCaseSensitive(response, "error");
    if (error != NULL) {
        cJSON *message = cJSON_GetObjectItemCaseSensitive(error, "message");
        fprintf(stderr, "[Error] %s\n",
                cJSON_IsString(message) ? message->valuestring : "request failed");
        cJSON_Delete(response);
        return;
    }

    const char *text = NULL;

    switch (provider) {
        case PROVIDER_OPENAI: {
            cJSON *choice = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(response, "choices"), 0);
            cJSON *message = cJSON_GetObjectItemCaseSensitive(choice, "message");
            cJSON *content = cJSON_GetObjectItemCaseSensitive(message, "content");
            if (cJSON_IsString(content)) text = content->valuestring;
            break;
        }
        case PROVIDER_ANTHROPIC: {
            cJSON *content = cJSON_GetObjectItemCaseSensitive(response, "content");
            cJSON *block;
            cJSON_ArrayForEach(block, content) {
                cJSON *type = cJSON_GetObjectItemCaseSensitive(block, "type");
                if (cJSON_IsString(type) && strcmp(type->valuestring, "text") == 0) {
                    cJSON *block_text = cJSON_GetObjectItemCaseSensitive(block, "text");
                    if (cJSON_IsString(block_text)) text = block_text->valuestring;
                    break;
                }
            }
            break;
        }
        case PROVIDER_GEMINI: {
            cJSON *candidate = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(response, "candidates"), 0);
            cJSON *content = cJSON_GetObjectItemCaseSensitive(candidate, "content");
            cJSON *part = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(content, "parts"), 0);
            cJSON *part_text = cJSON_GetObjectItemCaseSensitive(part, "text");
            if (cJSON_IsString(part_text)) text = part_text->valuestring;
            break;
        }
        case PROVIDER_NONE:
            break;
    }

    printf("%s\n", text != NULL ? text : raw_json);
    cJSON_Delete(response);
}

const char *default_model_for_provider(ai_provider provider) {
    switch (provider) {
        case PROVIDER_OPENAI:    return OPENAI_DEFAULT_MODEL;
        case PROVIDER_ANTHROPIC: return ANTHROPIC_DEFAULT_MODEL;
        case PROVIDER_GEMINI:    return GEMINI_DEFAULT_MODEL;
        case PROVIDER_NONE:      return NULL;
    }
    return NULL;
}

void send_ai_prompt(const key_store *ks, const ai_request *req){
    CURL *curl = curl_easy_init();

    if(!curl) return;

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: Application/json");

    char url[512];
    cJSON *root = cJSON_CreateObject();

    switch(req->provider){
        case PROVIDER_OPENAI: {
            if(strlen(ks->openai_key) == 0){
                fprintf(stderr, "[Error] OpenAI key error.\n");
                cJSON_Delete(root);
                curl_slist_free_all(headers);
                curl_easy_cleanup(curl);
                return;
            }

            snprintf(url, sizeof(url), OPENAI_URL);
            char auth_header[300];
            snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", ks->openai_key);
            headers = curl_slist_append(headers, auth_header);

            cJSON_AddStringToObject(root, "model", req->model);
            cJSON *messages = cJSON_AddArrayToObject(root, "messages");
            cJSON *msg = cJSON_CreateObject();
            cJSON_AddStringToObject(msg, "role", "user");
            cJSON_AddStringToObject(msg, "content", req->prompt);
            cJSON_AddItemToArray(messages, msg);
            break;
        }

        case PROVIDER_ANTHROPIC: {
            if (strlen(ks->anthropic_key) == 0) {
                fprintf(stderr, "[Error] Anthropic Key error.\n");
                cJSON_Delete(root);
                curl_slist_free_all(headers);
                curl_easy_cleanup(curl);
                return;
            }

            snprintf(url, sizeof(url), ANTHROPIC_URL);
            char auth_header[300];
            snprintf(auth_header, sizeof(auth_header), "x-api-key: %s", ks->anthropic_key);
            headers = curl_slist_append(headers, auth_header);
            headers = curl_slist_append(headers, "anthropic-version: 2023-06-01");

            cJSON_AddStringToObject(root, "model", req->model);
            cJSON_AddNumberToObject(root, "max_tokens", 4096);
            cJSON *messages = cJSON_AddArrayToObject(root, "messages");
            cJSON *msg = cJSON_CreateObject();
            cJSON_AddStringToObject(msg, "role", "user");
            cJSON_AddStringToObject(msg, "content", req->prompt);
            cJSON_AddItemToArray(messages, msg);
            break;
        }

        case PROVIDER_GEMINI: {
            if (strlen(ks->gemini_key) == 0) {
                fprintf(stderr, "[Error] Gemini Key error.\n");
                cJSON_Delete(root);
                curl_slist_free_all(headers);
                curl_easy_cleanup(curl);
                return;
            }

            snprintf(url, sizeof(url), GEMINI_URL, req->model, ks->gemini_key);

            cJSON *contents = cJSON_AddArrayToObject(root, "contents");
            cJSON *content_item = cJSON_CreateObject();
            cJSON *parts = cJSON_AddArrayToObject(content_item, "parts");
            cJSON *part_item = cJSON_CreateObject();
            cJSON_AddStringToObject(part_item, "text", req->prompt);
            cJSON_AddItemToArray(parts, part_item);
            cJSON_AddItemToArray(contents, content_item);
            break;
        }

        case PROVIDER_NONE:
            fprintf(stderr, "[Error] No AI provider selected.\n");
            cJSON_Delete(root);
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
            return;
    }

    char *json_payload = cJSON_PrintUnformatted(root);
    curl_buffer response = {0};

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_payload);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "Curl error: %s\n", curl_easy_strerror(res));
    } else {
        print_ai_response(req->provider, response.data);
    }

    free(response.data);
    free(json_payload);
    cJSON_Delete(root);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
}
