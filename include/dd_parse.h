#ifndef DD_PARSE_H
#define DD_PARSE_H

#include "cJSON.h"
#include "dd_report.h"

void build_disk_report(cJSON *root, dd_report *report);

#endif
