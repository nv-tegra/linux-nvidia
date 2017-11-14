/*
 * Copyright (c) 2017, NVIDIA CORPORATION.  All rights reserved.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/types.h>
#include <asm/cputype.h>
#include <asm/smp_plat.h>
#include <asm/cpu.h>
#include <linux/io.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/cpufreq.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <soc/tegra/tegra_bpmp.h>
#include <soc/tegra/bpmp_abi.h>
#include <soc/tegra/chip-id.h>
#include <linux/delay.h>
#include <linux/pstore.h>
#include <linux/ptrace.h>
#include <linux/platform_device.h>
#include <linux/platform/tegra/emc_bwmgr.h>
#include <linux/t19x_mce.h>
#include <linux/version.h>
#include <linux/pm_qos.h>
#include <linux/workqueue.h>

/* cpufreq transisition latency */
#define TEGRA_CPUFREQ_TRANSITION_LATENCY (300 * 1000) /* unit in nanoseconds */

#define KHZ_TO_HZ		1000
#define REF_CLK_MHZ		408 /* 408 MHz */
#define US_DELAY		5000
#define CPUFREQ_TBL_STEP_SIZE	4

#define LOOP_FOR_EACH_CLUSTER(cl)	for (cl = 0; \
					cl < MAX_CLUSTERS; cl++)
#define INDEX_STEP		2

struct cpu_emc_map {
	uint32_t cpufreq; /* unit in KHz */
	uint32_t emcfreq; /* unit in KHz */
};
static struct cpu_emc_map *cpu_emc_map_ptr;
static uint16_t cpu_emc_map_num;

enum cluster {
	CLUSTER0,
	CLUSTER1,
	CLUSTER2,
	CLUSTER3,
	MAX_CLUSTERS,
};

struct cc3_params {
	u32 ndiv;
	u32 freq;
	u8 enable;
};

struct per_cluster_data {
	struct cpufreq_frequency_table *clft;
	struct mrq_cpu_ndiv_limits_response ndiv_limits_tbl;
	struct tegra_bwmgr_client *bwmgr;
	struct cpumask cpu_mask;
	struct cc3_params cc3;
	uint8_t configured;
};

struct tegra_cpufreq_data {
	struct per_cluster_data pcluster[MAX_CLUSTERS];
	struct mutex mlock; /* lock protecting cc3 params */
	uint32_t freq_compute_delay; /* delay in reading clock counters */
	unsigned long emc_max_rate; /* Hz */
};

static struct tegra_cpufreq_data tfreq_data;
struct tegra_cpu_ctr {
	uint32_t cpu;
	uint32_t coreclk_cnt, last_coreclk_cnt;
	uint32_t refclk_cnt, last_refclk_cnt;
};

static struct workqueue_struct *read_counters_wq;
struct read_counters_work {
	struct work_struct work;
	struct tegra_cpu_ctr c;
};

static uint64_t read_freq_feedback(void)
{
	uint64_t val;
	static bool s_val;

	if (tegra_platform_is_sim()) {
		if (!s_val)
			val = (0x1L << 32) | 1;
		else
			val = (0x5L << 32) | 2;
		s_val = !s_val;
	} else
		asm volatile("mrs %0, s3_0_c15_c0_5" : "=r" (val) : );

	return val;
}

static inline uint16_t clamp_ndiv(struct mrq_cpu_ndiv_limits_response *nltbl,
				uint16_t ndiv)
{
	uint16_t min = nltbl->ndiv_min;
	uint16_t max = nltbl->ndiv_max;

	if (!ndiv || (ndiv < min))
		ndiv = min;
	if (ndiv > max)
		ndiv = max;

	return ndiv;
}

static inline uint16_t map_freq_to_ndiv(struct mrq_cpu_ndiv_limits_response
	*nltbl, uint32_t freq)
{
	uint16_t ndiv;

	ndiv = (freq * KHZ_TO_HZ / nltbl->ref_clk_hz) *
		nltbl->pdiv * nltbl->mdiv;

	if ((freq * KHZ_TO_HZ % nltbl->ref_clk_hz) *
		nltbl->pdiv * nltbl->mdiv)
		ndiv++;

	return ndiv;
}

