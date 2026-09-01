// SPDX-License-Identifier: GPL-2.0
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/bugchk.h>

bugchk_info_t global_bugchk_info = {
    .kernel_bug = 0,
    .fault_type = "none",
    .pid = -1,
    .task_name = "none"
};

static int bugchk_show(struct seq_file *m, void *v)
{
	seq_printf(m, "kernel_bug: %d\n", global_bugchk_info.kernel_bug);
	seq_printf(m, "fault: %s\n", global_bugchk_info.fault_type);
	seq_printf(m, "pid: %d\n", global_bugchk_info.pid);
	seq_printf(m, "name: %s\n", global_bugchk_info.task_name);
	return 0;
}

static int __init bugchk_init(void)
{
	proc_create_single("bugchk", 0444, NULL, bugchk_show);

	return 0;
}
fs_initcall(bugchk_init);
