#ifndef DD_PRINT_H
#define DD_PRINT_H

#include <stdio.h>

#include "dd_report.h"

typedef enum {
    DD_C_RED,
    DD_C_GREEN,
    DD_C_YELLOW,
    DD_C_DIM,
    DD_C_RESET
} dd_color_id;

void dd_color_init(void);
const char *dd_color(FILE *stream, dd_color_id color);

/* Fixed width label, so the values line up in a column */
#define DD_FIELD "  %-24s"

void print_section(const char *title);
void print_report_text(const dd_report *report);
const char *severity_to_str(dd_severity severity);
const char *severity_color(dd_severity severity);

#endif
