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

struct ptcache_head {
	spinlock_t lock;
	struct list_head pages;
	unsigned long count;
	atomic64_t hits;
	atomic64_t misses;
	atomic64_t returns;
} ____cacheline_aligned_in_smp;

static struct ptcache_head ptcache[MAX_NUMNODES];

static int __init ptcache_init(void)
{
	int node;

	for (node = 0; node < MAX_NUMNODES; node++) {
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

	if (node < 0 || node >= MAX_NUMNODES)
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

struct page *ptcache_alloc(struct mm_struct *mm, gfp_t gfp)
{
	struct page *page;
	int node;

	if (!mm || !READ_ONCE(mm->cache_only_mode))
		return NULL;

	node = numa_node_id();
	if (node < 0 || node >= MAX_NUMNODES)
		return NULL;

	page = ptcache_pop(node);
	if (page) {
		atomic64_inc(&ptcache[node].hits);
		SetPagePtCache(page);
		clear_highpage(page);
		return page;
	}

	atomic64_inc(&ptcache[node].misses);

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

	for (node = 0; node < MAX_NUMNODES; node++)
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

static int __init ptcache_proc_init(void)
{
	if (!proc_create("ptcache", 0644, NULL, &ptcache_proc_ops))
		return -ENOMEM;

	return 0;
}
late_initcall(ptcache_proc_init);
