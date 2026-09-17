// SPDX-License-Identifier: GPL-2.0
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>
#include <linux/bugchk.h>

static DEFINE_SPINLOCK(bugchk_info_lock);

#define MAX_LOG_LENGTH 64
static bugchk_info_t bugchk_info_list[MAX_LOG_LENGTH];
static u64 bugchk_i = 0;

void log_bugchk_info(const char *fault_type, pid_t pid, const char *task_name)
{
	spin_lock(&bugchk_info_lock);
	if (bugchk_i > MAX_LOG_LENGTH - 1)
		goto fin;
	
	bugchk_info_t *info = bugchk_info_list + bugchk_i;
	strlcpy(info->fault_type, fault_type, sizeof(info->fault_type));
	info->pid = pid;
	strlcpy(info->task_name, task_name, sizeof(info->task_name));
	
	fin:
	bugchk_i++;
	spin_unlock(&bugchk_info_lock);
}

static int bugchk_show(struct seq_file *m, void *v)
{
	spin_lock(&bugchk_info_lock);
	seq_printf(m, "bugchk count: %llu, exceeded: %s\n", bugchk_i, bugchk_i > MAX_LOG_LENGTH - 1 ? "yes" : "no");
	u64 iter_count = bugchk_i < MAX_LOG_LENGTH ? bugchk_i : MAX_LOG_LENGTH;
	for (u64 i = 0; i < iter_count; i++) {
		bugchk_info_t *info = &bugchk_info_list[i];
		seq_printf(m, "- bugchk[%llu]: fault_type=%s, pid=%llu, task_name=%s\n", i, info->fault_type, info->pid, info->task_name);
	}
	spin_unlock(&bugchk_info_lock);
	return 0;
}

static int __init bugchk_init(void)
{
	proc_create_single("bugchk", 0444, NULL, bugchk_show);
	return 0;
}
fs_initcall(bugchk_init);
