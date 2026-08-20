#include <stdio.h>
#include <stdlib.h>

#include "dd_scan.h"
#include "dd_smartctl.h"
#include "dd_print.h"

static void clean_screen(){
    printf("\x1b[2J\x1b[H");
    fflush(stdout);
}

int main(int argc, char **argv){
    //clean_screen();
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
    return analyze_disk(disks.disks[choice].name);
}