static void tegra_read_counters(struct work_struct *work)
{
	uint64_t val = 0;
	struct tegra_cpu_ctr *c;
	struct read_counters_work *read_counters_work;
	/*
	 * ref_clk_counter(32 bit counter) runs from constant clk,
	 * pll_p(408MHz).
	 * It will take = 2 ^ 32 / 408 MHz to overflow ref clk counter
	 *              = 10526880 usec = 10527 msec to overflow
	 *
	 * Like wise core_clk_counter(32 bit counter) runs from
	 * crab_clk(ctu_clk). ctu_clk, runs at full freq of cluster,
	 * Assuming max cluster clock ~2000MHz
	 * It will take = 2 ^ 32 / 2000 MHz to overflow core clk counter
	 *              = 2 sec to overflow
	 */

	read_counters_work = container_of(work, struct read_counters_work,
					work);
	c = &read_counters_work->c;

	val = read_freq_feedback();
	c->last_refclk_cnt = (uint32_t)(val & 0xffffffff);
	c->last_coreclk_cnt = (uint32_t) (val >> 32);
	udelay(tfreq_data.freq_compute_delay);
	val = read_freq_feedback();
	c->refclk_cnt = (uint32_t)(val & 0xffffffff);
	c->coreclk_cnt = (uint32_t) (val >> 32);
}

/**
 * Return instantaneous cpu speed
 * Instantaneous freq is calculated as -
 * -Takes sample on every query of getting the freq.
 *        - Read core and ref clock counters;
 *        - Delay for X us
 *       -  Read above cycle counters again
 *       - Calculates freq by subtracting current and previous counters
 *          divided by the delay time or eqv. of ref_clk_counter in delta time
 *       - Return Kcycles/second, freq in KHz
 *
 * - delta time period = x sec
 *          = delta ref_clk_counter / (408 * 10^6) sec
 * freq in Hz = cycles/sec
 *                 = (delta cycles / x sec
 *                 = (delta cycles * 408 * 10^6) / delta ref_clk_counter
 *     in KHz = (delta cycles * 408 * 10^3) / delta ref_clk_counter
 *
 * @cpu - logical cpu whose freq to be updated
 * Returns freq in KHz on success, 0 if cpu is offline
 */
static unsigned int tegra194_get_speed(uint32_t cpu)
{
	uint32_t delta_ccnt = 0;
	uint64_t delta_refcnt = 0;
	unsigned long rate_mhz = 0;
	struct tegra_cpu_ctr c;
	struct read_counters_work read_counters_work;

	read_counters_work.c.cpu = cpu;
	INIT_WORK_ONSTACK(&read_counters_work.work, tegra_read_counters);
	queue_work_on(cpu, read_counters_wq, &read_counters_work.work);
	flush_work(&read_counters_work.work);
	c = read_counters_work.c;
	delta_ccnt = c.coreclk_cnt - c.last_coreclk_cnt;
	if (!delta_ccnt)
		goto err_out;

	/* ref clock is 32 bits */
	delta_refcnt = c.refclk_cnt - c.last_refclk_cnt;
	if (!delta_refcnt) {
		pr_err("cpufreq: %d is idle, delta_refcnt: 0\n", cpu);
		goto err_out;
	}
	rate_mhz = ((unsigned long) delta_ccnt * REF_CLK_MHZ) / delta_refcnt;
err_out:
	return (unsigned int) (rate_mhz * 1000); /* in KHz */
}

/**
 * cluster_cpu_to_emc_freq - return emc freq in cpu_emc_map table corresponding
 *                           to cpu rate input
 * @cpu_rate - cpu rate in KHz
 * Returns emc freq in KHz
 */
static unsigned long cluster_cpu_to_emc_freq(uint32_t cpu_rate)
{
	int i;

	for (i = 0; i < cpu_emc_map_num; i++) {
		if (cpu_rate >= cpu_emc_map_ptr[i].cpufreq)
			return cpu_emc_map_ptr[i].emcfreq;
	}
	return 0;
}

/* Set emc clock by referring cpu_to_emc freq mapping */
static void set_cpufreq_to_emcfreq(enum cluster cl, uint32_t cluster_freq)
{
	unsigned long emc_freq;

	emc_freq = cluster_cpu_to_emc_freq(cluster_freq);

	tegra_bwmgr_set_emc(tfreq_data.pcluster[cl].bwmgr,
		emc_freq * KHZ_TO_HZ, TEGRA_BWMGR_SET_EMC_FLOOR);
	pr_debug("cluster %d, emc freq(KHz): %lu cluster_freq(KHz): %u\n",
		cl, emc_freq, cluster_freq);
}

static struct cpufreq_frequency_table *get_freqtable(uint8_t cpu)
{
	enum cluster cur_cl = topology_physical_package_id(cpu);

	return tfreq_data.pcluster[cur_cl].clft;
}

/* Write freq request in ndiv for a cpu */
static void write_ndiv_request(void *val)
{
	uint64_t regval = *((uint64_t *) val);

	if (!tegra_platform_is_sim())
		asm volatile("msr s3_0_c15_c0_4, %0" : : "r" (regval));
}

