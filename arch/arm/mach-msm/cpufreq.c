/* arch/arm/mach-msm/cpufreq.c
 *
 * MSM architecture cpufreq driver
 *
 * Copyright (C) 2007 Google, Inc.
 * Copyright (c) 2007-2014, The Linux Foundation. All rights reserved.
 * Author: Mike A. Chan <mikechan@google.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the
 * GNU General Public License for more details.
 *
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/cpufreq.h>
#include <linux/workqueue.h>
#include <linux/completion.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/sched.h>
#include <linux/suspend.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <mach/cpufreq.h>

#ifdef CONFIG_DEBUG_FS
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <asm/div64.h>
#endif

#ifdef CONFIG_LGE_LIMIT_FREQ_TABLE
#include <mach/board_lge.h>
#endif

#ifdef CONFIG_LGE_PM_BATTERY_ID_CHECKER
#include <linux/power/lge_battery_id.h>
#include <mach/msm_smsm.h>
#endif

#ifdef CONFIG_CPU_VOLTAGE_TABLE
static struct cpufreq_frequency_table *dts_freq_table;
#endif

static DEFINE_MUTEX(l2bw_lock);

static struct clk *cpu_clk[NR_CPUS];
static struct clk *l2_clk;
static unsigned int freq_index[NR_CPUS];
static unsigned int max_freq_index;
static struct cpufreq_frequency_table *freq_table;
static unsigned int *l2_khz;
static unsigned long *mem_bw;
static bool hotplug_ready;

struct cpufreq_work_struct {
	struct work_struct work;
	struct cpufreq_policy *policy;
	struct completion complete;
	int frequency;
	unsigned int index;
	int status;
};

static DEFINE_PER_CPU(struct cpufreq_work_struct, cpufreq_work);
static struct workqueue_struct *msm_cpufreq_wq;

struct cpufreq_suspend_t {
	struct mutex suspend_mutex;
	int device_suspended;
};

static DEFINE_PER_CPU(struct cpufreq_suspend_t, cpufreq_suspend);

#ifdef CONFIG_MSM_CPUFREQ_LIMITER
static unsigned int upper_limit_freq[NR_CPUS] = {2265600, 2265600,
						2265600, 2265600};
static unsigned int lower_limit_freq[NR_CPUS] = {0, 0, 0, 0};

unsigned int get_cpu_min_lock(unsigned int cpu)
{
	if (cpu >= 0 && cpu < NR_CPUS)
		return lower_limit_freq[cpu];
	else
		return 0;
}
EXPORT_SYMBOL(get_cpu_min_lock);

void set_cpu_min_lock(unsigned int cpu, int freq)
{
	if (cpu >= 0 && cpu < NR_CPUS) {
		if (freq <= 300000 || freq > 2803200)
			lower_limit_freq[cpu] = 0;
		else
			lower_limit_freq[cpu] = freq;
	}
}
EXPORT_SYMBOL(set_cpu_min_lock);

unsigned int get_max_lock(unsigned int cpu)
{
	if (cpu >= 0 && cpu < NR_CPUS)
		return upper_limit_freq[cpu];
	else
		return 0;
}
EXPORT_SYMBOL(get_max_lock);

void set_max_lock(unsigned int cpu, unsigned int freq)
{
	if (cpu >= 0 && cpu <= NR_CPUS) {
		if (freq <= 300000 || freq > 2803200)
			upper_limit_freq[cpu] = 0;
		else
			upper_limit_freq[cpu] = freq;
	}
}
EXPORT_SYMBOL(set_max_lock);
#endif

unsigned long msm_cpufreq_get_bw(void)
{
	return mem_bw[max_freq_index];
}

static void update_l2_bw(int *also_cpu)
{
	int rc = 0, cpu;
	unsigned int index = 0;

	mutex_lock(&l2bw_lock);

	if (also_cpu)
		index = freq_index[*also_cpu];

	for_each_online_cpu(cpu) {
		index = max(index, freq_index[cpu]);
	}

	if (l2_clk)
		rc = clk_set_rate(l2_clk, l2_khz[index] * 1000);
	if (rc) {
		pr_err("Error setting L2 clock rate!\n");
		goto out;
	}

	max_freq_index = index;
	rc = devfreq_msm_cpufreq_update_bw();
	if (rc)
		pr_err("Unable to update BW (%d)\n", rc);

out:
	mutex_unlock(&l2bw_lock);
}

static int set_cpu_freq(struct cpufreq_policy *policy, unsigned int new_freq,
			unsigned int index)
{
	int ret = 0;
	int saved_sched_policy = -EINVAL;
	int saved_sched_rt_prio = -EINVAL;
	struct cpufreq_freqs freqs;
	struct sched_param param = { .sched_priority = MAX_RT_PRIO-1 };
	unsigned long rate;
#ifdef CONFIG_MSM_CPUFREQ_LIMITER
	unsigned int ll_freq = lower_limit_freq[policy->cpu];
	unsigned int ul_freq = upper_limit_freq[policy->cpu];

	if (ll_freq || ul_freq) {
		unsigned int t_freq = new_freq;

		if (ll_freq && new_freq < ll_freq)
			t_freq = ll_freq;

		if (ul_freq && new_freq > ul_freq)
			t_freq = ul_freq;

		new_freq = t_freq;

		if (new_freq < policy->min)
			new_freq = policy->min;
		if (new_freq > policy->max)
			new_freq = policy->max;
	}
#endif

	freqs.old = policy->cur;
	freqs.new = new_freq;
	freqs.cpu = policy->cpu;

	/*
	 * Put the caller into SCHED_FIFO priority to avoid cpu starvation
	 * while increasing frequencies
	 */

	if (freqs.new > freqs.old && current->policy != SCHED_FIFO) {
		saved_sched_policy = current->policy;
		saved_sched_rt_prio = current->rt_priority;
		sched_setscheduler_nocheck(current, SCHED_FIFO, &param);
	}

	cpufreq_notify_transition(&freqs, CPUFREQ_PRECHANGE);

	rate = new_freq * 1000;
	rate = clk_round_rate(cpu_clk[policy->cpu], rate);
	ret = clk_set_rate(cpu_clk[policy->cpu], rate);
	if (!ret) {
		freq_index[policy->cpu] = index;
		update_l2_bw(NULL);
		cpufreq_notify_transition(&freqs, CPUFREQ_POSTCHANGE);
	}

	/* Restore priority after clock ramp-up */
	if (freqs.new > freqs.old && saved_sched_policy >= 0) {
		param.sched_priority = saved_sched_rt_prio;
		sched_setscheduler_nocheck(current, saved_sched_policy, &param);
	}
	return ret;
}

