#include <stdio.h>
#include <string.h>

#include "dd_print.h"

/* Prints a title followed by a matching underline. */
void print_section(const char *title){
    printf("\n" COLOR_GREEN "%s" COLOR_RESET "\n", title);

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
        case DD_GOOD:   return COLOR_GREEN;
        case DD_OK:     return "";
        case DD_ABSENT: return COLOR_DIM;
        case DD_WATCH:  return COLOR_YELLOW;
        case DD_ALARM:  return COLOR_RED;
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
        const char *reset = (*color != '\0') ? COLOR_RESET : "";

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
            printf(DD_FIELD COLOR_DIM "not reported" COLOR_RESET "\n", field->label);
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
