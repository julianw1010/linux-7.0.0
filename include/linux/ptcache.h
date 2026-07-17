#ifndef _LINUX_PTCACHE_H
#define _LINUX_PTCACHE_H

#include <linux/types.h>
#include <linux/gfp_types.h>

#ifdef CONFIG_PTCACHE_NUMA_NODE_COUNT
#define PTCACHE_NODE_COUNT CONFIG_PTCACHE_NUMA_NODE_COUNT
#else
#define PTCACHE_NODE_COUNT 8
#endif

struct mm_struct;
struct page;
struct ptdesc;
struct ptcache_stats;

extern int sysctl_ptcache_invlpgb;

struct page *ptcache_alloc(struct mm_struct *mm, gfp_t gfp);
bool ptcache_return_table(struct ptdesc *ptdesc);

enum ptcache_pt_level {
	PTCACHE_PT_PGD = 0,
	PTCACHE_PT_P4D,
	PTCACHE_PT_PUD,
	PTCACHE_PT_PMD,
	PTCACHE_PT_PTE,
	PTCACHE_PT_NR_LEVELS,
};

struct ptcache_stats *ptcache_stats_attach(struct mm_struct *mm);
void ptcache_stats_detach(struct mm_struct *mm);
void ptcache_stats_mark_enabled(struct mm_struct *mm);
void ptcache_stats_fault(struct mm_struct *mm, unsigned int flags);
void ptcache_stats_pt_write(void *tablep, int level);
void ptcache_stats_pt_inc(struct mm_struct *mm, int node, int level);
void ptcache_stats_pt_dec(struct mm_struct *mm, int node, int level);
void ptcache_stats_tlb_ipi(struct mm_struct *mm, long count);
void ptcache_stats_tlb_broadcast(struct mm_struct *mm, long count);
void ptcache_stats_numa(struct mm_struct *mm, bool huge, int from, int to);
void ptcache_stats_thp_split(struct mm_struct *mm);
void ptcache_stats_thp_collapse(struct mm_struct *mm);
void ptcache_stats_deposit(struct mm_struct *mm);
void ptcache_stats_withdraw(struct mm_struct *mm);

#endif /* _LINUX_PTCACHE_H */
