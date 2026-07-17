// SPDX-License-Identifier: GPL-2.0
#include <linux/ptcache.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/gfp.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/highmem.h>
#include <linux/page-flags.h>
#include <linux/nodemask.h>
#include <linux/topology.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/sched.h>

struct ptcache_head {
	spinlock_t lock;
	struct list_head pages;
	unsigned long count;
	atomic64_t hits;
	atomic64_t misses;
	atomic64_t returns;
} ____cacheline_aligned_in_smp;

static struct ptcache_head ptcache[PTCACHE_NODE_COUNT];

struct ptcache_stats {
	struct list_head list;
	unsigned long id;
	int pid;
	char comm[TASK_COMM_LEN];
	void *mm;
	int ever_enabled;

	atomic_long_t hits;
	atomic_long_t misses;

	atomic_long_t pt_cur[PTCACHE_NODE_COUNT][PTCACHE_PT_NR_LEVELS];
	atomic_long_t pt_max[PTCACHE_NODE_COUNT][PTCACHE_PT_NR_LEVELS];

	atomic_long_t tlb_shootdowns;
	atomic_long_t tlb_broadcasts;

	atomic_long_t numa_migrate_4k[PTCACHE_NODE_COUNT][PTCACHE_NODE_COUNT];
	atomic_long_t numa_migrate_2m[PTCACHE_NODE_COUNT][PTCACHE_NODE_COUNT];
};

static LIST_HEAD(ptcache_live_list);
static LIST_HEAD(ptcache_hist_list);
static DEFINE_SPINLOCK(ptcache_stats_lock);
static unsigned long ptcache_stats_next_id;

struct ptcache_stats *ptcache_stats_attach(struct mm_struct *mm)
{
	struct ptcache_stats *s;

	if (mm->ptcache_stats)
		return mm->ptcache_stats;

	s = kzalloc(sizeof(*s), GFP_KERNEL);
	if (!s)
		return NULL;

	INIT_LIST_HEAD(&s->list);
	s->pid = current->pid;
	get_task_comm(s->comm, current);
	s->mm = mm;

	spin_lock(&ptcache_stats_lock);
	s->id = ++ptcache_stats_next_id;
	list_add_tail(&s->list, &ptcache_live_list);
	spin_unlock(&ptcache_stats_lock);

	mm->ptcache_stats = s;
	return s;
}

void ptcache_stats_mark_enabled(struct mm_struct *mm)
{
	struct ptcache_stats *s = mm->ptcache_stats;

	if (!s)
		return;

	WRITE_ONCE(s->ever_enabled, 1);

	if (mm == current->mm) {
		s->pid = current->pid;
		get_task_comm(s->comm, current);
	}
}

void ptcache_stats_detach(struct mm_struct *mm)
{
	struct ptcache_stats *s = mm->ptcache_stats;

	if (!s)
		return;

	mm->ptcache_stats = NULL;

	if (!s->ever_enabled) {
		spin_lock(&ptcache_stats_lock);
		list_del(&s->list);
		spin_unlock(&ptcache_stats_lock);
		kfree(s);
		return;
	}

	spin_lock(&ptcache_stats_lock);
	list_move_tail(&s->list, &ptcache_hist_list);
	spin_unlock(&ptcache_stats_lock);
}

static void ptcache_bump_max(atomic_long_t *maxp, long cur)
{
	long mx = atomic_long_read(maxp);

	while (cur > mx) {
		long prev = atomic_long_cmpxchg(maxp, mx, cur);

		if (prev == mx)
			break;
		mx = prev;
	}
}

void ptcache_stats_pt_inc(struct mm_struct *mm, int node, int level)
{
	struct ptcache_stats *s;
	long cur;

	if (!mm || node < 0 || node >= PTCACHE_NODE_COUNT)
		return;
	if (level < 0 || level >= PTCACHE_PT_NR_LEVELS)
		return;
	s = mm->ptcache_stats;
	if (!s)
		return;
	cur = atomic_long_inc_return(&s->pt_cur[node][level]);
	ptcache_bump_max(&s->pt_max[node][level], cur);
}

void ptcache_stats_pt_dec(struct mm_struct *mm, int node, int level)
{
	struct ptcache_stats *s;

	if (!mm || node < 0 || node >= PTCACHE_NODE_COUNT)
		return;
	if (level < 0 || level >= PTCACHE_PT_NR_LEVELS)
		return;
	s = mm->ptcache_stats;
	if (!s)
		return;
	atomic_long_dec(&s->pt_cur[node][level]);
}

