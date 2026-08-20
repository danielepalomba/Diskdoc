#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "dd_report.h"

/* Takes the next free slot, NULL when the report is full */
static dd_field *next_field(dd_report *report){
    if(report->count == DD_MAX_FIELDS) return NULL;

    return &report->fields[report->count++];
}

/* Appends a numeric field to the report, NULL when it is full. */
dd_field *dd_add_number(dd_report *report, dd_section section, const char *key,
                        const char *label, dd_kind kind, double number){
    dd_field *field = next_field(report);
    if(field == NULL) return NULL;

    *field = (dd_field){
        .section = section,
        .key     = key,
        .label   = label,
        .kind    = kind,
        .number  = number,
    };

    return field;
}

/* Appends a formatted text field to the report, NULL when it is full. */
dd_field *dd_add_text(dd_report *report, dd_section section, const char *key,
                      const char *label, const char *fmt, ...){
    dd_field *field = next_field(report);
    if(field == NULL) return NULL;

    *field = (dd_field){
        .section = section,
        .key     = key,
        .label   = label,
        .kind    = DD_TEXT,
    };

    va_list args;
    va_start(args, fmt);
    vsnprintf(field->text, sizeof field->text, fmt, args);
    va_end(args);

    return field;
}

/* Appends a field marked as not reported by the disk. */
dd_field *dd_add_absent(dd_report *report, dd_section section, const char *key,
                        const char *label){
    dd_field *field = next_field(report);
    if(field == NULL) return NULL;

    *field = (dd_field){
        .section  = section,
        .key      = key,
        .label    = label,
        .kind     = DD_TEXT,
        .severity = DD_ABSENT,
    };

    return field;
}

/* Appends a note: a field with no key or label, only severity and text. */
dd_field *dd_add_note(dd_report *report, dd_section section,
                      dd_severity severity, const char *fmt, ...){
    dd_field *field = next_field(report);
    if(field == NULL) return NULL;

    *field = (dd_field){
        .section  = section,
        .kind     = DD_TEXT,
        .severity = severity,
    };

    va_list args;
    va_start(args, fmt);
    vsnprintf(field->text, sizeof field->text, fmt, args);
    va_end(args);

    return field;
}

/* Sets the severity of an existing field. */
void dd_flag(dd_field *field, dd_severity severity){
    if(field == NULL) return;

    field->severity = severity;
}

/* Sets severity and attaches a formatted note to an existing field. */
void dd_flag_note(dd_field *field, dd_severity severity, const char *fmt, ...){
    if(field == NULL) return;

    field->severity = severity;

    va_list args;
    va_start(args, fmt);
    vsnprintf(field->note, sizeof field->note, fmt, args);
    va_end(args);
}

/* Returns the highest (most concerning) severity in the report. */
dd_severity dd_report_worst(const dd_report *report){
    dd_severity worst = DD_OK;

    for(size_t i = 0; i < report->count; i++)
        if(report->fields[i].severity > worst)
            worst = report->fields[i].severity;

    return worst;
}

/* Collapses a severity into a 3-state exit code for cron/monitoring use. */
int dd_exit_code(dd_severity severity){
    if(severity >= DD_ALARM) return 2;
    if(severity >= DD_WATCH) return 1;
    return 0;
}

/* Convert an exit_code to a severity */
dd_severity dd_exit_code_to_severity(int exit_code){
    if(exit_code == -1) return DD_GOOD;
    if(exit_code == 1 || exit_code == 2) return DD_WATCH;
    if(exit_code == 3) return DD_ALARM;
    return DD_OK;
}

/* Looks up a field by key, NULL if none matches (notes carry no key). */
const dd_field *dd_find(const dd_report *report, const char *key){
    for(size_t i = 0; i < report->count; i++){
        const dd_field *field = &report->fields[i];

        if(field->key != NULL && strcmp(field->key, key) == 0) return field;
    }

    return NULL;
}