static void set_cpu_work(struct work_struct *work)
{
	struct cpufreq_work_struct *cpu_work =
		container_of(work, struct cpufreq_work_struct, work);

	cpu_work->status = set_cpu_freq(cpu_work->policy, cpu_work->frequency,
					cpu_work->index);
	complete(&cpu_work->complete);
}

static int msm_cpufreq_target(struct cpufreq_policy *policy,
				unsigned int target_freq,
				unsigned int relation)
{
	int ret = -EFAULT;
	int index;
	struct cpufreq_frequency_table *table;
	struct cpufreq_work_struct *cpu_work = NULL;

	mutex_lock(&per_cpu(cpufreq_suspend, policy->cpu).suspend_mutex);

	if (per_cpu(cpufreq_suspend, policy->cpu).device_suspended) {
		pr_debug("cpufreq: cpu%d scheduling frequency change "
				"in suspend.\n", policy->cpu);
		ret = -EFAULT;
		goto done;
	}

	table = cpufreq_frequency_get_table(policy->cpu);
	if (cpufreq_frequency_table_target(policy, table, target_freq, relation,
			&index)) {
		pr_err("cpufreq: invalid target_freq: %d\n", target_freq);
		ret = -EINVAL;
		goto done;
	}

	pr_debug("CPU[%d] target %d relation %d (%d-%d) selected %d\n",
		policy->cpu, target_freq, relation,
		policy->min, policy->max, table[index].frequency);

	cpu_work = &per_cpu(cpufreq_work, policy->cpu);
	cpu_work->policy = policy;
	cpu_work->frequency = table[index].frequency;
	cpu_work->index = table[index].index;
	cpu_work->status = -ENODEV;

	cancel_work_sync(&cpu_work->work);
	INIT_COMPLETION(cpu_work->complete);
	queue_work_on(policy->cpu, msm_cpufreq_wq, &cpu_work->work);
	wait_for_completion(&cpu_work->complete);

	ret = cpu_work->status;

done:
	mutex_unlock(&per_cpu(cpufreq_suspend, policy->cpu).suspend_mutex);
	return ret;
}

