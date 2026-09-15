#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <errno.h>
#include <unistd.h>

#include "cJSON.h"
#include "dd_scan.h"
#include "dd_smartctl.h"
#include "dd_parse.h"
#include "dd_print.h"
#include "dd_report.h"

/* smartctl reports its outcome as a bitmask.

   Bits 0-2 mean smartctl itself could not do its job, so there is no report to read.

   Bits 3-7 mean the report is valid and it is the disk that has
   something to report: those are results, not errors.
*/
#define DD_SMARTCTL_FAILED   0x03
#define DD_SMARTCTL_PARTIAL  0x04
#define DD_SMARTCTL_FINDINGS 0xF8
#define DD_SMARTCTL_OPEN_FAILED 0x02
#define DD_SMARTCTL_NOT_FOUND 127
#define DD_SMARTCTL_MAX_ARGS 4

/* SMART/health/self-test data on NVMe live on the controller, not the
   namespace: smartctl on /dev/nvme0n1 can fail to read logs */
static int nvme_controller_name(const char *dev_path, char *out, size_t out_size){
    if(strncmp(dev_path, "nvme", 4) != 0 || !isdigit((unsigned char)dev_path[4]))
        return 0;

    const char *p = dev_path + 4;
    while(isdigit((unsigned char)*p)) p++;

    const char *ns = p;
    if(*ns != 'n' || !isdigit((unsigned char)ns[1])) return 0;

    p = ns + 1;
    while(isdigit((unsigned char)*p)) p++;
    if(*p != '\0') return 0;

    size_t ctrl_len = (size_t)(ns - dev_path);
    if(ctrl_len >= out_size) return 0;

    memcpy(out, dev_path, ctrl_len);
    out[ctrl_len] = '\0';
    return 1;
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
            break;
        }
    }

    buffer[length] = '\0';
    return buffer;
}

/* Rereads the temporary file from the beginning and returns its contents.
   Returns a malloc'd string (possibly ""), or NULL on error. */
