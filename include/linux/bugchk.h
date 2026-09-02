#ifndef BUGCHK_HEADER_H
#define BUGCHK_HEADER_H

typedef struct bugchk_info {
    u8 kernel_bug;
    char fault_type[32];
    pid_t pid;
    char task_name[32];
} bugchk_info_t;

extern bugchk_info_t global_bugchk_info;

#endif
