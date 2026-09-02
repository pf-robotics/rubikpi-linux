#ifndef BUGCHK_HEADER_H
#define BUGCHK_HEADER_H

typedef struct bugchk_info {
    char fault_type[32];
    pid_t pid;
    char task_name[32];
} bugchk_info_t;

void log_bugchk_info(const char *fault_type, pid_t pid, const char *task_name);

#endif
