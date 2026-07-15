#ifndef _LINUX_PTCACHE_H
#define _LINUX_PTCACHE_H

#include <linux/types.h>
#include <linux/gfp_types.h>

struct mm_struct;
struct page;
struct ptdesc;

extern int sysctl_ptcache_invlpgb;

struct page *ptcache_alloc(struct mm_struct *mm, gfp_t gfp);
bool ptcache_return_table(struct ptdesc *ptdesc);

#endif /* _LINUX_PTCACHE_H */
