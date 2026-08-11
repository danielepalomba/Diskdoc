#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <string.h>
#include <errno.h>
#include <sys/wait.h>

#include "cJSON.h"
#include "diskdoc_core.h"

/* One NVMe data unit is 1000 sectors of 512 bytes */
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

#define DD_FIELD "  %-19s"

/* ANSI Colors */
#define COLOR_RED     "\x1b[31m"
#define COLOR_GREEN   "\x1b[32m"
#define COLOR_YELLOW  "\x1b[33m"
#define COLOR_RESET   "\x1b[0m"

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

void print_disk_list(const dd_disk_list *list){
    if(list->count == 0){
        puts(COLOR_RED "No physical disk found." COLOR_RESET);
        return;
    }

    for(size_t i = 0; i < list->count; i++)
        printf(COLOR_GREEN "[%zu]Device:" COLOR_RESET " /dev/%s\n", i, list->disks[i].name);
}

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

static void print_section(const char *title){
    printf("\n" COLOR_GREEN "%s" COLOR_RESET "\n", title);

    for(size_t i = strlen(title); i > 0; i--) putchar('-');
    putchar('\n');
}

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
        printf(DD_FIELD "%d%%\n", "Life used", (int)cJSON_GetNumberValue(percentage_used));

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

        printf(DD_FIELD "%.0f h (%.1f years)\n", "Power on time", hours, hours / (24 * 365.0));
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

static void parse_ata_ssd(cJSON *root){}

static void parse_ata_hdd(cJSON *root){}

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

//returns 1 when the report can be trusted, 0 when smartctl produced no report
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

void analyze_disk(const char *dev_path){
    char command[256];
    int status = 0;

    snprintf(command, sizeof(command), "smartctl -a -j /dev/%s 2>/dev/null", dev_path);
    
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
