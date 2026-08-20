#ifndef DD_SMARTCTL_H
#define DD_SMARTCTL_H

#include <stdbool.h>

int analyze_disk(const char *dev_path, bool print_report);

#endif
