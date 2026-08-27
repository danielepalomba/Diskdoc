#ifndef DD_SMARTCTL_H
#define DD_SMARTCTL_H

#include <stdbool.h>

int analyze_disk(const char *dev_path, bool print_report);
int start_self_test(const char *dev_path, const char *mode); // mode = "short" | "long" 

#endif