/* Read freq request in ndiv for a cpu */
static void read_ndiv_request(void *ret)
{
	uint64_t val = 0;

	if (!tegra_platform_is_sim())
		asm volatile("mrs %0, s3_0_c15_c0_4" : "=r" (val) : );
	else
		val = 4;
	*((uint64_t *) ret) = val;
}

/**
 * tegra_update_cpu_speed - update cpu freq
 * @rate - in kHz
 * @cpu - cpu whose freq to be updated
 */
static void tegra_update_cpu_speed(uint32_t rate, uint8_t cpu)
{
	struct mrq_cpu_ndiv_limits_response *nltbl;
	uint64_t val;
	enum cluster cur_cl;
	uint16_t ndiv;

	cur_cl = topology_physical_package_id(cpu);
	nltbl = &tfreq_data.pcluster[cur_cl].ndiv_limits_tbl;

	if (!nltbl->ref_clk_hz)
		return;

	ndiv = map_freq_to_ndiv(nltbl, rate);
	ndiv = clamp_ndiv(nltbl, ndiv);

	val = (uint64_t)ndiv;
	smp_call_function_single(cpu, write_ndiv_request, &val, 1);
}

/**
 * tegra194_set_speed - Request freq to be set for policy->cpu
 * @policy - cpufreq policy per cpu
 * @index - freq table index
 * Returns 0 on success, -ve on failure
 */
static int tegra194_set_speed(struct cpufreq_policy *policy, unsigned int index)
{
	struct cpufreq_frequency_table *ftbl;
	struct cpufreq_freqs freqs;
	uint32_t tgt_freq;
	enum cluster cl;
	int cpu, ret = 0;

	ftbl = get_freqtable(policy->cpu);
	tgt_freq = ftbl[index].frequency;
	freqs.old = policy->cur;

	if (policy->cur == tgt_freq)
		goto out;

	freqs.new = tgt_freq;

	cpufreq_freq_transition_begin(policy, &freqs);

	cl = topology_physical_package_id(policy->cpu);

	for_each_cpu(cpu, &tfreq_data.pcluster[cl].cpu_mask)
		tegra_update_cpu_speed(tgt_freq, cpu);

	if (tfreq_data.pcluster[cl].bwmgr)
		set_cpufreq_to_emcfreq(cl, tgt_freq);

	cpufreq_freq_transition_end(policy, &freqs, ret);
out:
	pr_debug("cpufreq: cpu: %d, ", policy->cpu);
	pr_debug("oldfreq(kHz): %d, ", freqs.old);
	pr_debug("req freq(kHz): %d ", tgt_freq);
	pr_debug("final freq(kHz): %d ", policy->cur);
	pr_debug("tgt_index %u\n", index);
	return ret;
}

static void __tegra_mce_cc3_ctrl(void *data)
{
	struct cc3_params *param = (struct cc3_params *)data;

	t19x_mce_cc3_ctrl(param->ndiv, param->enable);
}

static void enable_cc3(struct device_node *dn)
{
	struct mrq_cpu_ndiv_limits_response *nltbl;
	struct cc3_params *cc3;
	u32 enb, freq = 0, idx = 0;
	u16 ndiv;
	int cl;
	int ret = 0;

	LOOP_FOR_EACH_CLUSTER(cl) {
		if (!tfreq_data.pcluster[cl].configured)
			continue;
		nltbl = &tfreq_data.pcluster[cl].ndiv_limits_tbl;
		cc3 = &tfreq_data.pcluster[cl].cc3;

		if (!nltbl->ref_clk_hz)
			goto idx_inc;

		ret = of_property_read_u32_index(dn, "nvidia,enable-autocc3",
			idx + 1, &enb);
		if (!enb || ret)
			goto idx_inc;

		ret = of_property_read_u32_index(dn, "nvidia,autocc3-freq",
			idx + 1, &freq);
		if (ret)
			freq = 0;

		ndiv = map_freq_to_ndiv(nltbl, freq);
		ndiv = clamp_ndiv(nltbl, ndiv);
		cc3->enable = 1;
		cc3->ndiv = ndiv;

		ret = smp_call_function_any(&tfreq_data.pcluster[cl].cpu_mask,
				__tegra_mce_cc3_ctrl,
				cc3, 1);
		WARN_ON_ONCE(ret);
idx_inc:
		idx += INDEX_STEP;
	}
}

#ifdef CONFIG_DEBUG_FS
#define RW_MODE			(S_IWUSR | S_IRUGO)
#define RO_MODE			(S_IRUGO)

