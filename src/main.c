#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <getopt.h>
#include <curl/curl.h>

#include "dd_ai.h"
#include "dd_scan.h"
#include "dd_smartctl.h"
#include "dd_print.h"
#include "dd_report.h"
#include "dd_utils.h"

#define ART_DISKDOC \
    " ▄▄▄▄▄        ██                ▄▄              ▄▄                     \n" \
    " ██▀▀▀██      ▀▀                ██              ██                     \n" \
    " ██    ██   ████     ▄▄█████▄  ██ ▄██▀    ▄███▄██   ▄████▄    ▄█████▄ \n" \
    " ██    ██     ██     ██▄▄▄▄ ▀  ██▄██     ██▀  ▀██  ██▀  ▀██  ██▀    ▀ \n" \
    " ██    ██     ██      ▀▀▀▀██▄  ██▀██▄    ██    ██  ██    ██  ██       \n" \
    " ██▄▄▄██   ▄▄▄██▄▄▄  █▄▄▄▄▄██  ██  ▀█▄   ▀██▄▄███  ▀██▄▄██▀  ▀██▄▄▄▄█ \n" \
    " ▀▀▀▀▀     ▀▀▀▀▀▀▀▀   ▀▀▀▀▀▀   ▀▀   ▀▀▀    ▀▀▀ ▀▀    ▀▀▀▀      ▀▀▀▀▀  \n"

#define USAGE_MSG "Usage: diskdoc [-h] [-a] [-q] [-i] [-d <device>]\n" \
                  "       diskdoc -t <short|long> <device>\n" \
                  "\n" \
                  "  -h, --help            show this help message and exit\n" \
                  "  -a, --all             analyze every detected physical disk\n" \
                  "  -q, --quiet           only print the summary line per disk, skip the detailed report\n" \
                  "  -i, --ai              analyze a device with an AI model, using the API key set in .env\n" \
                  "  -d, --device <name>   analyze a single device by kernel name (e.g. sda)\n" \
                  "  -t, --test <mode>     start a short or long self-test on <device> (e.g. -t short nvme0)\n" \
                  "\n" \
                  "With no options, diskdoc scans the disks and lets you pick one interactively.\n" \
                  "-t cannot be combined with any other option.\n"

static void clean_screen(){
    printf("\x1b[2J\x1b[H");
    fflush(stdout);
}

static int check_anomaly(int *values, size_t size){
    int max = -999;
    for(size_t i = 0; i < size; i++){
        if(values[i] > max)
            max = values[i];
    }
    return max;
}

static void print_summary_row(const char *label, dd_severity severity){
    const char *color = severity_color(severity);
    const char *reset = (*color != '\0') ? COLOR_RESET : "";

    printf(DD_FIELD "%s%s%s\n", label, color, severity_to_str(severity), reset);
}

static int cmd_analyze_all(bool print){
    dd_disk_list disks;

    puts(COLOR_YELLOW "Scanning physical disks..." COLOR_RESET);
    if(scan_physical_disks(&disks)) return dd_exit_code(DD_ALARM);

    int values[disks.count];
    for(size_t i = 0; i < disks.count; i++){
        values[i] = analyze_disk(disks.disks[i].name, print);
    }

    print_section("Analysis report");
    for(size_t j = 0; j < disks.count; j++){
        char label[DD_NAME_LEN + 8];
        snprintf(label, sizeof label, "/dev/%s", disks.disks[j].name);
        print_summary_row(label, dd_exit_code_to_severity(values[j]));
    }
    putchar('\n');

    int worst = check_anomaly(values, disks.count);
    print_summary_row("Overall status", dd_exit_code_to_severity(worst));
    putchar('\n');

    return worst;
}

static int cmd_analyze_device(char *dev, bool print){
    const char *name = dev;
    dd_disk_list disks;

    if(name == NULL){
        clean_screen();
        puts(COLOR_YELLOW "Scanning physical disks..." COLOR_RESET);

        if(scan_physical_disks(&disks) != 0) return dd_exit_code(DD_ALARM);

        print_disk_list(&disks);

        int choice = prompt_disk_choice(&disks);
        if(choice < 0){
            puts("No disk selected.");
            return 0;
        }

        name = disks.disks[choice].name;
        printf("Selected disk: /dev/%s\n", name);
    }

    int exit_code = analyze_disk(name, print);

    char label[DD_NAME_LEN + 8];
    snprintf(label, sizeof label, "/dev/%s", name);
    print_summary_row(label, dd_exit_code_to_severity(exit_code));

    return exit_code;
}