static int msm_cpufreq_verify(struct cpufreq_policy *policy)
{
	cpufreq_verify_within_limits(policy, policy->cpuinfo.min_freq,
			policy->cpuinfo.max_freq);
	return 0;
}

static unsigned int msm_cpufreq_get_freq(unsigned int cpu)
{
	return clk_get_rate(cpu_clk[cpu]) / 1000;
}

static int __cpuinit msm_cpufreq_init(struct cpufreq_policy *policy)
{
	int cur_freq;
	int index;
	int ret = 0;
	struct cpufreq_frequency_table *table;
	struct cpufreq_work_struct *cpu_work = NULL;
	int cpu;

	table = cpufreq_frequency_get_table(policy->cpu);
	if (table == NULL)
		return -ENODEV;

 	/*
	 * In some SoC, some cores are clocked by same source, and their
	 * frequencies can not be changed independently. Find all other
	 * CPUs that share same clock, and mark them as controlled by
	 * same policy.
	 */
	for_each_possible_cpu(cpu)
		if (cpu_clk[cpu] == cpu_clk[policy->cpu])
			cpumask_set_cpu(cpu, policy->cpus);

	cpu_work = &per_cpu(cpufreq_work, policy->cpu);
	INIT_WORK(&cpu_work->work, set_cpu_work);
	init_completion(&cpu_work->complete);

	if (cpufreq_frequency_table_cpuinfo(policy, table)) {
#ifdef CONFIG_MSM_CPU_FREQ_SET_MIN_MAX
		policy->cpuinfo.min_freq = CONFIG_MSM_CPU_FREQ_MIN;
		policy->cpuinfo.max_freq = CONFIG_MSM_CPU_FREQ_MAX;
#endif
	}
#ifdef CONFIG_MSM_CPU_FREQ_SET_MIN_MAX
	policy->min = CONFIG_MSM_CPU_FREQ_MIN;
	policy->max = CONFIG_MSM_CPU_FREQ_MAX;
#else
#ifdef CONFIG_ARCH_MSM8974
	policy->max = 2265600;
	policy->min = 300000;
#endif
#endif

	cur_freq = clk_get_rate(cpu_clk[policy->cpu])/1000;

	if (cpufreq_frequency_table_target(policy, table, cur_freq,
	    CPUFREQ_RELATION_H, &index) &&
	    cpufreq_frequency_table_target(policy, table, cur_freq,
	    CPUFREQ_RELATION_L, &index)) {
		pr_info("cpufreq: cpu%d at invalid freq: %d\n",
				policy->cpu, cur_freq);
		return -EINVAL;
	}
	/*
	 * Call set_cpu_freq unconditionally so that when cpu is set to
	 * online, frequency limit will always be updated.
	 */
	ret = set_cpu_freq(policy, table[index].frequency, table[index].index);
	if (ret)
		return ret;
	pr_debug("cpufreq: cpu%d init at %d switching to %d\n",
			policy->cpu, cur_freq, table[index].frequency);
	policy->cur = table[index].frequency;

	return 0;
}

static int __cpuinit msm_cpufreq_cpu_callback(struct notifier_block *nfb,
		unsigned long action, void *hcpu)
{
	unsigned int cpu = (unsigned long)hcpu;
	int rc;

	/* Fail hotplug until this driver can get CPU clocks */
	if (!hotplug_ready)
		return NOTIFY_BAD;

	switch (action & ~CPU_TASKS_FROZEN) {
	/*
	 * Scale down clock/power of CPU that is dead and scale it back up
	 * before the CPU is brought up.
	 */
	case CPU_DEAD:
		clk_disable_unprepare(cpu_clk[cpu]);
		clk_disable_unprepare(l2_clk);
		update_l2_bw(NULL);
		break;
	case CPU_UP_CANCELED:
		clk_unprepare(cpu_clk[cpu]);
		clk_unprepare(l2_clk);
		update_l2_bw(NULL);
		break;
	case CPU_UP_PREPARE:
		rc = clk_prepare(l2_clk);
		if (rc < 0)
			return NOTIFY_BAD;
		rc = clk_prepare(cpu_clk[cpu]);
		if (rc < 0) {
			clk_unprepare(l2_clk);
			return NOTIFY_BAD;
		}
		update_l2_bw(&cpu);
		break;
	case CPU_STARTING:
		rc = clk_enable(l2_clk);
		if (rc < 0)
			return NOTIFY_BAD;
		rc = clk_enable(cpu_clk[cpu]);
		if (rc) {
			clk_disable(l2_clk);
			return NOTIFY_BAD;
		}
		break;
	default:
		break;
	}

	return NOTIFY_OK;
}

