#include <stdio.h>
#include <stdlib.h>

#include "dd_scan.h"
#include "dd_smartctl.h"
#include "dd_print.h"

/* Discards whatever is left on the current input line. */
static void clean_stdin_buff(){
    int c;
    while ((c = getchar()) != '\n' && c != EOF);
}

static void clean_screen(){
    printf("\x1b[2J\x1b[H");
    fflush(stdout);
}

/* Scans disks, lets the user pick one to analyze, and repeats until they quit. */
int main(void){
 
    char r;
    
    do{
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
        analyze_disk(disks.disks[choice].name);

        printf("\nContinue? (y/n)");
        r = fgetc(stdin);
        
        clean_stdin_buff();

    }while(r != 'n');

    return 0;
}
