#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <string.h>
#include <errno.h>
#include <sys/wait.h>

#include "cJSON.h"
#include "diskdoc_core.h"

/* NVMe data unit is 1000 sectors of 512 bytes */
#define DD_NVME_DATA_UNIT 512000.0

/* smartctl reports its outcome as a bitmask.
   
   Bits 0-2 mean smartctl itself could not do its job, so there is no report to  read.
   
   Bits 3-7 mean the report is valid and it is the disk that has
   something to report: those are results, not errors. 
*/
#define DD_SMARTCTL_FAILED   0x03
#define DD_SMARTCTL_PARTIAL  0x04
#define DD_SMARTCTL_FINDINGS 0xF8
#define DD_SMARTCTL_OPEN_FAILED 0x02

#define DD_FIELD "  %-24s"

/* ANSI Colors */
#define COLOR_RED     "\x1b[31m"
#define COLOR_GREEN   "\x1b[32m"
#define COLOR_YELLOW  "\x1b[33m"
#define COLOR_RESET   "\x1b[0m"

static void parse_nvme(cJSON *root);
static void parse_ata_ssd(cJSON *root);
static void parse_ata_hdd(cJSON *root);

/* ######################################################################
   CORE 
   ###################################################################### */

/* Fills the list with the physical disks found under /sys/block.
   Returns 0 on success, -1 when the directory cannot be read. */
int scan_physical_disks(dd_disk_list *list){
    list->count = 0;

    DIR *dir = opendir("/sys/block");
    if(dir == NULL){
        perror("Could not open /sys/block");
        return -1;
    }

    struct dirent *entry;
    char path[256];

    while((entry = readdir(dir)) != NULL){
        if(entry->d_name[0] == '.') continue; //ignore '.' and '..'
        if(strncmp(entry->d_name, "sr", 2) == 0) continue; //ignore optical drives

        int n = snprintf(path, sizeof(path), "/sys/block/%s/device", entry->d_name);
        if(n < 0 || (size_t)n >= sizeof(path)) continue; //truncate

        //check if path exists in order to know if entry is a physical device.
        if(access(path, F_OK) != 0) continue;

        if(strlen(entry->d_name) >= DD_NAME_LEN) continue; //name we cannot store

        if(list->count == DD_MAX_DISKS){
            fprintf(stderr, COLOR_RED "More than %d disks, ignoring the rest\n" COLOR_RESET,
                    DD_MAX_DISKS);
            break;
        }

        strcpy(list->disks[list->count].name, entry->d_name);
        list->count++;
    }

    closedir(dir);
    return 0;
}

/* Prints the scanned disks, numbered so the user can pick one */
void print_disk_list(const dd_disk_list *list){
    if(list->count == 0){
        puts(COLOR_RED "No physical disk found." COLOR_RESET);
        return;
    }

    for(size_t i = 0; i < list->count; i++)
        printf(COLOR_GREEN "[%zu]Device:" COLOR_RESET " /dev/%s\n", i, list->disks[i].name);
}

/* Asks which disk to analyze until the answer is a valid index.
   Returns the chosen index, or -1 when the user quits. */