static struct notifier_block __refdata msm_cpufreq_cpu_notifier = {
	.notifier_call = msm_cpufreq_cpu_callback,
};

static int msm_cpufreq_suspend(void)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		mutex_lock(&per_cpu(cpufreq_suspend, cpu).suspend_mutex);
		per_cpu(cpufreq_suspend, cpu).device_suspended = 1;
		mutex_unlock(&per_cpu(cpufreq_suspend, cpu).suspend_mutex);
	}

	return NOTIFY_DONE;
}

static int msm_cpufreq_resume(void)
{
	int cpu, ret;
	struct cpufreq_policy policy;

	for_each_possible_cpu(cpu) {
		per_cpu(cpufreq_suspend, cpu).device_suspended = 0;
	}

	/*
	 * Freq request might be rejected during suspend, resulting
	 * in policy->cur violating min/max constraint.
	 * Correct the frequency as soon as possible.
	 */
	for_each_online_cpu(cpu) {
		ret = cpufreq_get_policy(&policy, cpu);
		if (ret)
			continue;
		if (policy.cur <= policy.max && policy.cur >= policy.min)
			continue;
		ret = cpufreq_update_policy(cpu);
		if (ret)
			pr_info("cpufreq: Current frequency violates policy min/max for CPU%d\n",
			       cpu);
		else
			pr_info("cpufreq: Frequency violation fixed for CPU%d\n",
				cpu);
	}

	return NOTIFY_DONE;
}

static int msm_cpufreq_pm_event(struct notifier_block *this,
				unsigned long event, void *ptr)
{
	switch (event) {
	case PM_POST_HIBERNATION:
	case PM_POST_SUSPEND:
		return msm_cpufreq_resume();
	case PM_HIBERNATION_PREPARE:
	case PM_SUSPEND_PREPARE:
		return msm_cpufreq_suspend();
	default:
		return NOTIFY_DONE;
	}
}

static struct notifier_block msm_cpufreq_pm_notifier = {
	.notifier_call = msm_cpufreq_pm_event,
};

static struct freq_attr *msm_freq_attr[] = {
	&cpufreq_freq_attr_scaling_available_freqs,
	NULL,
};

static struct cpufreq_driver msm_cpufreq_driver = {
	/* lps calculations are handled here. */
	.flags		= CPUFREQ_STICKY | CPUFREQ_CONST_LOOPS,
	.init		= msm_cpufreq_init,
	.verify		= msm_cpufreq_verify,
	.target		= msm_cpufreq_target,
	.get		= msm_cpufreq_get_freq,
	.name		= "msm",
	.attr		= msm_freq_attr,
};

#ifdef CONFIG_LGE_LIMIT_FREQ_TABLE
#define PROP_FACT_TBL "lge,cpufreq-factory-table"
#endif

