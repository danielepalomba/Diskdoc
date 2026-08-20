#include <string.h>
#include <stdlib.h>

#include "cJSON.h"
#include "dd_parse_ata.h"
#include "dd_report.h"

/* The two sources every ATA reader draws from, plus the sector size that
   turns their counters into bytes. Built once per disk so that a reader
   takes one context instead of picking its own arguments out of the tree. */
typedef struct {
    cJSON *root;
    cJSON *attributes;  /* ata_smart_attributes.table */
    double sector_size;
} dd_ata;

/* Builds the ATA context used by every reader below: attribute table and
   sector size for this disk. */
static dd_ata ata_context(cJSON *root){
    cJSON *attributes = cJSON_GetObjectItemCaseSensitive(root, "ata_smart_attributes");
    cJSON *block_size = cJSON_GetObjectItemCaseSensitive(root, "logical_block_size");

    dd_ata ata;
    ata.root = root;
    ata.attributes = cJSON_GetObjectItemCaseSensitive(attributes, "table");
    ata.sector_size = cJSON_IsNumber(block_size) ? cJSON_GetNumberValue(block_size) : 512.0;

    return ata;
}

/* Looks up an attribute by id, as its position in the table is not fixed.
   name_part guards the vendor specific ids, NULL skips that check. */
static cJSON *find_ata_attribute(const dd_ata *ata, int id, const char *name_part){
    cJSON *attribute;
    cJSON_ArrayForEach(attribute, ata->attributes){
        cJSON *attr_id = cJSON_GetObjectItemCaseSensitive(attribute, "id");
        if(!cJSON_IsNumber(attr_id) || (int)cJSON_GetNumberValue(attr_id) != id)
            continue;

        if(name_part != NULL){
            cJSON *name = cJSON_GetObjectItemCaseSensitive(attribute, "name");
            if(!cJSON_IsString(name) || strstr(name->valuestring, name_part) == NULL)
                return NULL;
        }
        return attribute;
    }
    return NULL;
}

/* Reads the raw counter of an attribute into out.
   Returns 0 when the disk does not report that attribute. */
static int ata_attribute_raw(const dd_ata *ata, int id, const char *name_part,
                             double *out){
    cJSON *attribute = find_ata_attribute(ata, id, name_part);

    cJSON *raw = cJSON_GetObjectItemCaseSensitive(attribute, "raw");
    cJSON *raw_string = cJSON_GetObjectItemCaseSensitive(raw, "string");
    
    if(!cJSON_IsString(raw_string)){
        cJSON *raw_value = cJSON_GetObjectItemCaseSensitive(raw, "value");
        if(!cJSON_IsNumber(raw_value)) return 0;
        *out = cJSON_GetNumberValue(raw_value);
        return 1;
    }else{
        char *end;
        double decoded = strtod(raw_string->valuestring, &end);
        if(end != raw_string->valuestring){
            *out = decoded;
            return 1;
        }
        return 0;
    }
}

/* Reads the normalized value of an attribute (percentage of life left, for
   the wear attributes). */
static int ata_attribute_value(const dd_ata *ata, int id, const char *name_part,
                               double *out){
    cJSON *attribute = find_ata_attribute(ata, id, name_part);

    cJSON *value = cJSON_GetObjectItemCaseSensitive(attribute, "value");
    if(!cJSON_IsNumber(value)) return 0;

    *out = cJSON_GetNumberValue(value);
    return 1;
}

/* Looks up a device statistic by name across every page of the -x log,
   skipping entries the disk marks invalid.
   Returns 0 when the statistic is absent or invalid. */
