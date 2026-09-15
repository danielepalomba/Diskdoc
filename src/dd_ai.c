#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <curl/curl.h>
#include <cJSON.h>

#include "dd_ai.h"
#include "dd_utils.h"

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

/* Builds and performs the request for the selected provider. The key only
   ever lives in the local header buffer, which is wiped on every exit path,
   and redirects are refused so an Authorization header can never be replayed
   against another host. */
void send_ai_prompt(const char *api_key, const ai_request *req){
    char auth_header[DD_AUTH_HEADER_MAX] = {0};
    char url[512] = {0};
    struct curl_slist *headers = NULL;
    cJSON *root = NULL;
    char *json_payload = NULL;
    curl_buffer response = {0};
    CURL *curl = NULL;

    if(api_key == NULL || api_key[0] == '\0'){
        fprintf(stderr, "[Error] No API key available for the selected provider.\n");
        return;
    }

    curl = curl_easy_init();
    if(!curl) return;

    headers = curl_slist_append(headers, "Content-Type: application/json");
    root = cJSON_CreateObject();

    switch(req->provider){
        case PROVIDER_OPENAI: {
            snprintf(url, sizeof(url), OPENAI_URL);
            snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", api_key);
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
            snprintf(url, sizeof(url), ANTHROPIC_URL);
            snprintf(auth_header, sizeof(auth_header), "x-api-key: %s", api_key);
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
            snprintf(url, sizeof(url), GEMINI_URL, req->model);
            snprintf(auth_header, sizeof(auth_header), "x-goog-api-key: %s", api_key);
            headers = curl_slist_append(headers, auth_header);

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
        default:
            fprintf(stderr, "[Error] No AI provider selected.\n");
            goto cleanup;
    }

    json_payload = cJSON_PrintUnformatted(root);
    if(json_payload == NULL) goto cleanup;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_payload);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, DD_HTTP_TIMEOUT_SECONDS);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "Curl error: %s\n", curl_easy_strerror(res));
    } else {
        print_ai_response(req->provider, response.data);
    }

cleanup:
    dd_wipe(auth_header, sizeof auth_header);

    free(response.data);
    free(json_payload);
    cJSON_Delete(root);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
}