static char *get_smartctl_errors(int fd)
{
    if(fd < 0)
        return NULL;

    if(lseek(fd, 0, SEEK_SET) == (off_t)-1)
        return NULL;

    size_t cap = 1024, len = 0;
    char *buf = malloc(cap);
    if(!buf)
        return NULL;

    for(;;){
        if(len + 1 >= cap){
            char *tmp = realloc(buf, cap * 2);
            if(!tmp){ free(buf); return NULL; }
            buf = tmp;
            cap *= 2;
        }
        ssize_t n = read(fd, buf + len, cap - len - 1);
        if(n < 0){
            if(errno == EINTR) continue;
            free(buf);
            return NULL;
        }
        if(n == 0) break;
        len += (size_t)n;
    }

    while(len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r'))
        len--;                 
    buf[len] = '\0';

    return buf;
}

/* Opens the scratch file smartctl's stderr is redirected to. It is unlinked
   right away, so it lives only as long as the descriptor and leaves nothing
   behind. TMPDIR comes first, then the usual spots: /tmp can be a full tmpfs
   or mounted read-only, and losing the diagnosis there is the whole point of
   capturing it. Returns the fd, or -1 when no directory would take the file. */
static int open_stderr_capture(void){
    const char *dirs[3];
    size_t count = 0;
 
    const char *tmpdir = getenv("TMPDIR");
    if(tmpdir != NULL && tmpdir[0] == '/')
        dirs[count++] = tmpdir;

    dirs[count++] = "/tmp";
    dirs[count++] = "/var/tmp";

    for(size_t i = 0; i < count; i++){
        char tmpl[PATH_MAX];

        int len = snprintf(tmpl, sizeof tmpl, "%s/diskdoc-err-XXXXXX", dirs[i]);
        if(len < 0 || (size_t)len >= sizeof tmpl) continue;

        int fd = mkstemp(tmpl);
        if(fd >= 0){
            unlink(tmpl);
            return fd;
        }
    }

    return -1;
}

static const char *const smartctl_paths[] = {
    "/usr/sbin/smartctl",
    "/sbin/smartctl",
    "/usr/bin/smartctl",
    "/bin/smartctl",
    "/usr/local/sbin/smartctl",
    "/usr/local/bin/smartctl"
};

/* Runs smartctl with execv. Returns its stdout for the caller to free, 
   or NULL; *status receives the wait status and *err_out the captured 
   stderr when one was asked for. */
static char *run_smartctl(const char *dev_path, const char *const *args, size_t nargs,
                          int *status, char **err_out, bool merge_stderr){
    char target[32];
    char device[DD_NAME_LEN + 8];
    char *argv[DD_SMARTCTL_MAX_ARGS + 3];
    size_t argc = 0;

    *status = -1;
    if(err_out) *err_out = NULL;
    if(nargs > DD_SMARTCTL_MAX_ARGS) return NULL;

    if(!nvme_controller_name(dev_path, target, sizeof target))
        snprintf(target, sizeof target, "%s", dev_path);

    snprintf(device, sizeof device, "/dev/%s", target);

    argv[argc++] = (char *)"smartctl";
    for(size_t i = 0; i < nargs; i++) argv[argc++] = (char *)args[i];
    argv[argc++] = device;
    argv[argc] = NULL;

    int efd = (err_out != NULL) ? open_stderr_capture() : -1;
    int pipefd[2];

    if(pipe(pipefd) != 0){
        if(efd >= 0) close(efd);
        return NULL;
    }

    pid_t pid = fork();
    if(pid < 0){
        close(pipefd[0]);
        close(pipefd[1]);
        if(efd >= 0) close(efd);
        return NULL;
    }

    if(pid == 0){
        close(pipefd[0]);

        if(dup2(pipefd[1], STDOUT_FILENO) < 0) _exit(DD_SMARTCTL_NOT_FOUND);
        if(merge_stderr) dup2(pipefd[1], STDERR_FILENO);
        else if(efd >= 0) dup2(efd, STDERR_FILENO);

        close(pipefd[1]);
        if(efd >= 0) close(efd);

        for(size_t i = 0; i < sizeof smartctl_paths / sizeof smartctl_paths[0]; i++)
            execv(smartctl_paths[i], argv);

        _exit(DD_SMARTCTL_NOT_FOUND);
    }

    close(pipefd[1]);

    FILE *fp = fdopen(pipefd[0], "r");
    char *data = NULL;

    if(fp != NULL){
        data = read_smartctl_output(fp);
        fclose(fp);
    }else{
        close(pipefd[0]);
    }

    if(waitpid(pid, status, 0) < 0) *status = -1;

    if(efd >= 0){
        if(err_out) *err_out = get_smartctl_errors(efd);
        close(efd);
    }

    if(*status != -1 && WIFEXITED(*status) && WEXITSTATUS(*status) == DD_SMARTCTL_NOT_FOUND){
        fprintf(stderr, "%ssmartctl was not found, install smartmontools%s\n",
                dd_color(stderr, DD_C_RED), dd_color(stderr, DD_C_RESET));
        free(data);
        return NULL;
    }

    return data;
}

/* Runs smartctl and parses its JSON output. Returns the parsed root, or NULL.
   The caller owns the result and must cJSON_Delete it. */
static cJSON* run_smartctl_json(const char *dev_path, const char *const *args, size_t nargs,
                                char **err_out)
{
    int status = 0;
    char *data = run_smartctl(dev_path, args, nargs, &status, err_out, false);

    if(data == NULL){
        fprintf(stderr, "%sCould not read smartctl output for %s%s\n",
                dd_color(stderr, DD_C_RED), dev_path, dd_color(stderr, DD_C_RESET));
        return NULL;
    }

    if(status == -1)
        perror("waitpid");
    else if(WIFSIGNALED(status))
        fprintf(stderr, "%ssmartctl was killed by signal %d%s\n",
                dd_color(stderr, DD_C_RED), WTERMSIG(status), dd_color(stderr, DD_C_RESET));

    cJSON *root = cJSON_Parse(data);
    free(data);

    if(!root){
        fprintf(stderr, "%sFailed to parse smartctl output for %s%s\n",
                dd_color(stderr, DD_C_RED), dev_path, dd_color(stderr, DD_C_RESET));
        return NULL;
    }

    return root;
}

/* Runs smartctl and returns its stdout and stderr merged, as a string the
   caller must free, or NULL. *proc_status receives the wait status, for the
   caller to inspect with WIFEXITED and friends. */
static char* run_smartctl_text(const char *dev_path, const char *const *args, size_t nargs,
                               int *proc_status){
    char *data = run_smartctl(dev_path, args, nargs, proc_status, NULL, true);

    if(data == NULL)
        fprintf(stderr, "%sCould not read smartctl output for %s%s\n",
                dd_color(stderr, DD_C_RED), dev_path, dd_color(stderr, DD_C_RESET));

    return data;
}

/* Prints the messages smartctl attached to its report, in the given color */
static void print_smartctl_messages(cJSON *smartctl, FILE *stream, dd_color_id color){
    cJSON *messages = cJSON_GetObjectItemCaseSensitive(smartctl, "messages");
    if(!cJSON_IsArray(messages)) return;

    cJSON *message;
    cJSON_ArrayForEach(message, messages){
        cJSON *string = cJSON_GetObjectItemCaseSensitive(message, "string");
        if(cJSON_IsString(string))
            fprintf(stream, "%s  %s%s\n",
                    dd_color(stream, color), string->valuestring, dd_color(stream, DD_C_RESET));
    }
}

/* Prints what smartctl left on stderr, then frees it. */
static void print_smartctl_errors(const char *dev_path, char *err){
    if(err == NULL)
        fprintf(stderr, "%s  (smartctl's error output could not be captured)%s\n",
                dd_color(stderr, DD_C_YELLOW), dd_color(stderr, DD_C_RESET));
    else if(err[0] != '\0')
        fprintf(stderr, "%ssmartctl reported on %s:\n%s%s\n",
                dd_color(stderr, DD_C_RED), dev_path, err, dd_color(stderr, DD_C_RESET));

    free(err);
}

/* Decodes the smartctl exit bitmask and prints what it reported: fatal
   errors on stderr, partial reads and findings on stdout.
   Returns 1 when the report can be trusted, 0 when there is no report at all. */
static int check_smartctl_status(cJSON *root){
    static const char *const findings[] = {
        "SMART status reports the disk is FAILING",
        "some prefail attributes are below the threshold",
        "some attributes were below the threshold in the past",
        "the device error log contains records",
        "the self test log contains errors",
    };

    cJSON *smartctl = cJSON_GetObjectItemCaseSensitive(root, "smartctl");
    if(smartctl == NULL) return 1;

    cJSON *exit_status = cJSON_GetObjectItemCaseSensitive(smartctl, "exit_status");
    if(!cJSON_IsNumber(exit_status)) return 1;

    unsigned status = (unsigned)cJSON_GetNumberValue(exit_status);

    if(status & DD_SMARTCTL_FAILED){
        fprintf(stderr, "%ssmartctl could not read this device (0x%02x):%s\n",
                dd_color(stderr, DD_C_RED), status, dd_color(stderr, DD_C_RESET));
        print_smartctl_messages(smartctl, stderr, DD_C_RED);

        if(status & DD_SMARTCTL_OPEN_FAILED)
            fprintf(stderr, "%sReading SMART data usually requires root, try again with sudo.%s\n",
                    dd_color(stderr, DD_C_YELLOW), dd_color(stderr, DD_C_RESET));
        return 0;
    }

    if(status & DD_SMARTCTL_PARTIAL){
        printf("%sSome data could not be read (0x%02x):%s\n",
               dd_color(stdout, DD_C_YELLOW), status, dd_color(stdout, DD_C_RESET));
        print_smartctl_messages(smartctl, stdout, DD_C_YELLOW);
    }

    if(status & DD_SMARTCTL_FINDINGS){
        printf("%sFindings reported by smartctl (0x%02x):%s\n",
               dd_color(stdout, DD_C_YELLOW), status, dd_color(stdout, DD_C_RESET));

        for(size_t i = 0; i < sizeof findings / sizeof findings[0]; i++)
            if(status & (1u << (i + 3)))
                printf("%s  - %s%s\n",
                       dd_color(stdout, DD_C_YELLOW), findings[i], dd_color(stdout, DD_C_RESET));
    }

    return 1;
}

/* Runs smartctl on dev_path, parses its JSON output, and prints the disk report. */
int analyze_disk(const char *dev_path, bool print_report){
    printf("%sAnalyzing /dev/%s...%s\n",
           dd_color(stdout, DD_C_YELLOW), dev_path, dd_color(stdout, DD_C_RESET));
    
    static const char *const args[] = { "-x", "-j" };

    char *err = NULL;
    cJSON *root = run_smartctl_json(dev_path, args, 2, &err);
    if(root == NULL){
        print_smartctl_errors(dev_path, err);
        return dd_exit_code(DD_ALARM);
    }
 
    free(err);

    int exit_code = dd_exit_code(DD_ALARM);

    if(check_smartctl_status(root)){
        dd_report report = {0};
        build_disk_report(root, &report);
        if(print_report) print_report_text(&report);
        exit_code = dd_exit_code(dd_report_worst(&report));
    }

    cJSON_Delete(root);
    return exit_code;
}

cJSON *analyze_disk_raw(const char *dev_path){
    printf("%sAnalyzing /dev/%s...%s\n",
           dd_color(stdout, DD_C_YELLOW), dev_path, dd_color(stdout, DD_C_RESET));

    static const char *const args[] = { "-a", "-j" };

    char *err = NULL;
    cJSON *root = run_smartctl_json(dev_path, args, 2, &err);
    if(root == NULL){
        print_smartctl_errors(dev_path, err);
        return NULL;
    }

    free(err);
    return root;
}

/* Run a self-test on dev/<target>, checking for any command-related errors. */ 
int start_self_test(const char *dev_path, const char *mode){
    const char *args[] = { "-t", mode };

    int status = 0;
    char *output = run_smartctl_text(dev_path, args, 2, &status);
    if(output == NULL) return dd_exit_code(DD_ALARM);

    int exit_code = dd_exit_code(DD_ALARM);

    if(status == -1){
        perror("waitpid");
    }else if(WIFSIGNALED(status)){
        fprintf(stderr, "%ssmartctl was killed by signal %d%s\n",
                dd_color(stderr, DD_C_RED), WTERMSIG(status), dd_color(stderr, DD_C_RESET));
    }else if(WIFEXITED(status)){
        unsigned code = (unsigned)WEXITSTATUS(status);

        if(code & DD_SMARTCTL_FAILED){
            fprintf(stderr, "%ssmartctl could not start the self-test on %s (0x%02x):\n%s%s",
                    dd_color(stderr, DD_C_RED), dev_path, code, output, dd_color(stderr, DD_C_RESET));
            if(code & DD_SMARTCTL_OPEN_FAILED)
                fprintf(stderr, "%sReading SMART data usually requires root, try again with sudo.%s\n",
                        dd_color(stderr, DD_C_YELLOW), dd_color(stderr, DD_C_RESET));
        }else if(strstr(output, "not supported") != NULL){
            fprintf(stderr, "%sThis device does not support self-tests.%s\n",
                    dd_color(stderr, DD_C_RED), dd_color(stderr, DD_C_RESET));
        }else if(strstr(output, "Self-test has begun") != NULL){
            exit_code = dd_exit_code(DD_OK);
        }else{
            fprintf(stderr, "%sUnexpected smartctl output, self-test status unknown:\n%s%s",
                    dd_color(stderr, DD_C_YELLOW), output, dd_color(stderr, DD_C_RESET));
        }
    }

    free(output);
    return exit_code;
}
