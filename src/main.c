#include <stdio.h>

#include "diskdoc_core.h"

/* ANSI Colors */
#define COLOR_YELLOW  "\x1b[33m"
#define COLOR_RESET   "\x1b[0m"

int main(void){
    dd_disk_list disks;

    puts(COLOR_YELLOW "Scanning physical disks..." COLOR_RESET);

    if(scan_physical_disks(&disks) != 0) return 1;

    print_disk_list(&disks);

    int choice = prompt_disk_choice(&disks);
    if(choice < 0){
        puts("No disk selected.");
        return 0;
    }

    printf("Selected disk: /dev/%s\n", disks.disks[choice].name);
    return 0;
}