void ptcache_stats_tlb_ipi(struct mm_struct *mm, long count)
{
	struct ptcache_stats *s;

	if (!mm || count <= 0)
		return;
	s = mm->ptcache_stats;
	if (s)
		atomic_long_add(count, &s->tlb_shootdowns);
}

void ptcache_stats_tlb_broadcast(struct mm_struct *mm, long count)
{
	struct ptcache_stats *s;

	if (!mm || count <= 0)
		return;
	s = mm->ptcache_stats;
	if (s)
		atomic_long_add(count, &s->tlb_broadcasts);
}

void ptcache_stats_numa(struct mm_struct *mm, bool huge, int from, int to)
{
	struct ptcache_stats *s;

	if (!mm)
		return;
	s = mm->ptcache_stats;
	if (!s)
		return;
	if (from < 0 || from >= PTCACHE_NODE_COUNT ||
	    to < 0 || to >= PTCACHE_NODE_COUNT)
		return;
	if (huge)
		atomic_long_inc(&s->numa_migrate_2m[from][to]);
	else
		atomic_long_inc(&s->numa_migrate_4k[from][to]);
}

static int __init ptcache_init(void)
{
	int node;

	for (node = 0; node < PTCACHE_NODE_COUNT; node++) {
		spin_lock_init(&ptcache[node].lock);
		INIT_LIST_HEAD(&ptcache[node].pages);
		ptcache[node].count = 0;
		atomic64_set(&ptcache[node].hits, 0);
		atomic64_set(&ptcache[node].misses, 0);
		atomic64_set(&ptcache[node].returns, 0);
	}

	return 0;
}
early_initcall(ptcache_init);

static bool ptcache_push(struct page *page, int node)
{
	struct ptcache_head *cache;
	unsigned long flags;

	if (node < 0 || node >= PTCACHE_NODE_COUNT)
		return false;

	cache = &ptcache[node];

	ClearPagePtCache(page);

	spin_lock_irqsave(&cache->lock, flags);
	list_add(&page->lru, &cache->pages);
	cache->count++;
	spin_unlock_irqrestore(&cache->lock, flags);

	return true;
}

static struct page *ptcache_pop(int node)
{
	struct ptcache_head *cache = &ptcache[node];
	struct page *page = NULL;
	unsigned long flags;

	spin_lock_irqsave(&cache->lock, flags);
	if (!list_empty(&cache->pages)) {
		page = list_first_entry(&cache->pages, struct page, lru);
		list_del(&page->lru);
		cache->count--;
	}
	spin_unlock_irqrestore(&cache->lock, flags);

	return page;
}

int sysctl_ptcache_invlpgb __read_mostly = 1;

struct page *ptcache_alloc(struct mm_struct *mm, gfp_t gfp)
{
	struct page *page;
	int node;

	if (!mm || !READ_ONCE(mm->cache_only_mode))
		return NULL;

	node = numa_node_id();
	if (node < 0 || node >= PTCACHE_NODE_COUNT)
		return NULL;

	page = ptcache_pop(node);
	if (page) {
		atomic64_inc(&ptcache[node].hits);
		if (mm->ptcache_stats)
			atomic_long_inc(&mm->ptcache_stats->hits);
		SetPagePtCache(page);
		clear_highpage(page);
		return page;
	}

	atomic64_inc(&ptcache[node].misses);
	if (mm->ptcache_stats)
		atomic_long_inc(&mm->ptcache_stats->misses);

	return alloc_pages_node(node, gfp | __GFP_COMP, 0);
}

bool ptcache_return_table(struct ptdesc *ptdesc)
{
	struct page *page = ptdesc_page(ptdesc);
	int node;

	if (!PagePtCache(page))
		return false;

	node = page_to_nid(page);
	pagetable_dtor(ptdesc);

	if (ptcache_push(page, node)) {
		atomic64_inc(&ptcache[node].returns);
		return true;
	}

	__free_page(page);
	return true;
}

static int ptcache_drain_node(int node)
{
	struct ptcache_head *cache = &ptcache[node];
	struct page *page, *tmp;
	unsigned long flags;
	LIST_HEAD(freelist);
	int freed = 0;

	spin_lock_irqsave(&cache->lock, flags);
	list_splice_init(&cache->pages, &freelist);
	cache->count = 0;
	spin_unlock_irqrestore(&cache->lock, flags);

	list_for_each_entry_safe(page, tmp, &freelist, lru) {
		list_del(&page->lru);
		__free_page(page);
		freed++;
	}

	return freed;
}

