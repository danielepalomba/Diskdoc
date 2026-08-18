#ifndef DD_PARSE_ATA_H
#define DD_PARSE_ATA_H

#include "cJSON.h"
#include "dd_report.h"

void build_ata_ssd_report(cJSON *root, dd_report *report);
void build_ata_hdd_report(cJSON *root, dd_report *report);

#endif
