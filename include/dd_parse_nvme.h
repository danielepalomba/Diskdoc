#ifndef DD_PARSE_NVME_H
#define DD_PARSE_NVME_H

#include "cJSON.h"
#include "dd_report.h"

void build_nvme_report(cJSON *root, dd_report *report);

#endif
