#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <getopt.h>

#include "dd_scan.h"
#include "dd_smartctl.h"
#include "dd_print.h"

#define USAGE_MSG "Usage: diskdoc [-h] [-a] [-q] [-d <device>]\n" \
                  "\n" \
                  "  -h, --help            show this help message and exit\n" \
                  "  -a, --all             analyze every detected physical disk\n" \
                  "  -q, --quiet           only print the summary line per disk, skip the detailed report\n" \
                  "  -d, --device <name>   analyze a single device by kernel name (e.g. sda)\n" \
                  "\n" \
                  "With no options, diskdoc scans the disks and lets you pick one interactively.\n"

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

static int analyze_all(bool print){
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

static int analyze_device(char *dev, bool print){
    const char *name = dev;
    dd_disk_list disks;

    if(name == NULL){
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

int main(int argc, char **argv){
    
    clean_screen();
    
    int opt;
    int a_flag = 0, q_flag = 0;
    char *device_name = NULL;

    static struct option long_options[] = {
        {"help", no_argument, 0, 'h'},
        {"all", no_argument, 0, 'a'},
        {"quiet", no_argument, 0, 'q'},
        {"device", required_argument, 0, 'd'},
        {0, 0, 0, 0}
    };

    int option_index = 0;
    while((opt = getopt_long(argc, argv, "haqd:", long_options, &option_index)) != -1){
        switch(opt){
            case 'h':
                printf(USAGE_MSG);
                return 0;
            case 'a':
                a_flag = 1;
                break;
            case 'q':
                q_flag = 1;
                break;
            case 'd':
                device_name = optarg;
                break;
        }
    }

    bool print = !q_flag;

    if(a_flag) return analyze_all(print);
    return analyze_device(device_name, print);
}