static int ptcache_drain_all(void)
{
	int node, total = 0;

	for (node = 0; node < PTCACHE_NODE_COUNT; node++)
		total += ptcache_drain_node(node);

	return total;
}

static int ptcache_populate_node(int node, long want)
{
	int added = 0;

	while (added < want) {
		struct page *page;

		page = alloc_pages_node(node,
					GFP_KERNEL | __GFP_ZERO | __GFP_COMP |
					__GFP_THISNODE, 0);
		if (!page)
			break;

		if (page_to_nid(page) != node) {
			__free_page(page);
			break;
		}

		if (!ptcache_push(page, node)) {
			__free_page(page);
			break;
		}

		added++;
	}

	return added;
}

static void ptcache_size_cell(struct seq_file *m, long long pages)
{
	char buf[24];
	long long tenths = pages * 5 / 128;

	scnprintf(buf, sizeof(buf), "%lld.%lld", tenths / 10, tenths % 10);
	seq_printf(m, " %10s", buf);
}

static int ptcache_show(struct seq_file *m, void *v)
{
	long long tot_pages = 0, tot_hits = 0, tot_misses = 0, tot_returns = 0;
	char buf[12];
	int node;

	for_each_online_node(node) {
		tot_pages += ptcache[node].count;
		tot_hits += atomic64_read(&ptcache[node].hits);
		tot_misses += atomic64_read(&ptcache[node].misses);
		tot_returns += atomic64_read(&ptcache[node].returns);
	}

	seq_puts(m, " Per-node page-table page cache\n");
	seq_puts(m, " write N > 0: add N pages to the cache of every online node\n");
	seq_puts(m, " write -1:    drain all nodes\n");
	seq_puts(m, " per-process opt-in: prctl(PR_SET_PGTABLE_CACHE_ONLY, 1, 0)\n");
	seq_puts(m, " /proc/ptcache/invlpgb: 0 = deny global asid to all mms\n");
	seq_puts(m, " rows = cache metric,  cols = NUMA node\n");
	seq_puts(m, " ----------------------------------------------------------------------\n");

	seq_printf(m, " %-10s", "");
	for_each_online_node(node) {
		scnprintf(buf, sizeof(buf), "n%d", node);
		seq_printf(m, " %10s", buf);
	}
	seq_printf(m, " %10s\n", "TOTAL");

	seq_printf(m, " %-10s", "pages");
	for_each_online_node(node)
		seq_printf(m, " %10lu", ptcache[node].count);
	seq_printf(m, " %10lld\n", tot_pages);

	seq_printf(m, " %-10s", "size (MiB)");
	for_each_online_node(node)
		ptcache_size_cell(m, ptcache[node].count);
	ptcache_size_cell(m, tot_pages);
	seq_putc(m, '\n');

	seq_printf(m, " %-10s", "hits");
	for_each_online_node(node)
		seq_printf(m, " %10lld", atomic64_read(&ptcache[node].hits));
	seq_printf(m, " %10lld\n", tot_hits);

	seq_printf(m, " %-10s", "misses");
	for_each_online_node(node)
		seq_printf(m, " %10lld", atomic64_read(&ptcache[node].misses));
	seq_printf(m, " %10lld\n", tot_misses);

	seq_printf(m, " %-10s", "returns");
	for_each_online_node(node)
		seq_printf(m, " %10lld", atomic64_read(&ptcache[node].returns));
	seq_printf(m, " %10lld\n", tot_returns);

	return 0;
}

static int ptcache_open(struct inode *inode, struct file *file)
{
	return single_open(file, ptcache_show, NULL);
}

static ssize_t ptcache_write(struct file *file, const char __user *ubuf,
			     size_t count, loff_t *ppos)
{
	char buf[32];
	size_t len;
	long val;
	int node, total, drained;

	len = min(count, sizeof(buf) - 1);
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';

	if (kstrtol(buf, 10, &val))
		return -EINVAL;

	if (val == -1) {
		drained = ptcache_drain_all();
		pr_info("ptcache: drained %d pages\n", drained);
		return count;
	}

	if (val <= 0)
		return -EINVAL;

	total = 0;
	for_each_online_node(node)
		total += ptcache_populate_node(node, val);

	pr_info("ptcache: populated %d pages across %d nodes\n",
		total, num_online_nodes());

	return count;
}

static const struct proc_ops ptcache_proc_ops = {
	.proc_open	= ptcache_open,
	.proc_read	= seq_read,
	.proc_write	= ptcache_write,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int ptcache_invlpgb_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", READ_ONCE(sysctl_ptcache_invlpgb));

	return 0;
}

