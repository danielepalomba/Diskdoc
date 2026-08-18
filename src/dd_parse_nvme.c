#include <stdio.h>

#include "cJSON.h"
#include "dd_parse_nvme.h"
#include "dd_report.h"

/* NVMe data unit is 1000 sectors of 512 bytes */
#define DD_NVME_DATA_UNIT 512000.0

/* Decodes the NVMe critical warning bitmask: one field for the mask itself,
   then a note for every alarm the disk raised. */
static void add_critical_warning(dd_report *report, unsigned warning){
    static const char *const alarms[] = {
        "available spare is below the threshold",
        "temperature is outside the critical thresholds",
        "NVM subsystem reliability is degraded",
        "media has been placed in read only mode",
        "volatile memory backup device has failed",
        "persistent memory region is read only or unreliable",
    };

    if(warning == 0){
        dd_add_text(report, DD_SECTION_WEAR, "critical_warning",
                    "Critical warnings", "none");
        return;
    }

    dd_flag(dd_add_text(report, DD_SECTION_WEAR, "critical_warning",
                        "Critical warnings", "0x%02x", warning), DD_ALARM);

    for(size_t i = 0; i < sizeof alarms / sizeof alarms[0]; i++)
        if(warning & (1u << i))
            dd_add_note(report, DD_SECTION_WEAR, DD_ALARM, "- %s", alarms[i]);

    if(warning & 0xC0)
        dd_add_note(report, DD_SECTION_WEAR, DD_ALARM,
                    "- unknown flag set by the device");
}

/* Builds the wear and usage report from the NVMe SMART health log. */
void build_nvme_report(cJSON *root, dd_report *report){
    cJSON *log = cJSON_GetObjectItemCaseSensitive(root, "nvme_smart_health_information_log");
    if(log == NULL) return;

    cJSON *c_warning = cJSON_GetObjectItemCaseSensitive(log, "critical_warning");
    if(cJSON_IsNumber(c_warning))
        add_critical_warning(report, (unsigned)cJSON_GetNumberValue(c_warning));

    cJSON *percentage_used = cJSON_GetObjectItemCaseSensitive(log, "percentage_used");
    if(cJSON_IsNumber(percentage_used))
        dd_add_number(report, DD_SECTION_WEAR, "life_usage", "Life usage",
                      DD_PERCENT, cJSON_GetNumberValue(percentage_used));

    cJSON *available_spare = cJSON_GetObjectItemCaseSensitive(log, "available_spare");
    cJSON *available_spare_thr = cJSON_GetObjectItemCaseSensitive(log, "available_spare_threshold");
    if(cJSON_IsNumber(available_spare) && cJSON_IsNumber(available_spare_thr)){
        double spare = cJSON_GetNumberValue(available_spare);
        double threshold = cJSON_GetNumberValue(available_spare_thr);

        dd_flag_note(dd_add_number(report, DD_SECTION_WEAR, "available_spare",
                                   "Available spare", DD_PERCENT, spare),
                     spare < threshold ? DD_ALARM : DD_OK,
                     "threshold %.0f%%", threshold);

        if(spare < threshold)
            dd_add_note(report, DD_SECTION_WEAR, DD_ALARM,
                        "the disk has run out of replacement blocks");
    }

    cJSON *media_errors = cJSON_GetObjectItemCaseSensitive(log, "media_errors");
    if(cJSON_IsNumber(media_errors)){
        double errors = cJSON_GetNumberValue(media_errors);

        dd_flag(dd_add_number(report, DD_SECTION_WEAR, "media_errors",
                              "Media errors", DD_COUNT, errors),
                errors != 0 ? DD_WATCH : DD_OK);
    }

    cJSON *num_err_log = cJSON_GetObjectItemCaseSensitive(log, "num_err_log_entries");
    if(cJSON_IsNumber(num_err_log))
        dd_add_number(report, DD_SECTION_WEAR, "error_log_entries",
                      "Error log entries", DD_COUNT, cJSON_GetNumberValue(num_err_log));

    cJSON *p_on_hours = cJSON_GetObjectItemCaseSensitive(log, "power_on_hours");
    if(cJSON_IsNumber(p_on_hours))
        dd_add_number(report, DD_SECTION_USAGE, "power_on_hours", "Power on hours",
                      DD_HOURS, cJSON_GetNumberValue(p_on_hours));

    cJSON *p_cycles = cJSON_GetObjectItemCaseSensitive(log, "power_cycles");
    if(cJSON_IsNumber(p_cycles))
        dd_add_number(report, DD_SECTION_USAGE, "power_cycles", "Power cycles",
                      DD_COUNT, cJSON_GetNumberValue(p_cycles));

    cJSON *units_written = cJSON_GetObjectItemCaseSensitive(log, "data_units_written");
    if(cJSON_IsNumber(units_written))
        dd_add_number(report, DD_SECTION_USAGE, "data_written", "Data written",
                      DD_BYTES, cJSON_GetNumberValue(units_written) * DD_NVME_DATA_UNIT);

    cJSON *units_read = cJSON_GetObjectItemCaseSensitive(log, "data_units_read");
    if(cJSON_IsNumber(units_read))
        dd_add_number(report, DD_SECTION_USAGE, "data_read", "Data read",
                      DD_BYTES, cJSON_GetNumberValue(units_read) * DD_NVME_DATA_UNIT);
}
