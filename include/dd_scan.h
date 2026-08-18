#ifndef DD_SCAN_H
#define DD_SCAN_H

#include <stddef.h>

#define DD_MAX_DISKS 64
#define DD_NAME_LEN  32

typedef struct {
    char name[DD_NAME_LEN]; //kernel name
} dd_disk;

typedef struct {
    dd_disk disks[DD_MAX_DISKS];
    size_t  count;
} dd_disk_list;

int scan_physical_disks(dd_disk_list *list);
void print_disk_list(const dd_disk_list *list);
int prompt_disk_choice(const dd_disk_list *list);

#endif