static int ptcache_invlpgb_open(struct inode *inode, struct file *file)
{
	return single_open(file, ptcache_invlpgb_show, NULL);
}

static ssize_t ptcache_invlpgb_write(struct file *file, const char __user *ubuf,
				     size_t count, loff_t *ppos)
{
	char buf[32];
	size_t len;
	long val;

	len = min(count, sizeof(buf) - 1);
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';

	if (kstrtol(buf, 10, &val))
		return -EINVAL;

	if (val < 0 || val > 1)
		return -EINVAL;

	WRITE_ONCE(sysctl_ptcache_invlpgb, val);

	return count;
}

static const struct proc_ops ptcache_invlpgb_proc_ops = {
	.proc_open	= ptcache_invlpgb_open,
	.proc_read	= seq_read,
	.proc_write	= ptcache_invlpgb_write,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

#define PTCACHE_RULE \
	"========================================================================"

static void ptcache_print_kv(struct seq_file *m, const char *label, long val)
{
	seq_printf(m, "    %-40s %12ld\n", label, val);
}

static void ptcache_print_ratio(struct seq_file *m, const char *label,
				long num, long den)
{
	if (den > 0) {
		long x10 = (num * 10 + den / 2) / den;

		seq_printf(m, "    %-40s %10ld.%ld%%\n",
			   label, x10 / 10, x10 % 10);
	} else {
		seq_printf(m, "    %-40s %12s\n", label, "n/a");
	}
}

static void ptcache_print_section(struct seq_file *m, const char *name)
{
	seq_printf(m, "\n  %s\n", name);
	seq_puts(m, "  ------------------------------------------------------------------\n");
}

static void ptcache_print_node_header(struct seq_file *m)
{
	char buf[12];
	int node;

	seq_puts(m, "        ");
	for_each_online_node(node) {
		scnprintf(buf, sizeof(buf), "n%d", node);
		seq_printf(m, " %7s", buf);
	}
	seq_putc(m, '\n');
}

static void ptcache_print_node_matrix(struct seq_file *m,
				      atomic_long_t mat[][PTCACHE_NODE_COUNT])
{
	char buf[12];
	int from, to;

	ptcache_print_node_header(m);
	for_each_online_node(from) {
		scnprintf(buf, sizeof(buf), "n%d", from);
		seq_printf(m, "    %-4s", buf);
		for_each_online_node(to)
			seq_printf(m, " %7ld",
				   atomic_long_read(&mat[from][to]));
		seq_putc(m, '\n');
	}
}

static void ptcache_stats_print(struct seq_file *m, struct ptcache_stats *s,
				bool history)
{
	long h = atomic_long_read(&s->hits);
	long mi = atomic_long_read(&s->misses);
	long total = h + mi;
	long tlb_sent = atomic_long_read(&s->tlb_shootdowns);
	long tlb_bcast = atomic_long_read(&s->tlb_broadcasts);
	int node;

	seq_printf(m, "%s\n", PTCACHE_RULE);
	seq_printf(m, "  MM record #%lu\n", s->id);
	seq_printf(m, "%s\n", PTCACHE_RULE);
	seq_printf(m, "    %-40s %d\n", "pid", s->pid);
	seq_printf(m, "    %-40s %s\n", "comm", s->comm);
	seq_printf(m, "    %-40s %px\n", "mm", s->mm);

	ptcache_print_section(m, "Cache interaction");
	ptcache_print_kv(m, "Cache hits", h);
	ptcache_print_kv(m, "Cache misses", mi);
	ptcache_print_kv(m, "Cache allocs (hits + misses)", total);
	ptcache_print_ratio(m, "Hit rate", h * 100, total);

	ptcache_print_section(m, "TLB shootdowns (remote-CPU IPIs)");
	ptcache_print_kv(m, "Total shootdowns", tlb_sent);

	ptcache_print_section(m, "TLB broadcasts (INVLPGB)");
	ptcache_print_kv(m, "Total INVLPGB instructions", tlb_bcast);

	ptcache_print_section(m,
		"autoNUMA migrations: 4KB base pages  [rows = source node, cols = dest node]");
	ptcache_print_node_matrix(m, s->numa_migrate_4k);

	ptcache_print_section(m,
		"autoNUMA migrations: 2MB THP pages  [rows = source node, cols = dest node]");
	ptcache_print_node_matrix(m, s->numa_migrate_2m);

	{
		static const char * const lvl_name[PTCACHE_PT_NR_LEVELS] = {
			"PGD", "P4D", "PUD", "PMD", "PTE",
		};
		int lvl;

		ptcache_print_section(m, history ?
			"Page tables per node: max watermark  [rows = level, cols = node]" :
			"Page tables per node: current  [rows = level, cols = node]");
		ptcache_print_node_header(m);
		for (lvl = 0; lvl < PTCACHE_PT_NR_LEVELS; lvl++) {
			seq_printf(m, "    %-4s", lvl_name[lvl]);
			for_each_online_node(node)
				seq_printf(m, " %7ld",
					   atomic_long_read(history ?
							    &s->pt_max[node][lvl] :
							    &s->pt_cur[node][lvl]));
			seq_putc(m, '\n');
		}
	}

	seq_putc(m, '\n');
}

static void *ptcache_live_start(struct seq_file *m, loff_t *pos)
{
	spin_lock(&ptcache_stats_lock);
	m->private = &ptcache_live_list;
	return seq_list_start(&ptcache_live_list, *pos);
}

static void *ptcache_hist_start(struct seq_file *m, loff_t *pos)
{
	spin_lock(&ptcache_stats_lock);
	m->private = &ptcache_hist_list;
	return seq_list_start(&ptcache_hist_list, *pos);
}

static void *ptcache_seq_next(struct seq_file *m, void *v, loff_t *pos)
{
	return seq_list_next(v, (struct list_head *)m->private, pos);
}

static void ptcache_seq_stop(struct seq_file *m, void *v)
{
	spin_unlock(&ptcache_stats_lock);
}

static int ptcache_seq_show(struct seq_file *m, void *v)
{
	struct ptcache_stats *s = list_entry(v, struct ptcache_stats, list);

	if (!READ_ONCE(s->ever_enabled))
		return SEQ_SKIP;

	ptcache_stats_print(m, s, m->private == &ptcache_hist_list);
	return 0;
}

static const struct seq_operations ptcache_live_seq_ops = {
	.start	= ptcache_live_start,
	.next	= ptcache_seq_next,
	.stop	= ptcache_seq_stop,
	.show	= ptcache_seq_show,
};

static const struct seq_operations ptcache_hist_seq_ops = {
	.start	= ptcache_hist_start,
	.next	= ptcache_seq_next,
	.stop	= ptcache_seq_stop,
	.show	= ptcache_seq_show,
};

static int ptcache_status_open(struct inode *inode, struct file *file)
{
	return seq_open(file, &ptcache_live_seq_ops);
}

static int ptcache_history_open(struct inode *inode, struct file *file)
{
	return seq_open(file, &ptcache_hist_seq_ops);
}

static const struct proc_ops ptcache_status_proc_ops = {
	.proc_open	= ptcache_status_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= seq_release,
};

static int ptcache_stats_clear_history(void)
{
	struct ptcache_stats *s, *tmp;
	int freed = 0;

	spin_lock(&ptcache_stats_lock);
	list_for_each_entry_safe(s, tmp, &ptcache_hist_list, list) {
		list_del(&s->list);
		kfree(s);
		freed++;
	}
	spin_unlock(&ptcache_stats_lock);

	return freed;
}

static ssize_t ptcache_history_write(struct file *file, const char __user *ubuf,
				     size_t count, loff_t *ppos)
{
	char buf[32];
	size_t len;
	long val;
	int freed;

	len = min(count, sizeof(buf) - 1);
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';

	if (kstrtol(buf, 10, &val))
		return -EINVAL;

	if (val != -1)
		return -EINVAL;

	freed = ptcache_stats_clear_history();
	pr_info("ptcache: cleared %d history records\n", freed);
	return count;
}

static const struct proc_ops ptcache_history_proc_ops = {
	.proc_open	= ptcache_history_open,
	.proc_read	= seq_read,
	.proc_write	= ptcache_history_write,
	.proc_lseek	= seq_lseek,
	.proc_release	= seq_release,
};

static int __init ptcache_proc_init(void)
{
	struct proc_dir_entry *dir;

	dir = proc_mkdir("ptcache", NULL);
	if (!dir)
		return -ENOMEM;

	if (!proc_create("cache", 0644, dir, &ptcache_proc_ops))
		goto fail;

	if (!proc_create("invlpgb", 0644, dir, &ptcache_invlpgb_proc_ops))
		goto fail;

	if (!proc_create("status", 0444, dir, &ptcache_status_proc_ops))
		goto fail;

	if (!proc_create("history", 0644, dir, &ptcache_history_proc_ops))
		goto fail;

	return 0;

fail:
	remove_proc_subtree("ptcache", NULL);

	return -ENOMEM;
}
late_initcall(ptcache_proc_init);