static int get_delay(void *data, u64 *val)
{
	*val = tfreq_data.freq_compute_delay;

	return 0;
}

static int set_delay(void *data, u64 val)
{
	uint32_t udelay = val;

	if (udelay)
		tfreq_data.freq_compute_delay = udelay;

	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(freq_compute_fops, get_delay, set_delay,
	"%llu\n");

static int freq_get(void *data, u64 *val)
{
	uint64_t cpu = (uint64_t)data;

	get_online_cpus();
	if (cpu_online(cpu))
		*val = tegra194_get_speed(cpu);
	put_online_cpus();

	return 0;
}

/* Set freq in Khz for a cpu  */
static int freq_set(void *data, u64 val)
{
	uint64_t cpu = (uint64_t)data;
	uint32_t freq = val;

	if (val)
		tegra_update_cpu_speed(freq, cpu);

	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(freq_fops, freq_get, freq_set, "%llu\n");

/* Set ndiv for a cpu */
static int set_ndiv(void *data, u64 val)
{
	uint64_t cpu = (uint64_t)data;
	uint16_t ndiv = val & 0xffff;

	if (!val)
		return -EINVAL;

	get_online_cpus();
	if (cpu_online(cpu))
		smp_call_function_single(cpu, write_ndiv_request, &ndiv, 1);

	put_online_cpus();
	return 0;
}

/* Get ndiv for a cpu */
static int get_ndiv(void *data, u64 *ndiv)
{
	uint64_t cpu = (uint64_t)data;

	get_online_cpus();
	if (cpu_online(cpu)) {
		smp_call_function_single(cpu, read_ndiv_request, ndiv, 1);
		*ndiv = *ndiv & 0xffff;
	}

	put_online_cpus();
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(ndiv_fops, get_ndiv, set_ndiv, "%04llx\n");

static void dump_ndiv_limits_tbl(struct seq_file *s,
				struct mrq_cpu_ndiv_limits_response *nlt)
{
	seq_printf(s, "reference clk(hz): %u\n", nlt->ref_clk_hz);
	seq_printf(s, "pdiv: %u\n", nlt->pdiv);
	seq_printf(s, "mdiv: %u\n", nlt->mdiv);
	seq_printf(s, "ndiv_max: %u\n", nlt->ndiv_max);
	seq_printf(s, "ndiv_min: %u\n", nlt->ndiv_min);
	seq_puts(s, "\n");
}

static int show_bpmp_to_cpu_ndiv_limits(struct seq_file *s, void *data)
{
	struct mrq_cpu_ndiv_limits_response *nlt;
	enum cluster cl;

	LOOP_FOR_EACH_CLUSTER(cl) {
		if (!tfreq_data.pcluster[cl].configured)
			continue;
		nlt = &tfreq_data.pcluster[cl].ndiv_limits_tbl;

		/*
		 * ndiv_limits for this cluster is not present.
		 * Could be single cluster or n cluster chip but for <cl>,
		 * current cluster, ndiv_limits is not sent by BPMP.
		 */
		if (!nlt->ref_clk_hz)
			continue;

		seq_printf(s, "cluster %d:\n", cl);
		dump_ndiv_limits_tbl(s, nlt);
	}

	return 0;
}

static int stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, show_bpmp_to_cpu_ndiv_limits,
			inode->i_private);
}

static const struct file_operations lut_fops = {
	.open = stats_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int get_pcluster_cc3(void *data, u64 *val)
{
	enum cluster cl = (enum cluster)data;

	mutex_lock(&tfreq_data.mlock);

	*val = tfreq_data.pcluster[cl].cc3.enable;

	mutex_unlock(&tfreq_data.mlock);

	return 0;
}

static int set_pcluster_cc3(void *data, u64 val)
{
	enum cluster cl = (enum cluster)data;
	int wait = 1;
	int ret = 0;

	mutex_lock(&tfreq_data.mlock);

	if (tfreq_data.pcluster[cl].cc3.enable ^ (bool) val) {
		tfreq_data.pcluster[cl].cc3.enable = (bool) val;
		ret = smp_call_function_any(&tfreq_data.pcluster[cl].cpu_mask,
				__tegra_mce_cc3_ctrl,
				&tfreq_data.pcluster[cl].cc3, wait);
	}

	mutex_unlock(&tfreq_data.mlock);
	return ret;
}
DEFINE_SIMPLE_ATTRIBUTE(pcl_cc3_ops, get_pcluster_cc3,
	set_pcluster_cc3, "%llu\n");

static int get_cc3_ndiv(void *data, u64 *val)
{
	enum cluster cl = (enum cluster)data;

	mutex_lock(&tfreq_data.mlock);

	*val = tfreq_data.pcluster[cl].cc3.ndiv;

	mutex_unlock(&tfreq_data.mlock);

	return 0;
}

static int set_cc3_ndiv(void *data, u64 val)
{
	enum cluster cl = (enum cluster)data;
	int wait = 1;
	int ret = 0;

	mutex_lock(&tfreq_data.mlock);

	if (tfreq_data.pcluster[cl].cc3.ndiv != (u32) val) {
		tfreq_data.pcluster[cl].cc3.ndiv = (u32) val;
		ret = smp_call_function_any(&tfreq_data.pcluster[cl].cpu_mask,
				__tegra_mce_cc3_ctrl,
				&tfreq_data.pcluster[cl].cc3, wait);
	}

	mutex_unlock(&tfreq_data.mlock);
	return ret;
}
DEFINE_SIMPLE_ATTRIBUTE(cc3_ndiv_ops, get_cc3_ndiv, set_cc3_ndiv,
	"%llu\n");

static struct dentry *tegra_cpufreq_debugfs_root;
static int __init cc3_debug_init(void)
{
	struct dentry *dir;
	long int cl;
	uint8_t buff[64];

	LOOP_FOR_EACH_CLUSTER(cl) {
		if (!tfreq_data.pcluster[cl].configured)
			continue;
		snprintf(buff, sizeof(buff), "CLUSTER%ld", cl);
		dir = debugfs_create_dir(buff, tegra_cpufreq_debugfs_root);
		if (!dir)
			goto err_out;

		snprintf(buff, sizeof(buff), "cc3");
		dir = debugfs_create_dir(buff, dir);
		if (!dir)
			goto err_out;

		if (!debugfs_create_file("enable", RW_MODE, dir, (void *)cl,
			&pcl_cc3_ops))
			goto err_out;

		if (!debugfs_create_file("ndiv", RW_MODE, dir, (void *)cl,
			&cc3_ndiv_ops))
			goto err_out;
	}
	return 0;

err_out:
	return -EINVAL;
}

static int __init tegra_cpufreq_debug_init(void)
{
	struct dentry *dir;
	uint8_t buff[15];
	uint64_t cpu;

	tegra_cpufreq_debugfs_root = debugfs_create_dir("tegra_cpufreq", NULL);
	if (!tegra_cpufreq_debugfs_root)
		return -ENOMEM;

	if (!debugfs_create_file("bpmp_cpu_ndiv_limits_table", RO_MODE,
				 tegra_cpufreq_debugfs_root,
					NULL,
					&lut_fops))
		goto err_out;

	if (!debugfs_create_file("freq_compute_delay", RW_MODE,
				 tegra_cpufreq_debugfs_root,
					NULL,
					&freq_compute_fops))
		goto err_out;

	if (cc3_debug_init())
		goto err_out;

	for_each_possible_cpu(cpu) {
		snprintf(buff, sizeof(buff), "cpu%llu", cpu);
		dir = debugfs_create_dir(buff, tegra_cpufreq_debugfs_root);
		if (!dir)
			goto err_out;
		if (!debugfs_create_file("freq", RW_MODE, dir, (void *)cpu,
			&freq_fops))
			goto err_out;
		if (!debugfs_create_file("ndiv", RW_MODE, dir,
			(void *)cpu, &ndiv_fops))
			goto err_out;
	}
	return 0;
err_out:
	debugfs_remove_recursive(tegra_cpufreq_debugfs_root);
	return -ENOMEM;
}

static void __exit tegra_cpufreq_debug_exit(void)
{
	debugfs_remove_recursive(tegra_cpufreq_debugfs_root);
}
#endif

static int tegra194_cpufreq_init(struct cpufreq_policy *policy)
{
	struct cpufreq_frequency_table *ftbl;
	enum cluster cl;
	uint32_t freq;
	int ret = 0;
	int idx;

	if (policy->cpu >= CONFIG_NR_CPUS)
		return -EINVAL;

	freq = tegra194_get_speed(policy->cpu); /* boot freq */

	ftbl = get_freqtable(policy->cpu);

	cpufreq_table_validate_and_show(policy, ftbl);

	/* clip boot frequency to table entry */
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 9, 0)
	ret = cpufreq_frequency_table_target(policy, ftbl, freq,
		CPUFREQ_RELATION_L, &idx);
#else
	idx = cpufreq_frequency_table_target(policy, freq,
					     CPUFREQ_RELATION_L);
#endif
	if (!ret && (freq != ftbl[idx].frequency)) {
		freq = ftbl[idx].frequency;
		tegra_update_cpu_speed(freq, policy->cpu);
	}

	policy->cur = tegra194_get_speed(policy->cpu);

	cl = topology_physical_package_id(policy->cpu);
	if (tfreq_data.pcluster[cl].bwmgr)
		set_cpufreq_to_emcfreq(cl, policy->cur);

	policy->cpuinfo.transition_latency =
	TEGRA_CPUFREQ_TRANSITION_LATENCY;

	cpumask_copy(policy->cpus, &tfreq_data.pcluster[cl].cpu_mask);

	return ret;
}

static int tegra194_cpufreq_exit(struct cpufreq_policy *policy)
{
	struct cpufreq_frequency_table *ftbl;
	enum cluster cl;

	ftbl = get_freqtable(policy->cpu);
	cpufreq_frequency_table_cpuinfo(policy, ftbl);
	cl = topology_physical_package_id(policy->cpu);
	if (tfreq_data.pcluster[cl].bwmgr)
		tegra_bwmgr_set_emc(tfreq_data.pcluster[cl].bwmgr, 0,
			TEGRA_BWMGR_SET_EMC_FLOOR);

	return 0;
}

static struct cpufreq_driver tegra_cpufreq_driver = {
	.name = "tegra_cpufreq",
	.flags = CPUFREQ_ASYNC_NOTIFICATION | CPUFREQ_STICKY |
				CPUFREQ_CONST_LOOPS,
	.verify = cpufreq_generic_frequency_table_verify,
	.target_index = tegra194_set_speed,
	.get = tegra194_get_speed,
	.init = tegra194_cpufreq_init,
	.exit = tegra194_cpufreq_exit,
	.attr = cpufreq_generic_attr,
};

static int cpu_freq_notify(struct notifier_block *b,
			unsigned long l, void *v)
{
	struct cpufreq_policy *policy;
	u32 qmin, qmax, cpu;

	qmin = (u32)pm_qos_read_min_bound(PM_QOS_CPU_FREQ_BOUNDS);
	qmax = (u32)pm_qos_read_max_bound(PM_QOS_CPU_FREQ_BOUNDS);

	for_each_online_cpu(cpu) {
		policy = cpufreq_cpu_get(cpu);
		if (policy) {
			policy->user_policy.min = qmin;
			policy->user_policy.max = qmax;
			cpufreq_update_policy(policy->cpu);
			cpufreq_cpu_put(policy);
		}
	}
	return NOTIFY_OK;
}

static struct notifier_block cpu_freq_nb = {
	.notifier_call = cpu_freq_notify,
};

static void __init pm_qos_register_notifier(void)
{
	pm_qos_add_min_notifier(PM_QOS_CPU_FREQ_BOUNDS,
		&cpu_freq_nb);
	pm_qos_add_max_notifier(PM_QOS_CPU_FREQ_BOUNDS,
		&cpu_freq_nb);
}

static void free_resources(void)
{
	enum cluster cl;

	LOOP_FOR_EACH_CLUSTER(cl) {
		if (!tfreq_data.pcluster[cl].configured)
			continue;

		/* free table */
		kfree(tfreq_data.pcluster[cl].clft);

		/* unregister from emc bw manager */
		tegra_bwmgr_unregister(tfreq_data.pcluster[cl].bwmgr);

	}
	flush_workqueue(read_counters_wq);
	destroy_workqueue(read_counters_wq);
}

static void __init free_allocated_res_init(void)
{
	free_resources();
}

static void __exit free_allocated_res_exit(void)
{
	free_resources();
}

static int __init init_freqtbls(struct device_node *dn)
{
	u16 freq_table_step_size = CPUFREQ_TBL_STEP_SIZE;
	u16 dt_freq_table_step_size = 0;
	struct cpufreq_frequency_table *ftbl;
	struct mrq_cpu_ndiv_limits_response *nltbl;
	u16 ndiv, max_freq_steps, delta_ndiv;
	enum cluster cl;
	int ret = 0, index;

	if (!of_property_read_u16(dn, "freq_table_step_size",
					&dt_freq_table_step_size)) {
		freq_table_step_size = dt_freq_table_step_size;
		if (!freq_table_step_size) {
			freq_table_step_size = CPUFREQ_TBL_STEP_SIZE;
			pr_info("cpufreq: Invalid cpu ");
			pr_info("freq_table_step_size:%d ",
				dt_freq_table_step_size);
			pr_info("setting to default value:%d\n",
				freq_table_step_size);
		}
	}

	pr_debug("cpufreq: CPU frequency table step size: %d\n",
			freq_table_step_size);

	LOOP_FOR_EACH_CLUSTER(cl) {
		if (!tfreq_data.pcluster[cl].configured)
			continue;
		nltbl = &tfreq_data.pcluster[cl].ndiv_limits_tbl;

		/*
		 * ndiv_limits for this cluster is not present.
		 * Could be single cluster or n cluster chip but for <cl>,
		 * current cluster, ndiv_limits is not sent by BPMP.
		 */
		if (!nltbl->ref_clk_hz) {
			pr_warn("%s: cpufreq: ", __func__);
			pr_warn("cluster %d has no ndiv_limits table\n", cl);
			continue;
		}

		delta_ndiv = nltbl->ndiv_max - nltbl->ndiv_min;
		if (unlikely(delta_ndiv == 0))
			max_freq_steps = 1;
		else {
			/* We store both ndiv_min and ndiv_max hence the +1 */
			max_freq_steps = delta_ndiv / freq_table_step_size + 1;
		}

		max_freq_steps += (delta_ndiv % freq_table_step_size) ? 1 : 0;

		/* Allocate memory 1 + max_freq_steps to write END_OF_TABLE */
		ftbl = kzalloc(sizeof(struct cpufreq_frequency_table) *
			(max_freq_steps + 1), GFP_KERNEL);
		if (!ftbl) {
			ret = -ENOMEM;
			while (cl--)
				kfree(tfreq_data.pcluster[cl].clft);
			goto err_out;
		}

		for (index = 0, ndiv = nltbl->ndiv_min;
				ndiv < nltbl->ndiv_max;
				index++, ndiv += freq_table_step_size)
			ftbl[index].frequency = (unsigned long)
				(nltbl->ref_clk_hz * ndiv)
				/ (nltbl->pdiv * nltbl->mdiv * 1000);

		ftbl[index++].frequency = (unsigned long)
			(nltbl->ndiv_max * nltbl->ref_clk_hz) /
			(nltbl->pdiv * nltbl->mdiv * 1000);

		ftbl[index].frequency = CPUFREQ_TABLE_END;

		tfreq_data.pcluster[cl].clft = ftbl;
	}

err_out:
	return ret;
}

static int __init get_ndiv_limits_tbl_from_bpmp(void)
{
	struct mrq_cpu_ndiv_limits_request md;
	struct mrq_cpu_ndiv_limits_response *nltbl;
	int cl;
	int ret = 0;
	bool ok = false;

	LOOP_FOR_EACH_CLUSTER(cl) {
		if (!tfreq_data.pcluster[cl].configured)
			continue;
		nltbl = &tfreq_data.pcluster[cl].ndiv_limits_tbl;
		md.cluster_id = cpu_to_le32(cl);

		ret = tegra_bpmp_send_receive(MRQ_CPU_NDIV_LIMITS, &md,
				sizeof(struct mrq_cpu_ndiv_limits_request),
				nltbl,
				sizeof(struct mrq_cpu_ndiv_limits_response));

		if (ret) {
			pr_warn("%s: cpufreq: ", __func__);
			pr_warn("cluster %d: ndiv_limits query failed!\n", cl);
			goto err_out;
		} else {
			ok = true;
		}
	}
err_out:
	return ok ? 0 : ret;
}

static void set_cpu_mask(void)
{
	int cpu_num;
	enum cluster cl;

	LOOP_FOR_EACH_CLUSTER(cl) {
		if (!tfreq_data.pcluster[cl].configured)
			continue;
		cpumask_clear(&tfreq_data.pcluster[cl].cpu_mask);
	}

	for_each_possible_cpu(cpu_num) {
		cl = topology_physical_package_id(cpu_num);
		if (!tfreq_data.pcluster[cl].configured)
			continue;
		cpumask_set_cpu(cpu_num,
			&tfreq_data.pcluster[cl].cpu_mask);
	}
}

static int __init register_with_emc_bwmgr(void)
{
	enum tegra_bwmgr_client_id bw_id_array[MAX_CLUSTERS] = {
		TEGRA_BWMGR_CLIENT_CPU_CLUSTER_0,
		TEGRA_BWMGR_CLIENT_CPU_CLUSTER_1,
		TEGRA_BWMGR_CLIENT_CPU_CLUSTER_2,
		TEGRA_BWMGR_CLIENT_CPU_CLUSTER_3
	};

	struct tegra_bwmgr_client *bwmgr;
	enum cluster cl;
	int ret = 0;

	LOOP_FOR_EACH_CLUSTER(cl) {
		if (!tfreq_data.pcluster[cl].configured)
			continue;
		bwmgr = tegra_bwmgr_register(bw_id_array[cl]);
		if (IS_ERR_OR_NULL(bwmgr)) {
			pr_warn("cpufreq: emc bw manager registration failed");
			pr_warn(" for cluster %d\n", cl);
			ret = -ENODEV;
			while (cl--)
				tegra_bwmgr_unregister(
				tfreq_data.pcluster[cl].bwmgr);
			goto err_out;
		}
		tfreq_data.pcluster[cl].bwmgr = bwmgr;
	}
err_out:
	return ret;
}

static void tegra_cpufreq_cpu_emc_map_init(struct device_node *dn)
{
	struct property *prop;
	int len;

	prop = of_find_property(dn, "cpu_emc_map", &len);
	if (prop) {
		len = rounddown(len, sizeof(struct cpu_emc_map));
		cpu_emc_map_ptr = kzalloc(len, GFP_KERNEL);
		if (cpu_emc_map_ptr) {
			of_property_read_u32_array(dn, "cpu_emc_map",
				(u32 *)cpu_emc_map_ptr, len / sizeof(uint32_t));
			cpu_emc_map_num = len / sizeof(struct cpu_emc_map);
		}
	}
}

static int __init tegra194_cpufreq_probe(struct platform_device *pdev)
{
	struct device_node *dn;
	uint32_t cpu;
	int ret = 0, cl = 0;

	dn = pdev->dev.of_node;
	tegra_cpufreq_cpu_emc_map_init(dn);

	mutex_init(&tfreq_data.mlock);
	tfreq_data.freq_compute_delay = US_DELAY;

	for_each_possible_cpu(cpu) {
		cl = topology_physical_package_id(cpu);
		if (!tfreq_data.pcluster[cl].configured)
			tfreq_data.pcluster[cl].configured = 1;
	}

	set_cpu_mask();

#ifdef CONFIG_DEBUG_FS
	ret = tegra_cpufreq_debug_init();
	if (ret) {
		pr_err("tegra19x-cpufreq: failed to create debugfs nodes\n");
		goto err_out;
	}
#endif

	ret = register_with_emc_bwmgr();
	if (ret) {
		pr_err("tegra19x-cpufreq: fail to register emc bw manager\n");
		goto err_out;
	}

	read_counters_wq = create_workqueue("read_counters_wq");
	if (!read_counters_wq) {
		pr_err("tegra19x-cpufreq: fail to create_workqueue\n");
		goto err_free_res;
	}

	tfreq_data.emc_max_rate = tegra_bwmgr_get_max_emc_rate();
	if (cpu_emc_map_ptr)
		cpu_emc_map_ptr[0].emcfreq = tfreq_data.emc_max_rate / 1000;

	ret = get_ndiv_limits_tbl_from_bpmp();
	if (ret)
		goto err_free_res;

	enable_cc3(dn);

	ret = init_freqtbls(dn);
	if (ret)
		goto err_free_res;

	ret = cpufreq_register_driver(&tegra_cpufreq_driver);
	if (ret)
		goto err_free_res;

	pm_qos_register_notifier();

	goto err_out;
err_free_res:
	free_allocated_res_init();
err_out:
	pr_info("%s: platform driver Initialization: %s\n",
		__func__, (ret ? "fail" : "pass"));
	return ret;
}

static int __exit tegra194_cpufreq_remove(struct platform_device *pdev)
{
#ifdef CONFIG_DEBUG_FS
	tegra_cpufreq_debug_exit();
#endif
	cpufreq_unregister_driver(&tegra_cpufreq_driver);
	free_allocated_res_exit();
	return 0;
}

static const struct of_device_id tegra194_cpufreq_of_match[] = {
	{. compatible = "nvidia,tegra194-cpufreq", },
	{ }
};
MODULE_DEVICE_TABLE(of, tegra194_cpufreq_of_match);
static struct platform_driver tegra194_cpufreq_platform_driver __refdata = {
	.driver = {
		.name = "tegra194-cpufreq",
		.of_match_table = tegra194_cpufreq_of_match,
	},
	.probe = tegra194_cpufreq_probe,
	.remove = tegra194_cpufreq_remove,
};
module_platform_driver(tegra194_cpufreq_platform_driver);

MODULE_AUTHOR("Hoang Pham <hopham@nvidia.com>");
MODULE_AUTHOR("Bo Yan <byan@nvidia.com>");
MODULE_DESCRIPTION("NVIDIA Tegra194 cpufreq driver");
MODULE_LICENSE("GPL v2");