#ifndef DD_REPORT_H
#define DD_REPORT_H

#include <stddef.h>

#define DD_MAX_FIELDS 64

#if defined(__GNUC__)
# define DD_PRINTF(fmt_pos, first_arg) \
       __attribute__((format(printf, fmt_pos, first_arg)))
#else
#  define DD_PRINTF(fmt_pos, first_arg)
#endif

/* Ordered from sound to worrying, because dd_report_worst compares them as
   numbers. DD_OK has to stay zero, so that a field nobody flagged starts out
   sound.

   DD_GOOD is for the values stating the disk checked itself and found nothing
   wrong, which is not the same as a value that simply has no reason to worry
   anybody. */
typedef enum {
    DD_GOOD = -1, DD_OK = 0, DD_ABSENT, DD_WATCH, DD_ALARM
} dd_severity;

typedef enum {
    DD_SECTION_DEVICE, DD_SECTION_HEALTH,
    DD_SECTION_WEAR,   DD_SECTION_USAGE
} dd_section;

/* How the value has to be rendered, not what it means. */
typedef enum {
    DD_TEXT,
    DD_COUNT,
    DD_BYTES,
    DD_HOURS,
    DD_PERCENT,
    DD_CELSIUS,
} dd_kind;

typedef struct {
    dd_section  section;
    const char *key;        // stable id for machine readable output, NULL on notes
    const char *label;      // human readable label, NULL on notes
    dd_kind     kind;
    double      number;     // valid if kind != DD_TEXT
    char        text[128];  // valid if kind == DD_TEXT
    dd_severity severity;
    char        note[96];   // optional, printed under the value
} dd_field;

/* A report is ready to use once zeroed: dd_report report = {0}; */
typedef struct {
    dd_field fields[DD_MAX_FIELDS];
    size_t   count;
} dd_report;

dd_field *dd_add_number(dd_report *report, dd_section section, const char *key,
                        const char *label, dd_kind kind, double number);

dd_field *dd_add_text(dd_report *report, dd_section section, const char *key,
                      const char *label, const char *fmt, ...) DD_PRINTF(5, 6);

dd_field *dd_add_absent(dd_report *report, dd_section section,
                        const char *key, const char *label);

dd_field *dd_add_note(dd_report *report, dd_section section,
                      dd_severity severity, const char *fmt, ...) DD_PRINTF(4, 5);

void dd_flag(dd_field *field, dd_severity severity);

void dd_flag_note(dd_field *field, dd_severity severity,
                  const char *fmt, ...) DD_PRINTF(3, 4);

dd_severity dd_report_worst(const dd_report *report);

int dd_exit_code(dd_severity severity);

dd_severity dd_exit_code_to_severity(int exit_code);

const dd_field *dd_find(const dd_report *report, const char *key);

#endif