static int find_device_statistic(const dd_ata *ata, const char *name, double *out){
    cJSON *statistics = cJSON_GetObjectItemCaseSensitive(ata->root, "ata_device_statistics");
    cJSON *pages = cJSON_GetObjectItemCaseSensitive(statistics, "pages");

    cJSON *page;
    cJSON_ArrayForEach(page, pages){
        cJSON *table = cJSON_GetObjectItemCaseSensitive(page, "table");

        cJSON *entry;
        cJSON_ArrayForEach(entry, table){
            cJSON *entry_name = cJSON_GetObjectItemCaseSensitive(entry, "name");
            if(!cJSON_IsString(entry_name) || strcmp(entry_name->valuestring, name) != 0)
                continue;

            cJSON *flags = cJSON_GetObjectItemCaseSensitive(entry, "flags");
            if(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(flags, "valid"))) return 0;

            cJSON *value = cJSON_GetObjectItemCaseSensitive(entry, "value");
            if(!cJSON_IsNumber(value)) return 0;

            *out = cJSON_GetNumberValue(value);
            return 1;
        }
    }
    return 0;
}

/* Bytes the disk has moved in its life, from the statistic if it has one.
   Returns 0 when nothing reports them in a unit we can name. */
static int ata_data_moved(const dd_ata *ata, const char *statistic,
                          int id, double *bytes){
    double sectors;
    if(find_device_statistic(ata, statistic, &sectors)){
        *bytes = sectors * ata->sector_size;
        return 1;
    }
    
    double count;
    cJSON *attribute = find_ata_attribute(ata, id, NULL);
    cJSON *name = cJSON_GetObjectItemCaseSensitive(attribute, "name");
    
    if(!cJSON_IsString(name)) return 0;

    if(!ata_attribute_raw(ata, id, NULL, &count)) return 0;

    if(strstr(name->valuestring, "GiB") != NULL)
        *bytes = count * (1024.0 * 1024.0 * 1024.0);
    else if(strstr(name->valuestring, "LBA") != NULL ||
        strstr(name->valuestring, "Sector") != NULL)
    *bytes = count * ata->sector_size;
    else
        return 0;

    return 1;
}

/* A counter whose expected value is zero: anything else is worth watching */
static void add_counter(dd_report *report, const char *key, const char *label,
                        double count){
    dd_flag(dd_add_number(report, DD_SECTION_WEAR, key, label, DD_COUNT, count),
            count != 0 ? DD_WATCH : DD_OK);
}

/* Adds a wear counter read from the attribute table; a missing attribute is
   reported as absent, not zero. */
static void add_ata_counter(dd_report *report, const dd_ata *ata, int id,
                            const char *name_part, const char *key,
                            const char *label){
    double count;

    if(!ata_attribute_raw(ata, id, name_part, &count))
        dd_add_absent(report, DD_SECTION_WEAR, key, label);
    else
        add_counter(report, key, label, count);
}

/* How long the disk has worked and how much it has moved. Identical on both
   families, so both builders end with this. */
static void add_ata_usage(dd_report *report, const dd_ata *ata){
    double count;
    double bytes;

   if(find_device_statistic(ata, "Power-on Hours", &count))
        dd_add_number(report, DD_SECTION_USAGE, "power_on_hours", "Power on hours",
                      DD_HOURS, count);
    else
        dd_add_absent(report, DD_SECTION_USAGE, "power_on_hours", "Power on hours");

    if(find_device_statistic(ata, "Lifetime Power-On Resets", &count))
        dd_add_number(report, DD_SECTION_USAGE, "power_cycles", "Power cycle count",
                      DD_COUNT, count);
    else
        dd_add_absent(report, DD_SECTION_USAGE, "power_cycles", "Power cycle count");

    if(ata_data_moved(ata, "Logical Sectors Written", 241, &bytes))
        dd_add_number(report, DD_SECTION_USAGE, "data_written", "Data written",
                      DD_BYTES, bytes);
    else
        dd_add_absent(report, DD_SECTION_USAGE, "data_written", "Data written");

    if(ata_data_moved(ata, "Logical Sectors Read", 242, &bytes))
        dd_add_number(report, DD_SECTION_USAGE, "data_read", "Data read",
                      DD_BYTES, bytes);
    else
        dd_add_absent(report, DD_SECTION_USAGE, "data_read", "Data read");
}