#define PROP_TBL "qcom,cpufreq-table"
static int cpufreq_parse_dt(struct device *dev)
{
	int ret, len, nf, num_cols = 2, i, j;
	u32 *data;

#ifdef CONFIG_LGE_PM_BATTERY_ID_CHECKER
    uint *smem_batt = 0;
    int IsBattery = 0;
#endif

#ifdef CONFIG_LGE_LIMIT_FREQ_TABLE
	enum lge_boot_mode_type boot_mode = lge_get_boot_mode();
#endif

	if (l2_clk)
		num_cols++;

#ifdef CONFIG_LGE_PM_BATTERY_ID_CHECKER
    smem_batt = (uint *)smem_alloc(SMEM_BATT_INFO, sizeof(smem_batt));
    if (smem_batt == NULL) {
        pr_err("%s : smem_alloc returns NULL\n",__func__);
    }
    else {
        pr_err("Batt ID from SBL = %d\n", *smem_batt);
        if (*smem_batt == BATT_ID_DS2704_L ||
            *smem_batt == BATT_ID_DS2704_C ||
            *smem_batt == BATT_ID_ISL6296_L ||
            *smem_batt == BATT_ID_ISL6296_C) {
            //To Do if Battery is present
            IsBattery = 1;
        }
        else {
            //To Do if Battery is absent
            IsBattery = 0;
        }
    }
#endif

#ifdef CONFIG_LGE_LIMIT_FREQ_TABLE
	if(boot_mode == LGE_BOOT_MODE_FACTORY || boot_mode == LGE_BOOT_MODE_PIFBOOT
#ifdef CONFIG_LGE_PM_BATTERY_ID_CHECKER
	   || IsBattery == 0
#endif
		) {
		/* XXX: MSM8974v1 is not considered */
		/* Parse CPU freq -> L2/Mem BW map table. */
		if (!of_find_property(dev->of_node, PROP_FACT_TBL, &len))
			return -EINVAL;
		len /= sizeof(*data);
	} else {
		/* Parse CPU freq -> L2/Mem BW map table. */
		if (!of_find_property(dev->of_node, PROP_TBL, &len))
			return -EINVAL;
		len /= sizeof(*data);
	}
#else /* qmc original */
	/* Parse CPU freq -> L2/Mem BW map table. */
	if (!of_find_property(dev->of_node, PROP_TBL, &len))
		return -EINVAL;
	len /= sizeof(*data);
#endif

	if (len % num_cols || len == 0)
		return -EINVAL;
	nf = len / num_cols;

	data = devm_kzalloc(dev, len * sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

#ifdef CONFIG_LGE_LIMIT_FREQ_TABLE
	if(boot_mode == LGE_BOOT_MODE_FACTORY || boot_mode == LGE_BOOT_MODE_PIFBOOT
#ifdef CONFIG_LGE_PM_BATTERY_ID_CHECKER
	   || IsBattery == 0
#endif
		) {
		ret = of_property_read_u32_array(dev->of_node, PROP_FACT_TBL, data, len);
		if (ret)
			return ret;
	} else {
		ret = of_property_read_u32_array(dev->of_node, PROP_TBL, data, len);
		if (ret)
			return ret;
	}
#else /* qmc original */
	ret = of_property_read_u32_array(dev->of_node, PROP_TBL, data, len);
	if (ret)
		return ret;
#endif

	/* Allocate all data structures. */
	freq_table = devm_kzalloc(dev, (nf + 1) * sizeof(*freq_table),
				  GFP_KERNEL);
	mem_bw = devm_kzalloc(dev, nf * sizeof(*mem_bw), GFP_KERNEL);

	if (!freq_table || !mem_bw)
		return -ENOMEM;

	if (l2_clk) {
		l2_khz = devm_kzalloc(dev, nf * sizeof(*l2_khz), GFP_KERNEL);
		if (!l2_khz)
			return -ENOMEM;
	}

	j = 0;
	for (i = 0; i < nf; i++) {
		unsigned long f;

		f = clk_round_rate(cpu_clk[0], data[j++] * 1000);
		if (IS_ERR_VALUE(f))
			break;
		f /= 1000;

		/*
		 * Check if this is the last feasible frequency in the table.
		 *
		 * The table listing frequencies higher than what the HW can
		 * support is not an error since the table might be shared
		 * across CPUs in different speed bins. It's also not
		 * sufficient to check if the rounded rate is lower than the
		 * requested rate as it doesn't cover the following example:
		 *
		 * Table lists: 2.2 GHz and 2.5 GHz.
		 * Rounded rate returns: 2.2 GHz and 2.3 GHz.
		 *
		 * In this case, we can CPUfreq to use 2.2 GHz and 2.3 GHz
		 * instead of rejecting the 2.5 GHz table entry.
		 */
		if (i > 0 && f <= freq_table[i-1].frequency)
			break;

		freq_table[i].index = i;
		freq_table[i].frequency = f;

		if (l2_clk) {
			f = clk_round_rate(l2_clk, data[j++] * 1000);
			if (IS_ERR_VALUE(f)) {
				pr_err("Error finding L2 rate for CPU %d KHz\n",
					freq_table[i].frequency);
				freq_table[i].frequency = CPUFREQ_ENTRY_INVALID;
			} else {
				f /= 1000;
				l2_khz[i] = f;
			}
		}

		mem_bw[i] = data[j++];
	}

	freq_table[i].index = i;
	freq_table[i].frequency = CPUFREQ_TABLE_END;

#ifdef CONFIG_CPU_VOLTAGE_TABLE
	dts_freq_table =
		devm_kzalloc(dev, (nf + 1) *
			sizeof(struct cpufreq_frequency_table),
			GFP_KERNEL);

	if (!dts_freq_table)
		return -ENOMEM;

	for (i = 0, j = 0; i < nf; i++, j += 3)
		dts_freq_table[i].frequency = data[j];
	dts_freq_table[i].frequency = CPUFREQ_TABLE_END;
#endif

	devm_kfree(dev, data);

	return 0;
}

#ifdef CONFIG_CPU_VOLTAGE_TABLE
bool is_used_by_scaling(unsigned int freq)
{
	unsigned int i, cpu_freq;

	for (i = 0; dts_freq_table[i].frequency != CPUFREQ_TABLE_END; i++) {
		cpu_freq = dts_freq_table[i].frequency;
		if (cpu_freq == CPUFREQ_ENTRY_INVALID)
			continue;
		if (freq == cpu_freq)
			return true;
	}
	return false;
}
#endif

#ifdef CONFIG_DEBUG_FS
static int msm_cpufreq_show(struct seq_file *m, void *unused)
{
	unsigned int i, cpu_freq;

	if (!freq_table)
		return 0;

	seq_printf(m, "%10s%10s", "CPU (KHz)", "L2 (KHz)");
	seq_printf(m, "%12s\n", "Mem (MBps)");

	for (i = 0; freq_table[i].frequency != CPUFREQ_TABLE_END; i++) {
		cpu_freq = freq_table[i].frequency;
		if (cpu_freq == CPUFREQ_ENTRY_INVALID)
			continue;
		seq_printf(m, "%10d", cpu_freq);
		seq_printf(m, "%10d", l2_khz ? l2_khz[i] : cpu_freq);
		seq_printf(m, "%12lu", mem_bw[i]);
		seq_printf(m, "\n");
	}
	return 0;
}

static int msm_cpufreq_open(struct inode *inode, struct file *file)
{
	return single_open(file, msm_cpufreq_show, inode->i_private);
}

const struct file_operations msm_cpufreq_fops = {
	.open		= msm_cpufreq_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= seq_release,
};
#endif

static int __init msm_cpufreq_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	char clk_name[] = "cpu??_clk";
	struct clk *c;
	int cpu, ret;

	l2_clk = devm_clk_get(dev, "l2_clk");
	if (IS_ERR(l2_clk))
		l2_clk = NULL;

	for_each_possible_cpu(cpu) {
		snprintf(clk_name, sizeof(clk_name), "cpu%d_clk", cpu);
		c = devm_clk_get(dev, clk_name);
		if (IS_ERR(c))
			return PTR_ERR(c);
		cpu_clk[cpu] = c;
	}
	hotplug_ready = true;

	ret = cpufreq_parse_dt(dev);
	if (ret)
		return ret;

	for_each_possible_cpu(cpu) {
		cpufreq_frequency_table_get_attr(freq_table, cpu);
	}

	ret = register_devfreq_msm_cpufreq();
	if (ret) {
		pr_err("devfreq governor registration failed\n");
		return ret;
	}

#ifdef CONFIG_DEBUG_FS
	if (!debugfs_create_file("msm_cpufreq", S_IRUGO, NULL, NULL,
		&msm_cpufreq_fops))
		return -ENOMEM;
#endif

	return 0;
}

