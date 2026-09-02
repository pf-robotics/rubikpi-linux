#ifndef BUGCHK_HEADER_H
#define BUGCHK_HEADER_H

typedef struct bugchk_info {
    char fault_type[32];
    u64 pid;
    char task_name[32];
} bugchk_info_t;

void log_bugchk_info(const char *fault_type, u64 pid, const char *task_name);

#endif
