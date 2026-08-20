#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>

#include "cJSON.h"
#include "dd_smartctl.h"
#include "dd_parse.h"
#include "dd_print.h"
#include "dd_report.h"

/* smartctl reports its outcome as a bitmask.

   Bits 0-2 mean smartctl itself could not do its job, so there is no report to read.

   Bits 3-7 mean the report is valid and it is the disk that has
   something to report: those are results, not errors.
*/
#define DD_SMARTCTL_FAILED   0x03
#define DD_SMARTCTL_PARTIAL  0x04
#define DD_SMARTCTL_FINDINGS 0xF8
#define DD_SMARTCTL_OPEN_FAILED 0x02

/* Reads a whole pipe into a buffer that grows as needed.
   Returns a string the caller has to free, or NULL on error. */
static char* read_smartctl_output(FILE *fp){
    size_t capacity = 16384;
    size_t length   = 0;

    char *buffer = malloc(capacity);
    if(buffer == NULL) return NULL;

    for(;;){
        if(length == capacity - 1){
            size_t new_capacity = capacity * 2;
            char *tmp = realloc(buffer, new_capacity);
            if(tmp == NULL){
                free(buffer);
                return NULL;
            }
            buffer   = tmp;
            capacity = new_capacity;
        }

        size_t n = fread(buffer + length, 1, capacity - 1 - length, fp);
        length += n;

        if(n == 0){
            if(ferror(fp)){
                free(buffer);
                return NULL;
            }
            break;
        }
    }

    buffer[length] = '\0';
    return buffer;
}

/* Prints the messages smartctl attached to its report, in the given color */
static void print_smartctl_messages(cJSON *smartctl, FILE *stream, const char *color){
    cJSON *messages = cJSON_GetObjectItemCaseSensitive(smartctl, "messages");
    if(!cJSON_IsArray(messages)) return;

    cJSON *message;
    cJSON_ArrayForEach(message, messages){
        cJSON *string = cJSON_GetObjectItemCaseSensitive(message, "string");
        if(cJSON_IsString(string))
            fprintf(stream, "%s  %s\n" COLOR_RESET, color, string->valuestring);
    }
}

/* Decodes the smartctl exit bitmask and prints what it reported: fatal
   errors on stderr, partial reads and findings on stdout.
   Returns 1 when the report can be trusted, 0 when there is no report at all. */
static int check_smartctl_status(cJSON *root){
    static const char *const findings[] = {
        "SMART status reports the disk is FAILING",
        "some prefail attributes are below the threshold",
        "some attributes were below the threshold in the past",
        "the device error log contains records",
        "the self test log contains errors",
    };

    cJSON *smartctl = cJSON_GetObjectItemCaseSensitive(root, "smartctl");
    if(smartctl == NULL) return 1;

    cJSON *exit_status = cJSON_GetObjectItemCaseSensitive(smartctl, "exit_status");
    if(!cJSON_IsNumber(exit_status)) return 1;

    unsigned status = (unsigned)cJSON_GetNumberValue(exit_status);

    if(status & DD_SMARTCTL_FAILED){
        fprintf(stderr, COLOR_RED "smartctl could not read this device (0x%02x):\n" COLOR_RESET,
                status);
        print_smartctl_messages(smartctl, stderr, COLOR_RED);

        if(status & DD_SMARTCTL_OPEN_FAILED)
            fprintf(stderr, COLOR_YELLOW
                    "Reading SMART data usually requires root, try again with sudo.\n"
                    COLOR_RESET);
        return 0;
    }

    if(status & DD_SMARTCTL_PARTIAL){
        printf(COLOR_YELLOW "Some data could not be read (0x%02x):\n" COLOR_RESET, status);
        print_smartctl_messages(smartctl, stdout, COLOR_YELLOW);
    }

    if(status & DD_SMARTCTL_FINDINGS){
        printf(COLOR_YELLOW "Findings reported by smartctl (0x%02x):\n" COLOR_RESET, status);

        for(size_t i = 0; i < sizeof findings / sizeof findings[0]; i++)
            if(status & (1u << (i + 3)))
                printf(COLOR_YELLOW "  - %s\n" COLOR_RESET, findings[i]);
    }

    return 1;
}

/* Runs smartctl on dev_path, parses its JSON output, and prints the disk report. */
int analyze_disk(const char *dev_path, bool print_report){
    char command[256];
    int status = 0;

    snprintf(command, sizeof(command), "smartctl -x -j /dev/%s 2>/dev/null", dev_path);

    FILE *fp = popen(command, "r");
    if(fp == NULL){
        fprintf(stderr, COLOR_RED "Error while running smartctl on %s\n" COLOR_RESET, dev_path);
        return dd_exit_code(DD_ALARM);
    }

    printf(COLOR_YELLOW "Analyzing /dev/%s..." COLOR_RESET "\n", dev_path);

    char *data = read_smartctl_output(fp);

    status = pclose(fp);

    if(data == NULL){
        fprintf(stderr, COLOR_RED "Could not read smartctl output for %s\n" COLOR_RESET, dev_path);
        return dd_exit_code(DD_ALARM);
    }

    if(status == -1)
        perror("pclose");
    else if(WIFSIGNALED(status))
        fprintf(stderr, COLOR_RED "smartctl was killed by signal %d\n" COLOR_RESET,
                WTERMSIG(status));
 
    cJSON *root = cJSON_Parse(data);
    free(data);

    if(root == NULL) return dd_exit_code(DD_ALARM);

    int exit_code = dd_exit_code(DD_ALARM);

    if(check_smartctl_status(root)){
        dd_report report = {0};
        build_disk_report(root, &report);
        if(print_report) print_report_text(&report);
        exit_code = dd_exit_code(dd_report_worst(&report));
    }

    cJSON_Delete(root);
    return exit_code;
}