int prompt_disk_choice(const dd_disk_list *list){
    char line[64];

    if(list->count == 0) return -1;

    for(;;){
        printf("Select a disk [0-%zu], or q to quit: ", list->count - 1);
        fflush(stdout);

        if(fgets(line, sizeof(line), stdin) == NULL){
            putchar('\n');
            return -1; //EOF, the user pressed ctrl-D
        }

        //if the line did not fit, drop the rest so it is not read as an answer
        if(strchr(line, '\n') == NULL){
            int c;
            while((c = getchar()) != '\n' && c != EOF)
                ;
        }

        if(line[0] == 'q' || line[0] == 'Q') return -1;

        char *end;
        errno = 0;
        long choice = strtol(line, &end, 10);

        if(end == line){ //no digits at all
            puts(COLOR_RED "Invalid choice, try again." COLOR_RESET);
            continue;
        }

        //only trailing blanks are allowed after the number
        while(*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r') end++;

        if(*end != '\0' || errno == ERANGE || choice < 0 || choice >= (long)list->count){
            puts(COLOR_RED "Invalid choice, try again." COLOR_RESET);
            continue;
        }

        return (int)choice;
    }
}

/* Reads a whole pipe into a buffer that grows as needed.
   Returns a string the caller has to free, or NULL on error. */
static char* read_smartctl_output(FILE *fp){
    size_t capacity = 16384;
    size_t length   = 0;

    char *buffer = malloc(capacity);
    if(buffer == NULL) return NULL;

    for(;;){
        if(length == capacity - 1){
            size_t new_capacity = capacity * 2;
            char *tmp = realloc(buffer, new_capacity);
            if(tmp == NULL){
                free(buffer);
                return NULL;
            }
            buffer   = tmp;
            capacity = new_capacity;
        }

        size_t n = fread(buffer + length, 1, capacity - 1 - length, fp);
        length += n;

        if(n == 0){
            if(ferror(fp)){
                free(buffer);
                return NULL;
            }
            break; //feof
        }
    }

    buffer[length] = '\0';
    return buffer;
}

/* Prints a section title underlined with dashes */
static void print_section(const char *title){
    printf("\n" COLOR_GREEN "%s" COLOR_RESET "\n", title);

    for(size_t i = strlen(title); i > 0; i--) putchar('-');
    putchar('\n');
}

/* Prints the messages smartctl attached to its report, in the given color */
static void print_smartctl_messages(cJSON *smartctl, FILE *stream, const char *color){
    cJSON *messages = cJSON_GetObjectItemCaseSensitive(smartctl, "messages");
    if(!cJSON_IsArray(messages)) return;

    cJSON *message;
    cJSON_ArrayForEach(message, messages){
        cJSON *string = cJSON_GetObjectItemCaseSensitive(message, "string");
        if(cJSON_IsString(string))
            fprintf(stream, "%s  %s\n" COLOR_RESET, color, string->valuestring);
    }
}

/* Decodes the smartctl exit bitmask and prints what it reported.
   Returns 1 when the report can be trusted, 0 when there is no report at all. */
static int check_smartctl_status(cJSON *root){
    static const char *const findings[] = {
        "SMART status reports the disk is FAILING",         //bit 3
        "some prefail attributes are below the threshold",  //bit 4
        "some attributes were below the threshold in the past", //bit 5
        "the device error log contains records",            //bit 6
        "the self test log contains errors",                //bit 7
    };

    cJSON *smartctl = cJSON_GetObjectItemCaseSensitive(root, "smartctl");
    if(smartctl == NULL) return 1;

    cJSON *exit_status = cJSON_GetObjectItemCaseSensitive(smartctl, "exit_status");
    if(!cJSON_IsNumber(exit_status)) return 1; 

    unsigned status = (unsigned)cJSON_GetNumberValue(exit_status);

    if(status & DD_SMARTCTL_FAILED){
        fprintf(stderr, COLOR_RED "smartctl could not read this device (0x%02x):\n" COLOR_RESET,
                status);
        print_smartctl_messages(smartctl, stderr, COLOR_RED);

        if(status & DD_SMARTCTL_OPEN_FAILED)
            fprintf(stderr, COLOR_YELLOW
                    "Reading SMART data usually requires root, try again with sudo.\n"
                    COLOR_RESET);
        return 0;
    }

    /* A single command was refused, which on NVMe usually means an optional
       feature the device does not implement. The rest of the report stands. */
    if(status & DD_SMARTCTL_PARTIAL){
        printf(COLOR_YELLOW "Some data could not be read (0x%02x):\n" COLOR_RESET, status);
        print_smartctl_messages(smartctl, stdout, COLOR_YELLOW);
    }

    if(status & DD_SMARTCTL_FINDINGS){
        printf(COLOR_YELLOW "Findings reported by smartctl (0x%02x):\n" COLOR_RESET, status);

        for(size_t i = 0; i < sizeof findings / sizeof findings[0]; i++)
            if(status & (1u << (i + 3)))
                printf(COLOR_YELLOW "  - %s\n" COLOR_RESET, findings[i]);
    }

    return 1;
}

/* Prints the values common to every disk, then hands the report to the
   parser that matches its protocol */
static void parse_disk_data(const char *data){
    cJSON *root = cJSON_Parse(data);

    if(!root) return;

    if(!check_smartctl_status(root)){
        cJSON_Delete(root);
        return;
    }

    // --- Common values ---
    print_section("Device");

    // Model name
    cJSON *model_name = cJSON_GetObjectItemCaseSensitive(root, "model_name");
    if(cJSON_IsString(model_name))
        printf(DD_FIELD "%s\n", "Model", model_name->valuestring);

    // Serial number
    cJSON *serial_number = cJSON_GetObjectItemCaseSensitive(root, "serial_number");
    if(cJSON_IsString(serial_number))
        printf(DD_FIELD "%s\n", "Serial number", serial_number->valuestring);

    // Firmware version
    cJSON *firmware_version = cJSON_GetObjectItemCaseSensitive(root, "firmware_version");
    if(cJSON_IsString(firmware_version))
        printf(DD_FIELD "%s\n", "Firmware", firmware_version->valuestring);

    // User capacity
    cJSON *user_capacity = cJSON_GetObjectItemCaseSensitive(root, "user_capacity");
    if(user_capacity != NULL){
        cJSON *bytes = cJSON_GetObjectItemCaseSensitive(user_capacity, "bytes");
        if(cJSON_IsNumber(bytes)){
            double capacity = cJSON_GetNumberValue(bytes);
            printf(DD_FIELD "%.0f GB (%.1f GiB)\n", "Capacity",
                   capacity / 1e9, capacity / (1024.0 * 1024.0 * 1024.0));
        }
    }

    print_section("Health");

    // Smart status
    cJSON *smart_status = cJSON_GetObjectItemCaseSensitive(root, "smart_status");
    if(smart_status != NULL){
        cJSON *passed = cJSON_GetObjectItemCaseSensitive(smart_status, "passed");
        if(cJSON_IsBool(passed)){
            if(cJSON_IsTrue(passed))
                printf(DD_FIELD COLOR_GREEN "passed" COLOR_RESET "\n", "SMART status");
            else
                printf(DD_FIELD COLOR_RED "not passed" COLOR_RESET "\n", "SMART status");
        }
    }

    // Temperature
    cJSON *temperature = cJSON_GetObjectItemCaseSensitive(root, "temperature");
    if(temperature != NULL){
        cJSON *current_temp = cJSON_GetObjectItemCaseSensitive(temperature, "current");
        if(cJSON_IsNumber(current_temp))
            printf(DD_FIELD "%.0f °C\n", "Temperature", cJSON_GetNumberValue(current_temp));
    }

    // Check SMART support status
    cJSON *smart_support = cJSON_GetObjectItemCaseSensitive(root, "smart_support");
    if(smart_support != NULL){
        cJSON *available = cJSON_GetObjectItemCaseSensitive(smart_support, "available");
        cJSON *enabled = cJSON_GetObjectItemCaseSensitive(smart_support, "enabled");

        if(cJSON_IsBool(available) && cJSON_IsBool(enabled)){
            if(cJSON_IsTrue(available) && cJSON_IsFalse(enabled))
                printf(DD_FIELD COLOR_YELLOW "SMART is supported but disabled, "
                        "these values may be out of date" COLOR_RESET "\n", "");
        }
    }

    // Check the device type
    cJSON *device = cJSON_GetObjectItemCaseSensitive(root, "device");
    if(device != NULL){
        cJSON *protocol = cJSON_GetObjectItemCaseSensitive(device, "protocol");

        if(cJSON_IsString(protocol)){
            if(strcmp(protocol->valuestring, "NVMe") == 0){
                parse_nvme(root);
            }else if(strcmp(protocol->valuestring, "ATA") == 0){
                cJSON *rotation = cJSON_GetObjectItemCaseSensitive(root, "rotation_rate");

                if((cJSON_IsNumber(rotation) && rotation->valueint == 0) || rotation == NULL)
                    parse_ata_ssd(root);
                else
                    parse_ata_hdd(root);
            }
        }
    }
    cJSON_Delete(root);
}

/* Runs smartctl on the given device and prints the report it returns */
void analyze_disk(const char *dev_path){
    char command[256];
    int status = 0;

    snprintf(command, sizeof(command), "smartctl -x -j /dev/%s 2>/dev/null", dev_path);
    
    FILE *fp = popen(command, "r");
    if(fp == NULL){
        fprintf(stderr, COLOR_RED "Error while running smartctl on %s\n" COLOR_RESET, dev_path);
        return;
    }

    printf(COLOR_YELLOW "Analyzing /dev/%s..." COLOR_RESET "\n", dev_path);
    
    char *data = read_smartctl_output(fp);

    status = pclose(fp);

    if(data == NULL){
        fprintf(stderr, COLOR_RED "Could not read smartctl output for %s\n" COLOR_RESET, dev_path);
        return;
    }

    if(status == -1)
        perror("pclose");
    else if(WIFSIGNALED(status))
        fprintf(stderr, COLOR_RED "smartctl was killed by signal %d\n" COLOR_RESET,
                WTERMSIG(status));

    parse_disk_data(data);
    free(data);
}

/* ######################################################################
   NVME
   ###################################################################### */

/* Decodes the NVMe critical warning bitmask into readable alarms */
static void print_critical_warning(unsigned warning){
    static const char *const alarms[] = {
        "available spare is below the threshold",
        "temperature is outside the critical thresholds",
        "NVM subsystem reliability is degraded",
        "media has been placed in read only mode",
        "volatile memory backup device has failed",
        "persistent memory region is read only or unreliable",
    };

    if(warning == 0){
        printf(DD_FIELD COLOR_GREEN "none" COLOR_RESET "\n", "Critical warnings");
        return;
    }

    printf(DD_FIELD COLOR_RED "0x%02x" COLOR_RESET "\n", "Critical warnings", warning);

    for(size_t i = 0; i < sizeof alarms / sizeof alarms[0]; i++)
        if(warning & (1u << i))
            printf(DD_FIELD COLOR_RED "- %s" COLOR_RESET "\n", "", alarms[i]);

    if(warning & 0xC0)
        printf(DD_FIELD COLOR_RED "- unknown flag set by the device" COLOR_RESET "\n", "");
}

/* Prints wear, reliability and usage of an NVMe disk from its health log */
static void parse_nvme(cJSON *root){
    cJSON *log = cJSON_GetObjectItemCaseSensitive(root, "nvme_smart_health_information_log");
    if(log == NULL) return;

    print_section("Wear and reliability");

    // Critical warning
    cJSON *c_warning = cJSON_GetObjectItemCaseSensitive(log, "critical_warning");
    if(cJSON_IsNumber(c_warning))
        print_critical_warning((unsigned)cJSON_GetNumberValue(c_warning));

    // Percentage used
    cJSON *percentage_used = cJSON_GetObjectItemCaseSensitive(log, "percentage_used");
    if(cJSON_IsNumber(percentage_used))
        printf(DD_FIELD "%d%%\n", "Life usage", (int)cJSON_GetNumberValue(percentage_used));

    // Reserve blocks
    cJSON *available_spare = cJSON_GetObjectItemCaseSensitive(log, "available_spare");
    cJSON *available_spare_thr = cJSON_GetObjectItemCaseSensitive(log, "available_spare_threshold");
    if(cJSON_IsNumber(available_spare) && cJSON_IsNumber(available_spare_thr)){
        double spare = cJSON_GetNumberValue(available_spare);
        double threshold = cJSON_GetNumberValue(available_spare_thr);

        printf(DD_FIELD "%s%.0f%%" COLOR_RESET " (threshold %.0f%%)\n", "Available spare",
                spare < threshold ? COLOR_RED : "", spare, threshold);

        if(spare < threshold)
            printf(DD_FIELD COLOR_RED "the disk has run out of replacement blocks\n"
                    COLOR_RESET, "");
    }

    // Media errors
    cJSON *media_errors = cJSON_GetObjectItemCaseSensitive(log, "media_errors");
    if(cJSON_IsNumber(media_errors)){
        double errors = cJSON_GetNumberValue(media_errors);
        printf(DD_FIELD "%s%.0f" COLOR_RESET "\n", "Media errors",
                errors != 0 ? COLOR_YELLOW : "", errors);
    }

    // Err logs
    cJSON *num_err_log = cJSON_GetObjectItemCaseSensitive(log, "num_err_log_entries");
    if(cJSON_IsNumber(num_err_log))
        printf(DD_FIELD "%zu\n", "Error log entries", (size_t)cJSON_GetNumberValue(num_err_log));

    print_section("Usage");

    // Power
    cJSON *p_on_hours = cJSON_GetObjectItemCaseSensitive(log, "power_on_hours");
    cJSON *p_cycles = cJSON_GetObjectItemCaseSensitive(log, "power_cycles");
    if(cJSON_IsNumber(p_on_hours) && cJSON_IsNumber(p_cycles)){
        double hours = cJSON_GetNumberValue(p_on_hours);

        printf(DD_FIELD "%.0f h (%.1f years)\n", "Power on hours", hours, hours / (24 * 365.0));
        printf(DD_FIELD "%zu\n", "Power cycles", (size_t)cJSON_GetNumberValue(p_cycles));
    }

    cJSON *units_written = cJSON_GetObjectItemCaseSensitive(log, "data_units_written");
    cJSON *units_read = cJSON_GetObjectItemCaseSensitive(log, "data_units_read");
    if(cJSON_IsNumber(units_written) && cJSON_IsNumber(units_read)){
        printf(DD_FIELD "%.2f TB\n", "Data written",
                cJSON_GetNumberValue(units_written) * DD_NVME_DATA_UNIT / 1e12);
        printf(DD_FIELD "%.2f TB\n", "Data read",
                cJSON_GetNumberValue(units_read) * DD_NVME_DATA_UNIT / 1e12);
    }
}

/* ######################################################################
   SATA COMMON
   ###################################################################### */

/* Looks up an attribute by id, as its position in the table is not fixed.
   name_part guards the vendor specific ids, NULL skips that check. */
static cJSON *find_ata_attribute(cJSON *table, int id, const char *name_part){
    cJSON *attribute;
    cJSON_ArrayForEach(attribute, table){
        cJSON *attr_id = cJSON_GetObjectItemCaseSensitive(attribute, "id");
        if(!cJSON_IsNumber(attr_id) || (int)cJSON_GetNumberValue(attr_id) != id)
            continue;

        if(name_part != NULL){
            cJSON *name = cJSON_GetObjectItemCaseSensitive(attribute, "name");
            if(!cJSON_IsString(name) || strstr(name->valuestring, name_part) == NULL)
                return NULL; //the id means something else on this disk
        }
        return attribute;
    }
    return NULL;
}

/* Reads the raw counter of an attribute into out.
   Returns 0 when the disk does not report that attribute. */
static int ata_attribute_raw(cJSON *table, int id, const char *name_part, double *out){
    cJSON *attribute = find_ata_attribute(table, id, name_part);

    cJSON *raw = cJSON_GetObjectItemCaseSensitive(attribute, "raw");
    cJSON *raw_value = cJSON_GetObjectItemCaseSensitive(raw, "value");
    if(!cJSON_IsNumber(raw_value)) return 0;

    *out = cJSON_GetNumberValue(raw_value);
    return 1;
}

/* Reads the normalized value of an attribute: for the wear attributes that is
   the percentage of life left, while the raw side holds a vendor counter. */
static int ata_attribute_value(cJSON *table, int id, const char *name_part, double *out){
    cJSON *attribute = find_ata_attribute(table, id, name_part);

    cJSON *value = cJSON_GetObjectItemCaseSensitive(attribute, "value");
    if(!cJSON_IsNumber(value)) return 0;

    *out = cJSON_GetNumberValue(value);
    return 1;
}

/* Looks up a device statistic by name across every page of the -x log.
   Returns 0 when the disk does not report it or does not keep it up to date. */
static int find_device_statistic(cJSON *root, const char *name, double *out){
    cJSON *statistics = cJSON_GetObjectItemCaseSensitive(root, "ata_device_statistics");
    cJSON *pages = cJSON_GetObjectItemCaseSensitive(statistics, "pages");

    cJSON *page;
    cJSON_ArrayForEach(page, pages){
        cJSON *table = cJSON_GetObjectItemCaseSensitive(page, "table");

        cJSON *entry;
        cJSON_ArrayForEach(entry, table){
            cJSON *entry_name = cJSON_GetObjectItemCaseSensitive(entry, "name");
            if(!cJSON_IsString(entry_name) || strcmp(entry_name->valuestring, name) != 0)
                continue;

            /* a statistic the device does not keep is reported as zero */
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
static int ata_data_moved(cJSON *root, cJSON *table, const char *statistic,
                          int id, double *bytes){
    cJSON *block_size = cJSON_GetObjectItemCaseSensitive(root, "logical_block_size");
    double sector = cJSON_IsNumber(block_size) ? cJSON_GetNumberValue(block_size) : 512.0;

    double sectors;
    if(find_device_statistic(root, statistic, &sectors)){
        *bytes = sectors * sector;
        return 1;
    }

    cJSON *attribute = find_ata_attribute(table, id, NULL);
    cJSON *name = cJSON_GetObjectItemCaseSensitive(attribute, "name");
    cJSON *raw = cJSON_GetObjectItemCaseSensitive(attribute, "raw");
    cJSON *raw_value = cJSON_GetObjectItemCaseSensitive(raw, "value");
    if(!cJSON_IsString(name) || !cJSON_IsNumber(raw_value)) return 0;

    double count = cJSON_GetNumberValue(raw_value);

    if(strstr(name->valuestring, "GiB") != NULL)
        *bytes = count * (1024.0 * 1024.0 * 1024.0);
    else if(strstr(name->valuestring, "LBA") != NULL ||
            strstr(name->valuestring, "Sector") != NULL)
        *bytes = count * sector;
    else
        return 0; //an unit we cannot name

    return 1;
}

/* Prints a byte count in the largest unit that keeps it readable */
static void print_data_moved(const char *label, double bytes){
    if(bytes >= 1e12)
        printf(DD_FIELD "%.2f TB\n", label, bytes / 1e12);
    else if(bytes >= 1e9)
        printf(DD_FIELD "%.2f GB\n", label, bytes / 1e9);
    else
        printf(DD_FIELD "%.2f MB\n", label, bytes / 1e6);
}

/* ######################################################################
   SATA SSD
   ###################################################################### */

/* How much of the rated write endurance is still available */
static void print_ata_life_left(cJSON *root, cJSON *table){
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

    if(find_device_statistic(root, "Percentage Used Endurance Indicator", &left)){
        left = 100.0 - left;
        if(left < 0) left = 0; 
    }else {
        size_t i;
        for(i = 0; i < sizeof wear_attributes / sizeof wear_attributes[0]; i++)
            if(ata_attribute_value(table, wear_attributes[i].id,
                                   wear_attributes[i].name, &left))
                break;

        //the disk reports its wear in none of the ways we know
        if(i == sizeof wear_attributes / sizeof wear_attributes[0]) return;
    }

    const char *color = "";
    if(left <= 10) color = COLOR_RED;
    else if(left <= 20) color = COLOR_YELLOW;

    printf(DD_FIELD "%s%.0f%%" COLOR_RESET "\n", "Life left", color, left);
}

/* Prints self test outcome, wear and error counters of an ATA SSD */
static void parse_ata_ssd(cJSON *root){
    
    print_section("Wear and reliability");
    // Self test
    cJSON *ata_smart_data = cJSON_GetObjectItemCaseSensitive(root, "ata_smart_data");
    if(ata_smart_data != NULL){
        cJSON *self_test = cJSON_GetObjectItemCaseSensitive(ata_smart_data, "self_test");
        if(self_test != NULL){ 
            cJSON *test_status = cJSON_GetObjectItemCaseSensitive(self_test, "status");
            if(test_status != NULL){
                cJSON *passed = cJSON_GetObjectItemCaseSensitive(test_status, "passed");
                if(cJSON_IsTrue(passed))
                    printf(DD_FIELD COLOR_GREEN "passed\n" COLOR_RESET, "Self test");
                else
                    printf(DD_FIELD COLOR_RED "not passed\n" COLOR_RESET, "Self test");          
            } 
        }
    }

    cJSON *attributes = cJSON_GetObjectItemCaseSensitive(root, "ata_smart_attributes");
    cJSON *table = cJSON_GetObjectItemCaseSensitive(attributes, "table");

    // Wearout
    print_ata_life_left(root, table);

    if(cJSON_IsArray(table)){
        double count;

        // Reallocated sectors
        if(ata_attribute_raw(table, 5, NULL, &count))
            printf(DD_FIELD "%s%.0f" COLOR_RESET "\n", "Reallocated sectors",
                    count != 0 ? COLOR_YELLOW : "", count);

        /* Program and erase failures: most controllers report them as 171/172,
           Samsung and a few others as 181/182. A disk exposes one pair or the
           other, so the first id that answers is the one to trust. */
        if(ata_attribute_raw(table, 171, "Program_Fail", &count) ||
           ata_attribute_raw(table, 181, "Program_Fail", &count))
            printf(DD_FIELD "%s%.0f" COLOR_RESET "\n", "Program failures",
                    count != 0 ? COLOR_YELLOW : "", count);

        if(ata_attribute_raw(table, 172, "Erase_Fail", &count) ||
           ata_attribute_raw(table, 182, "Erase_Fail", &count))
            printf(DD_FIELD "%s%.0f" COLOR_RESET "\n", "Erase failures",
                    count != 0 ? COLOR_YELLOW : "", count);
        
        if(ata_attribute_raw(table, 187, "Reported_Uncorrect", &count))
            printf(DD_FIELD "%s%.0f" COLOR_RESET "\n", "Reported uncorrect",
                    count != 0 ? COLOR_YELLOW : "", count);
        
        print_section("Usage");

        if(find_device_statistic(root, "Power-on Hours", &count))
            printf(DD_FIELD "%.0f\n", "Power on hours", count);

        if(find_device_statistic(root, "Lifetime Power-On Resets", &count))
            printf(DD_FIELD "%.0f\n", "Power cycle count", count);


        // Data moved
        double bytes;
        if(ata_data_moved(root, table, "Logical Sectors Written", 241, &bytes))
            print_data_moved("Data written", bytes);

        if(ata_data_moved(root, table, "Logical Sectors Read", 242, &bytes))
            print_data_moved("Data read", bytes);
    }
}

/* ######################################################################
   SATA HDD
   ###################################################################### */

/* Same for a spinning disk, whose health lives in different attributes */
static void parse_ata_hdd(cJSON *root){
    print_section("Wear and reliability");
    
    cJSON *attributes = cJSON_GetObjectItemCaseSensitive(root, "ata_smart_attributes");
    cJSON *table = cJSON_GetObjectItemCaseSensitive(attributes, "table");

    if(cJSON_IsArray(table)){
        double count; 

         if(ata_attribute_raw(table, 5, NULL, &count))
             printf(DD_FIELD "%s%.0f" COLOR_RESET "\n", "Reallocated sectors",
                count != 0 ? COLOR_YELLOW : "", count);

        if(ata_attribute_raw(table, 187, NULL, &count))
             printf(DD_FIELD "%s%.0f" COLOR_RESET "\n", "Reported uncorrect",
                count != 0 ? COLOR_YELLOW : "", count);

        if(ata_attribute_raw(table, 188, NULL, &count))
             printf(DD_FIELD "%s%.0f" COLOR_RESET "\n", "Command timeout",
                count != 0 ? COLOR_YELLOW : "", count);

        if(ata_attribute_raw(table, 197, NULL, &count))
             printf(DD_FIELD "%s%.0f" COLOR_RESET "\n", "Current pending sector",
                count != 0 ? COLOR_YELLOW : "", count);

        if(ata_attribute_raw(table, 198, NULL, &count))
             printf(DD_FIELD "%s%.0f" COLOR_RESET "\n", "Offline uncorrectable",
                count != 0 ? COLOR_YELLOW : "", count);

        print_section("Usage");

        if(find_device_statistic(root, "Power-on Hours", &count))
            printf(DD_FIELD "%.0f\n", "Power on hours", count);

        if(find_device_statistic(root, "Lifetime Power-On Resets", &count))
            printf(DD_FIELD "%.0f\n", "Power cycle count", count);

        // Data moved
        double bytes;
        if(ata_data_moved(root, table, "Logical Sectors Written", 241, &bytes))
            print_data_moved("Data written", bytes);

        if(ata_data_moved(root, table, "Logical Sectors Read", 242, &bytes))
            print_data_moved("Data read", bytes);
    }
}























