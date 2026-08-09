#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <string.h>
#include <errno.h>

#include "diskdoc_core.h"

/* ANSI Colors */
#define COLOR_RED     "\x1b[31m"
#define COLOR_GREEN   "\x1b[32m"
#define COLOR_YELLOW  "\x1b[33m"
#define COLOR_RESET   "\x1b[0m"

int scan_physical_disks(dd_disk_list *list){
    list->count = 0;

    DIR *dir = opendir("/sys/block");
    if(dir == NULL){
        perror("Could not open /sys/block");
        return -1;
    }

    struct dirent *entry;
    char path[256];

    while((entry = readdir(dir)) != NULL){
        if(entry->d_name[0] == '.') continue; //ignore '.' and '..'
        if(strncmp(entry->d_name, "sr", 2) == 0) continue; //ignore optical drives

        int n = snprintf(path, sizeof(path), "/sys/block/%s/device", entry->d_name);
        if(n < 0 || (size_t)n >= sizeof(path)) continue; //truncate

        //check if path exists in order to know if entry is a physical device.
        if(access(path, F_OK) != 0) continue;

        if(strlen(entry->d_name) >= DD_NAME_LEN) continue; //name we cannot store

        if(list->count == DD_MAX_DISKS){
            fprintf(stderr, COLOR_RED "More than %d disks, ignoring the rest\n" COLOR_RESET,
                    DD_MAX_DISKS);
            break;
        }

        strcpy(list->disks[list->count].name, entry->d_name);
        list->count++;
    }

    closedir(dir);
    return 0;
}

void print_disk_list(const dd_disk_list *list){
    if(list->count == 0){
        puts(COLOR_RED "No physical disk found." COLOR_RESET);
        return;
    }

    for(size_t i = 0; i < list->count; i++)
        printf(COLOR_GREEN "[%zu]Device:" COLOR_RESET " /dev/%s\n", i, list->disks[i].name);
}

int prompt_disk_choice(const dd_disk_list *list){
    char line[64];

    if(list->count == 0) return -1;

    for(;;){
        printf("Select a disk [0-%zu], or q to quit: ", list->count - 1);
        fflush(stdout);

        if(fgets(line, sizeof(line), stdin) == NULL){
            putchar('\n');
            return -1; //EOF, the user pressed ctrl-D
        }

        //if the line did not fit, drop the rest so it is not read as an answer
        if(strchr(line, '\n') == NULL){
            int c;
            while((c = getchar()) != '\n' && c != EOF)
                ;
        }

        if(line[0] == 'q' || line[0] == 'Q') return -1;

        char *end;
        errno = 0;
        long choice = strtol(line, &end, 10);

        if(end == line){ //no digits at all
            puts(COLOR_RED "Invalid choice, try again." COLOR_RESET);
            continue;
        }

        //only trailing blanks are allowed after the number
        while(*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r') end++;

        if(*end != '\0' || errno == ERANGE || choice < 0 || choice >= (long)list->count){
            puts(COLOR_RED "Invalid choice, try again." COLOR_RESET);
            continue;
        }

        return (int)choice;
    }
}

void analyze_disk(const char *dev_path){
    char command[256];

    snprintf(command, sizeof(command), "smartctl -a -j /dev/%s 2>/dev/null", dev_path);
    
    FILE *fp = popen(command, "r");
    if(fp == NULL){
        fprintf(stderr, COLOR_RED "Error while running smartctl on %s\n" COLOR_RESET, dev_path);
        return;
    }

    puts(COLOR_YELLOW "Analyzing..." COLOR_RESET);

    char buffer[1024];
    while (fgets(buffer, sizeof(buffer), fp) != NULL) {
        printf("%s", buffer);
    }

    int status = pclose(fp);

    if(status != 0)
        printf(COLOR_YELLOW "Warning: smartctl exited with a nonull status.\n" COLOR_RESET);
}
