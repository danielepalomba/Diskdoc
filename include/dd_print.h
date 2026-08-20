#ifndef DD_PRINT_H
#define DD_PRINT_H

#include "dd_report.h"

/* ANSI Colors */
#define COLOR_RED     "\x1b[31m"
#define COLOR_GREEN   "\x1b[32m"
#define COLOR_YELLOW  "\x1b[33m"
#define COLOR_DIM     "\x1b[2m"
#define COLOR_RESET   "\x1b[0m"

/* Fixed width label, so the values line up in a column */
#define DD_FIELD "  %-24s"

void print_section(const char *title);
void print_report_text(const dd_report *report);
const char *severity_to_str(dd_severity severity);
const char *severity_color(dd_severity severity);

#endif