/* How much of the rated write endurance is still available */
static void add_ata_life_left(dd_report *report, const dd_ata *ata){
   static const struct {
        int id;
        const char *name;
    } wear_attributes[] = {
        { 231, "SSD_Life_Left" },
        { 245, "SSD_Life_Left" },
        { 233, "Media_Wearout_Indicator" },
        { 202, "Percent_Lifetime_Remain" },
        { 169, "Remaining_Lifetime_Perc" },
        { 177, "Wear_Leveling_Count" },
    };

    double left;

    if(find_device_statistic(ata, "Percentage Used Endurance Indicator", &left)){
        left = 100.0 - left;
        if(left < 0) left = 0;
    }else {
        size_t i;
        for(i = 0; i < sizeof wear_attributes / sizeof wear_attributes[0]; i++)
            if(ata_attribute_value(ata, wear_attributes[i].id,
                                   wear_attributes[i].name, &left))
                break;

        if(i == sizeof wear_attributes / sizeof wear_attributes[0]){
            dd_add_absent(report, DD_SECTION_WEAR, "life_left", "Life left");
            return;
        }
    }

    dd_severity severity = DD_OK;
    if(left <= 10) severity = DD_ALARM;
    else if(left <= 20) severity = DD_WATCH;

    dd_flag(dd_add_number(report, DD_SECTION_WEAR, "life_left", "Life left",
                          DD_PERCENT, left), severity);
}

/* Builds the wear and usage report for an ATA SSD. */
void build_ata_ssd_report(cJSON *root, dd_report *report){
    dd_ata ata = ata_context(root);

    cJSON *ata_smart_data = cJSON_GetObjectItemCaseSensitive(root, "ata_smart_data");
    cJSON *self_test = cJSON_GetObjectItemCaseSensitive(ata_smart_data, "self_test");
    cJSON *test_status = cJSON_GetObjectItemCaseSensitive(self_test, "status");
    cJSON *passed = cJSON_GetObjectItemCaseSensitive(test_status, "passed");

    if(test_status != NULL)
        dd_flag(dd_add_text(report, DD_SECTION_WEAR, "self_test", "Self test",
                            "%s", cJSON_IsTrue(passed) ? "passed" : "not passed"),
                cJSON_IsTrue(passed) ? DD_GOOD : DD_ALARM);

    add_ata_life_left(report, &ata);

    add_ata_counter(report, &ata, 5, NULL, "reallocated_sectors", "Reallocated sectors");

    double count;

    if(ata_attribute_raw(&ata, 171, "Program_Fail", &count) ||
       ata_attribute_raw(&ata, 181, "Program_Fail", &count))
        add_counter(report, "program_failures", "Program failures", count);
    else
        dd_add_absent(report, DD_SECTION_WEAR, "program_failures", "Program failures");

    if(ata_attribute_raw(&ata, 172, "Erase_Fail", &count) ||
       ata_attribute_raw(&ata, 182, "Erase_Fail", &count))
        add_counter(report, "erase_failures", "Erase failures", count);
    else
        dd_add_absent(report, DD_SECTION_WEAR, "erase_failures", "Erase failures");

    add_ata_counter(report, &ata, 187, "Reported_Uncorrect",
                    "reported_uncorrect", "Reported uncorrect");

    add_ata_usage(report, &ata);
}

/* Builds the wear and usage report for an ATA HDD from its five
   failure-linked counters. */
void build_ata_hdd_report(cJSON *root, dd_report *report){
    static const struct {
        int id;
        const char *key;
        const char *label;
    } counters[] = {
        {   5, "reallocated_sectors",    "Reallocated sectors"    },
        { 187, "reported_uncorrect",     "Reported uncorrect"     },
        { 188, "command_timeout",        "Command timeout"        },
        { 197, "current_pending_sector", "Current pending sector" },
        { 198, "offline_uncorrectable",  "Offline uncorrectable"  },
    };

    dd_ata ata = ata_context(root);

    for(size_t i = 0; i < sizeof counters / sizeof counters[0]; i++)
        add_ata_counter(report, &ata, counters[i].id, NULL,
                        counters[i].key, counters[i].label);

    add_ata_usage(report, &ata);
}
