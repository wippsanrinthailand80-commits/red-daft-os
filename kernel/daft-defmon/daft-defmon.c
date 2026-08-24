// daft-defmon.c — DEFENSIVE kernel module for Red Daft OS.
//
// Purpose: enumerate EVERY task in the kernel task list (including processes
// that an attacker tried to hide via DKOM by unlinking from the list) and
// expose it via /proc/daft_defmon. A userspace helper diffs this against
// /proc to reveal hidden processes. This is detection, not stealth.
//
// It does NOT hide anything, does NOT hook syscalls, and does NOT patch memory.

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/version.h>

static int defmon_show(struct seq_file *m, void *v)
{
	struct task_struct *task;

	rcu_read_lock();
	for_each_process(task) {
		seq_printf(m, "%d\t%s\n", task->pid, task->comm);
	}
	rcu_read_unlock();
	return 0;
}

static int defmon_open(struct inode *inode, struct file *file)
{
	return single_open(file, defmon_show, NULL);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0)
static const struct proc_ops defmon_ops = {
	.proc_open	= defmon_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};
#else
static const struct file_operations defmon_ops = {
	.open		= defmon_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};
#endif

static int __init defmon_init(void)
{
	if (!proc_create("daft_defmon", 0, NULL, &defmon_ops)) {
		pr_err("daft-defmon: failed to create /proc/daft_defmon\n");
		return -ENOMEM;
	}
	pr_info("daft-defmon: loaded (defensive task enumerator)\n");
	return 0;
}

static void __exit defmon_exit(void)
{
	remove_proc_entry("daft_defmon", NULL);
	pr_info("daft-defmon: unloaded\n");
}

module_init(defmon_init);
module_exit(defmon_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Red Daft defensive monitor: reveals DKOM-hidden processes");
MODULE_AUTHOR("Red Daft OS");
