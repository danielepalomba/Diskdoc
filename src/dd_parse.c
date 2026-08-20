#include <string.h>

#include "cJSON.h"
#include "dd_parse.h"
#include "dd_parse_nvme.h"
#include "dd_parse_ata.h"
#include "dd_report.h"

/* Adds the fields every disk reports regardless of protocol; fields the
   disk doesn't provide are simply omitted. */
static void build_common_report(cJSON *root, dd_report *report){
    cJSON *model_name = cJSON_GetObjectItemCaseSensitive(root, "model_name");
    if(cJSON_IsString(model_name))
        dd_add_text(report, DD_SECTION_DEVICE, "model", "Model",
                    "%s", model_name->valuestring);

    cJSON *serial_number = cJSON_GetObjectItemCaseSensitive(root, "serial_number");
    if(cJSON_IsString(serial_number))
        dd_add_text(report, DD_SECTION_DEVICE, "serial_number", "Serial number",
                    "%s", serial_number->valuestring);

    cJSON *firmware_version = cJSON_GetObjectItemCaseSensitive(root, "firmware_version");
    if(cJSON_IsString(firmware_version))
        dd_add_text(report, DD_SECTION_DEVICE, "firmware", "Firmware",
                    "%s", firmware_version->valuestring);

    cJSON *user_capacity = cJSON_GetObjectItemCaseSensitive(root, "user_capacity");
    cJSON *bytes = cJSON_GetObjectItemCaseSensitive(user_capacity, "bytes");
    if(cJSON_IsNumber(bytes)){
        double capacity = cJSON_GetNumberValue(bytes);

        dd_flag_note(dd_add_number(report, DD_SECTION_DEVICE, "capacity",
                                   "Capacity", DD_BYTES, capacity),
                     DD_OK, "%.1f GiB", capacity / (1024.0 * 1024.0 * 1024.0));
    }

    cJSON *smart_status = cJSON_GetObjectItemCaseSensitive(root, "smart_status");
    cJSON *passed = cJSON_GetObjectItemCaseSensitive(smart_status, "passed");
    if(cJSON_IsBool(passed))
        dd_flag(dd_add_text(report, DD_SECTION_HEALTH, "smart_status", "SMART status",
                            "%s", cJSON_IsTrue(passed) ? "passed" : "not passed"),
                cJSON_IsTrue(passed) ? DD_GOOD : DD_ALARM);

    cJSON *temperature = cJSON_GetObjectItemCaseSensitive(root, "temperature");
    cJSON *current_temp = cJSON_GetObjectItemCaseSensitive(temperature, "current");
    if(cJSON_IsNumber(current_temp))
        dd_add_number(report, DD_SECTION_HEALTH, "temperature", "Temperature",
                      DD_CELSIUS, cJSON_GetNumberValue(current_temp));

    cJSON *smart_support = cJSON_GetObjectItemCaseSensitive(root, "smart_support");
    cJSON *available = cJSON_GetObjectItemCaseSensitive(smart_support, "available");
    cJSON *enabled = cJSON_GetObjectItemCaseSensitive(smart_support, "enabled");
    if(cJSON_IsTrue(available) && cJSON_IsFalse(enabled))
        dd_add_note(report, DD_SECTION_HEALTH, DD_WATCH,
                    "SMART is supported but disabled, these values may be out of date");
}

/* Builds the full report for a smartctl JSON output into report, dispatching
   to the NVMe or ATA builder based on the device protocol. Does not print:
   the caller decides what to do with the finished report. */
void build_disk_report(cJSON *root, dd_report *report){
    build_common_report(root, report);

    cJSON *device = cJSON_GetObjectItemCaseSensitive(root, "device");
    cJSON *protocol = cJSON_GetObjectItemCaseSensitive(device, "protocol");

    if(cJSON_IsString(protocol)){
        if(strcmp(protocol->valuestring, "NVMe") == 0){
            build_nvme_report(root, report);
        }else if(strcmp(protocol->valuestring, "ATA") == 0){
            cJSON *rotation = cJSON_GetObjectItemCaseSensitive(root, "rotation_rate");

            if((cJSON_IsNumber(rotation) && rotation->valueint == 0) || rotation == NULL)
                build_ata_ssd_report(root, report);
            else
                build_ata_hdd_report(root, report);
        }
    }
}
