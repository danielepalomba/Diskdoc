#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <string.h>
#include <errno.h>

#include "cJSON.h"
#include "diskdoc_core.h"

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

static void parse_nvme(cJSON *root){}

static void parse_ata_ssd(cJSON *root){}

static void parse_ata_hdd(cJSON *root){}

static void parse_disk_data(const char *data){
    cJSON *root = cJSON_Parse(data);

    if(!root) return;

    // --- Common values ---
    // Model name
    cJSON *model_name = cJSON_GetObjectItemCaseSensitive(root, "model_name");
    if(cJSON_IsString(model_name))
        printf("Model: %s\n", model_name->valuestring);
  
    // Serial number
    cJSON *serial_number = cJSON_GetObjectItemCaseSensitive(root, "serial_number");
    if(cJSON_IsString(serial_number)){
        printf("Serial number: %s\n", serial_number->valuestring);
    }

    // Firmware version
    cJSON *firmware_version = cJSON_GetObjectItemCaseSensitive(root, "firmware_version");
    if(cJSON_IsString(firmware_version)){
        printf("Firmware versions: %s\n", firmware_version->valuestring);
    }
    
    // User capacity
    cJSON *user_capacity = cJSON_GetObjectItemCaseSensitive(root, "user_capacity");
    if(user_capacity != NULL){
        cJSON *bytes = cJSON_GetObjectItemCaseSensitive(user_capacity, "bytes");
        if(cJSON_IsNumber(bytes)){
            //GB is what the vendor advertises, GiB is what the OS reports
            double capacity = cJSON_GetNumberValue(bytes);
            printf("Capacity: %.0f GB (%.1f GiB)\n",
                   capacity / 1e9, capacity / (1024.0 * 1024.0 * 1024.0));
        }
    }

    // Temperature
    cJSON *temperature = cJSON_GetObjectItemCaseSensitive(root, "temperature");
    if(temperature != NULL){
        cJSON *current_temp = cJSON_GetObjectItemCaseSensitive(temperature, "current");
        if(cJSON_IsNumber(current_temp)){
            printf("Temperature: %.1fC°\n", cJSON_GetNumberValue(current_temp));
        }
    }

    // Smart status
    cJSON *smart_status = cJSON_GetObjectItemCaseSensitive(root, "smart_status");
    if(smart_status != NULL){
        cJSON *passed = cJSON_GetObjectItemCaseSensitive(smart_status, "passed");
        if(cJSON_IsBool(passed)){
            if(cJSON_IsTrue(passed))
                 printf("SMART STATUS: " COLOR_GREEN "passed" COLOR_RESET "\n");
            else
                printf("SMART STATUS: " COLOR_RED "not passed" COLOR_RESET "\n");
        }
    }

    // Check SMART support status
    cJSON *smart_support = cJSON_GetObjectItemCaseSensitive(root, "smart_support");
    if(smart_support != NULL){
        cJSON *available = cJSON_GetObjectItemCaseSensitive(smart_support, "available");
        cJSON *enabled = cJSON_GetObjectItemCaseSensitive(smart_support, "enabled");
        
        if(cJSON_IsBool(available) && cJSON_IsBool(enabled)){
            if(cJSON_IsTrue(available) && cJSON_IsFalse(enabled))
                printf(COLOR_YELLOW "Smart support is available but it's not enabled."
                        "The values displayed may not be current\n" COLOR_RESET);
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

    puts(COLOR_YELLOW "Analyzing..." COLOR_RESET);
    
    char *data = read_smartctl_output(fp);
    parse_disk_data(data);

    status = pclose(fp);

    if(data == NULL){
        fprintf(stderr, COLOR_RED "Could not read smartctl output for %s\n" COLOR_RESET, dev_path);
        return;
    }

    //printf("%s\n", data);
    free(data);

    if(status != 0)
        printf(COLOR_YELLOW "Warning: smartctl exited with a nonull status.\n" COLOR_RESET);
}