static struct of_device_id match_table[] = {
	{ .compatible = "qcom,msm-cpufreq" },
	{}
};

static struct platform_driver msm_cpufreq_plat_driver = {
	.driver = {
		.name = "msm-cpufreq",
		.of_match_table = match_table,
		.owner = THIS_MODULE,
	},
};

static int __init msm_cpufreq_register(void)
{
	int cpu, rc;

	for_each_possible_cpu(cpu) {
		mutex_init(&(per_cpu(cpufreq_suspend, cpu).suspend_mutex));
		per_cpu(cpufreq_suspend, cpu).device_suspended = 0;
	}

	rc = platform_driver_probe(&msm_cpufreq_plat_driver,
				   msm_cpufreq_probe);
	if (rc < 0) {
		/* Unblock hotplug if msm-cpufreq probe fails */
		unregister_hotcpu_notifier(&msm_cpufreq_cpu_notifier);
		for_each_possible_cpu(cpu)
			mutex_destroy(&(per_cpu(cpufreq_suspend, cpu).
					suspend_mutex));
		return rc;
	}

	msm_cpufreq_wq = alloc_workqueue("msm-cpufreq", WQ_HIGHPRI, 0);
	register_pm_notifier(&msm_cpufreq_pm_notifier);
	return cpufreq_register_driver(&msm_cpufreq_driver);
}

subsys_initcall(msm_cpufreq_register);

static int __init msm_cpufreq_early_register(void)
{
	return register_hotcpu_notifier(&msm_cpufreq_cpu_notifier);
}
core_initcall(msm_cpufreq_early_register);
