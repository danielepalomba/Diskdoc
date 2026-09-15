#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dd_print.h"

static bool color_out, color_err;

/* Decides once whether stdout and stderr get colors: only when they are
   terminals and NO_COLOR is unset. (https://no-color.org) */
void dd_color_init(void){ 
    const char *no_color = getenv("NO_COLOR");
    bool enabled = (no_color == NULL || no_color[0] == '\0');
    color_out = enabled && isatty(STDOUT_FILENO);
    color_err = enabled && isatty(STDERR_FILENO);
}

/* The escape sequence for a color on the given stream, or "" when that
   stream has colors disabled, so callers never need to check themselves. */
const char *dd_color(FILE *stream, dd_color_id color){
    bool on = (stream == stderr) ? color_err : color_out;
    if(!on) return "";

    switch(color){
        case DD_C_RED:    return "\x1b[31m";
        case DD_C_GREEN:  return "\x1b[32m";
        case DD_C_YELLOW: return "\x1b[33m";
        case DD_C_DIM:    return "\x1b[2m";
        case DD_C_RESET:  return "\x1b[0m";
    }
    return "";
}

/* Prints a title followed by a matching underline. */
void print_section(const char *title){
    printf("\n%s%s%s\n", dd_color(stdout, DD_C_GREEN), title, dd_color(stdout, DD_C_RESET));

    for(size_t i = strlen(title); i > 0; i--) putchar('-');
    putchar('\n');
}

/* Formats a byte count using whichever unit (MB/GB/TB) keeps it readable. */
static void format_bytes(char *out, size_t size, double bytes){
    if(bytes >= 1e12)
        snprintf(out, size, "%.2f TB", bytes / 1e12);
    else if(bytes >= 1e9)
        snprintf(out, size, "%.2f GB", bytes / 1e9);
    else
        snprintf(out, size, "%.2f MB", bytes / 1e6);
}

/* Maps a section enum to its display title. */
static const char *section_title(dd_section section){
    switch(section){
        case DD_SECTION_DEVICE: return "Device";
        case DD_SECTION_HEALTH: return "Health";
        case DD_SECTION_WEAR:   return "Wear and reliability";
        case DD_SECTION_USAGE:  return "Usage";
    }
    return "";
}

/* The colour says how worrying the value is, never which field it belongs to */
const char *severity_color(dd_severity severity){
    switch(severity){
        case DD_GOOD:   return dd_color(stdout, DD_C_GREEN);
        case DD_OK:     return "";
        case DD_ABSENT: return dd_color(stdout, DD_C_DIM);
        case DD_WATCH:  return dd_color(stdout, DD_C_YELLOW);
        case DD_ALARM:  return dd_color(stdout, DD_C_RED);
    }
    return "";
}

/* Maps a severity to the label shown next to a disk in summary output. */
const char *severity_to_str(dd_severity severity){
    switch(severity){
        case DD_GOOD:
            return "GOOD";
        case DD_OK:
            return "OK";
        case DD_ABSENT:
        case DD_WATCH:
            return "WARNING";
        case DD_ALARM:
            return "ALARM";
    }
    return "";
}

/* Renders the value in the unit its kind calls for */
static void format_value(const dd_field *field, char *out, size_t size){
    switch(field->kind){
        case DD_TEXT:
            snprintf(out, size, "%s", field->text);
            break;
        case DD_COUNT:
            snprintf(out, size, "%.0f", field->number);
            break;
        case DD_BYTES:
            format_bytes(out, size, field->number);
            break;
        case DD_HOURS:
            snprintf(out, size, "%.0f h (%.1f years)",
                     field->number, field->number / (24 * 365.0));
            break;
        case DD_PERCENT:
            snprintf(out, size, "%.0f%%", field->number);
            break;
        case DD_CELSIUS:
            snprintf(out, size, "%.0f °C", field->number);
            break;
    }
}

/* Prints every field of the report, grouped by section and colored by severity. */
void print_report_text(const dd_report *report){
    int started = 0;
    dd_section current = DD_SECTION_DEVICE;

    for(size_t i = 0; i < report->count; i++){
        const dd_field *field = &report->fields[i];
        const char *color = severity_color(field->severity);
        const char *reset = dd_color(stdout, DD_C_RESET);

        if(!started || field->section != current){
            print_section(section_title(field->section));
            current = field->section;
            started = 1;
        }

        if(field->label == NULL){
            printf(DD_FIELD "%s%s%s\n", "", color, field->text, reset);
            continue;
        }

        if(field->severity == DD_ABSENT){
            printf(DD_FIELD "%s%s%s\n", field->label, color, "not reported", reset);
            continue;
        }

        char value[160];
        format_value(field, value, sizeof value);

        if(field->note[0] == '\0')
            printf(DD_FIELD "%s%s%s\n", field->label, color, value, reset);
        else
            printf(DD_FIELD "%s%s (%s)%s\n",
                   field->label, color, value, field->note, reset);
    }
}