static int cmd_ai_analyze_device(char *dev){
    const char *name = dev;
    dd_disk_list disks;

    if(name == NULL){
        clean_screen();
        puts(COLOR_YELLOW "Scanning physical disks..." COLOR_RESET);

        if(scan_physical_disks(&disks) != 0) return dd_exit_code(DD_ALARM);

        print_disk_list(&disks);

        int choice = prompt_disk_choice(&disks);
        if(choice < 0){
            puts("No disk selected.");
            return 0;
        }

        name = disks.disks[choice].name;
        printf("Selected disk: /dev/%s\n", name);
    }

    key_store ks;
    ai_provider provider = load_keys(&ks);
    if(provider == PROVIDER_NONE){
        fprintf(stderr, "[Error] No API key found in .env\n");
        return DD_EXIT_ERROR;
    }

    cJSON *payload = analyze_disk_raw(name);
    if(payload == NULL)
        return DD_EXIT_ERROR;

    char *json_str = cJSON_PrintUnformatted(payload);
    cJSON_Delete(payload);
    if(json_str == NULL)
        return DD_EXIT_ERROR;

    size_t prompt_len = strlen(SMARTCTL_FULL_PROMPT_FMT) + strlen(json_str) + 1;
    char *prompt = malloc(prompt_len);
    if(prompt == NULL){
        free(json_str);
        return DD_EXIT_ERROR;
    }
    snprintf(prompt, prompt_len, SMARTCTL_FULL_PROMPT_FMT, json_str);
    free(json_str);

    ai_request request = {
        .provider = provider,
        .model = default_model_for_provider(provider),
        .prompt = prompt
    };

    send_ai_prompt(&ks, &request);

    free(prompt);
    return 0;
}

static int cmd_self_test(const char *dev, const char *mode){
    if(strcmp(mode, "short") != 0 && strcmp(mode, "long") != 0){
        fprintf(stderr, "Invalid test mode '%s': expected 'short' or 'long'\n", mode);
        return DD_EXIT_ERROR;
    }

    printf(COLOR_YELLOW "Starting %s self-test on /dev/%s..." COLOR_RESET "\n", mode, dev);
    int exit_code = start_self_test(dev, mode);

    char label[DD_NAME_LEN + 8];
    snprintf(label, sizeof label, "/dev/%s", dev);
    print_summary_row(label, dd_exit_code_to_severity(exit_code));

    return exit_code;
}

int main(int argc, char **argv){
    
    int opt;
    int a_flag = 0, q_flag = 0, t_flag = 0, i_flag = 0, other_flag = 0;
    char *device_name = NULL;
    char *test_mode = NULL;

    curl_global_init(CURL_GLOBAL_DEFAULT);
    load_env_file(".env");

    static struct option long_options[] = {
        {"help", no_argument, 0, 'h'},
        {"all", no_argument, 0, 'a'},
        {"quiet", no_argument, 0, 'q'},
        {"ai", no_argument, 0, 'i'},
        {"device", required_argument, 0, 'd'},
        {"test", required_argument, 0, 't'},
        {0, 0, 0, 0}
    };

    int option_index = 0;
    while((opt = getopt_long(argc, argv, "haqid:t:", long_options, &option_index)) != -1){
        switch(opt){
            case 'h':
                printf("%s\n", ART_DISKDOC);
                puts(USAGE_MSG);
                return 0;
            case 'a':
                a_flag = 1;
                other_flag = 1;
                break;
            case 'q':
                q_flag = 1;
                other_flag = 1;
                break;
            case 'i':
                i_flag = 1;
                other_flag = 1;
                break;
            case 'd':
                device_name = optarg;
                other_flag = 1;
                break;
            case 't':
                test_mode = optarg;
                t_flag = 1;
                break;
        }
    }

    bool print = !q_flag;

    if(t_flag && other_flag){
        fprintf(stderr, "-t/--test can't be combined with other options\n");
        return DD_EXIT_ERROR;
    }

    if(t_flag){
        if(optind >= argc){
            fprintf(stderr, "Missing device name for -t/--test (e.g. diskdoc -t short nvme0)\n");
            return DD_EXIT_ERROR;
        }
        return cmd_self_test(argv[optind], test_mode);
    }

    if(i_flag) return cmd_ai_analyze_device(device_name);
    if(a_flag) return cmd_analyze_all(print);
    return cmd_analyze_device(device_name, print);
}
