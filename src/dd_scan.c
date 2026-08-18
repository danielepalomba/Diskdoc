#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <string.h>
#include <errno.h>

#include "dd_scan.h"
#include "dd_print.h"

/* Fills the list with the physical disks under /sys/block, skipping dotfiles,
   optical drives, and anything without a device node.
   Returns 0 on success, -1 when the directory cannot be read. */
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
        if(entry->d_name[0] == '.') continue;
        if(strncmp(entry->d_name, "sr", 2) == 0) continue;

        int n = snprintf(path, sizeof(path), "/sys/block/%s/device", entry->d_name);
        if(n < 0 || (size_t)n >= sizeof(path)) continue;

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

/* Prints the scanned disks, numbered so the user can pick one */
void print_disk_list(const dd_disk_list *list){
    if(list->count == 0){
        puts(COLOR_RED "No physical disk found." COLOR_RESET);
        return;
    }

    for(size_t i = 0; i < list->count; i++)
        printf(COLOR_GREEN "[%zu]Device:" COLOR_RESET " /dev/%s\n", i, list->disks[i].name);
}

/* Asks which disk to analyze until the answer is a valid index.
   Returns the chosen index, or -1 when the user quits. */
int prompt_disk_choice(const dd_disk_list *list){
    char line[64];

    if(list->count == 0) return -1;

    for(;;){
        printf("Select a disk [0-%zu], or q to quit: ", list->count - 1);
        fflush(stdout);

        if(fgets(line, sizeof(line), stdin) == NULL){
            putchar('\n');
            return -1;
        }

        if(strchr(line, '\n') == NULL){
            int c;
            while((c = getchar()) != '\n' && c != EOF)
                ;
        }

        if(line[0] == 'q' || line[0] == 'Q') return -1;

        char *end;
        errno = 0;
        long choice = strtol(line, &end, 10);

        if(end == line){
            puts(COLOR_RED "Invalid choice, try again." COLOR_RESET);
            continue;
        }

        while(*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r') end++;

        if(*end != '\0' || errno == ERANGE || choice < 0 || choice >= (long)list->count){
            puts(COLOR_RED "Invalid choice, try again." COLOR_RESET);
            continue;
        }

        return (int)choice;
    }
}
