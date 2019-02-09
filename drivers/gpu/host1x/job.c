/*
 * Tegra host1x Job
 *
 * Copyright (c) 2010-2015, NVIDIA Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/dma-fence.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/host1x.h>
#include <linux/kref.h>
#include <linux/module.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <trace/events/host1x.h>

#include "channel.h"
#include "debug.h"
#include "dev.h"
#include "firewall.h"
#include "job.h"
#include "syncpt.h"

struct host1x_job *host1x_job_alloc(struct host1x_channel *ch,
				    u32 num_cmdbufs, u32 num_relocs)
{
	struct host1x_job *job = NULL;
	unsigned int num_unpins = num_relocs;
	u64 total;
	void *mem;

	if (!IS_ENABLED(CONFIG_TEGRA_HOST1X_FIREWALL))
		num_unpins += num_cmdbufs;

	/* Check that we're not going to overflow */
	total = sizeof(struct host1x_job) +
		(u64)num_relocs * sizeof(struct host1x_reloc) +
		(u64)num_unpins * sizeof(struct host1x_job_unpin_data) +
		(u64)num_cmdbufs * sizeof(struct host1x_job_gather) +
		(u64)num_unpins * sizeof(dma_addr_t) +
		(u64)num_unpins * sizeof(u32 *);
	if (total > ULONG_MAX)
		return NULL;

	mem = job = kzalloc(total, GFP_KERNEL);
	if (!job)
		return NULL;

	kref_init(&job->ref);
	job->channel = ch;

	/* Redistribute memory to the structs  */
	mem += sizeof(struct host1x_job);
	job->relocs = num_relocs ? mem : NULL;
	mem += num_relocs * sizeof(struct host1x_reloc);
	job->unpins = num_unpins ? mem : NULL;
	mem += num_unpins * sizeof(struct host1x_job_unpin_data);
	job->gathers = num_cmdbufs ? mem : NULL;
	mem += num_cmdbufs * sizeof(struct host1x_job_gather);
	job->addr_phys = num_unpins ? mem : NULL;

	job->reloc_addr_phys = job->addr_phys;
	job->gather_addr_phys = &job->addr_phys[num_relocs];

	return job;
}
EXPORT_SYMBOL(host1x_job_alloc);

struct host1x_job *host1x_job_get(struct host1x_job *job)
{
	kref_get(&job->ref);
	return job;
}
EXPORT_SYMBOL(host1x_job_get);

static void job_free(struct kref *ref)
{
	struct host1x_job *job = container_of(ref, struct host1x_job, ref);
	unsigned int i;

	for (i = 0; i < job->num_fences; i++)
		dma_fence_put(job->fences[i]);

	kfree(job->fences);
	kfree(job);
}

void host1x_job_put(struct host1x_job *job)
{
	kref_put(&job->ref, job_free);
}
EXPORT_SYMBOL(host1x_job_put);

void host1x_job_add_gather(struct host1x_job *job, struct host1x_bo *bo,
			   unsigned int words, unsigned int offset)
{
	struct host1x_job_gather *gather = &job->gathers[job->num_gathers];

	gather->words = words;
	gather->bo = bo;
	gather->offset = offset;

	job->num_gathers++;
}
EXPORT_SYMBOL(host1x_job_add_gather);

int host1x_job_add_fence(struct host1x_job *job, struct dma_fence *fence)
{
	struct dma_fence **fences;

	fences = krealloc(job->fences, (job->num_fences + 1) * sizeof(*fence),
			  GFP_KERNEL);
	if (!fences)
		return -ENOMEM;

	fences[job->num_fences] = dma_fence_get(fence);

	job->fences = fences;
	job->num_fences++;

	return 0;
}
EXPORT_SYMBOL(host1x_job_add_fence);

static unsigned int pin_job(struct host1x *host, struct host1x_job *job)
{
	struct host1x_bo *bo = NULL;
	unsigned int i;
	int err;

	job->num_unpins = 0;

	for (i = 0; i < job->num_relocs; i++) {
		struct host1x_reloc *reloc = &job->relocs[i];
		struct sg_table *sgt;
		dma_addr_t phys_addr;

		bo = host1x_bo_get(reloc->target.bo);
		if (!bo) {
			err = -EINVAL;
			goto unpin;
		}

		err = host1x_bo_pin(bo, &phys_addr, &sgt);
		if (err)
			goto unpin;

		job->addr_phys[job->num_unpins] = phys_addr;
		job->unpins[job->num_unpins].bo = bo;
		job->unpins[job->num_unpins].sgt = sgt;
		job->num_unpins++;
	}

	/*
	 * We will copy gathers BO content later, so there is no need to
	 * hold and pin them.
	 */
	if (IS_ENABLED(CONFIG_TEGRA_HOST1X_FIREWALL))
		return 0;

	for (i = 0; i < job->num_gathers; i++) {
		struct host1x_job_gather *g = &job->gathers[i];
		size_t gather_size = 0;
		struct scatterlist *sg;
		struct sg_table *sgt;
		dma_addr_t phys_addr;
		unsigned long shift;
		struct iova *alloc;
		unsigned int j;

		bo = host1x_bo_get(g->bo);
		if (!bo) {
			err = -EINVAL;
			goto unpin;
		}

		err = host1x_bo_pin(bo, &phys_addr, &sgt);
		if (err)
			goto unpin;

		if (host->domain) {
			for_each_sg(sgt->sgl, sg, sgt->nents, j)
				gather_size += sg->length;
			gather_size = iova_align(&host->iova, gather_size);

			shift = iova_shift(&host->iova);
			alloc = alloc_iova(&host->iova, gather_size >> shift,
					   host->iova_end >> shift, true);
			if (!alloc) {
				err = -ENOMEM;
				goto unpin;
			}

			err = iommu_map_sg(host->domain,
					iova_dma_addr(&host->iova, alloc),
					sgt->sgl, sgt->nents, IOMMU_READ);
			if (err == 0) {
				__free_iova(&host->iova, alloc);
				err = -EINVAL;
				goto unpin;
			}

			job->addr_phys[job->num_unpins] =
				iova_dma_addr(&host->iova, alloc);
			job->unpins[job->num_unpins].size = gather_size;
		} else {
			job->addr_phys[job->num_unpins] = phys_addr;
		}

		job->gather_addr_phys[i] = job->addr_phys[job->num_unpins];

		job->unpins[job->num_unpins].bo = bo;
		job->unpins[job->num_unpins].sgt = sgt;
		job->num_unpins++;
	}

	return 0;

unpin:
	if (bo)
		host1x_bo_put(bo);
	host1x_job_unpin(job);
	return err;
}

static int do_relocs(struct host1x_job *job, struct host1x_job_gather *g)
{
	u32 last_page = ~0;
	void *cmdbuf_page_addr = NULL;
	struct host1x_bo *cmdbuf = g->bo;
	unsigned int i;

	/* pin & patch the relocs for one gather */
	for (i = 0; i < job->num_relocs; i++) {
		struct host1x_reloc *reloc = &job->relocs[i];
		u32 reloc_addr = (job->reloc_addr_phys[i] +
				  reloc->target.offset) >> reloc->shift;
		u32 *target;

		/* skip all other gathers */
		if (cmdbuf != reloc->cmdbuf.bo)
			continue;

		if (IS_ENABLED(CONFIG_TEGRA_HOST1X_FIREWALL)) {
			target = (u32 *)job->gather_copy_mapped +
					reloc->cmdbuf.offset / sizeof(u32) +
						g->offset / sizeof(u32);
			goto patch_reloc;
		}

		if (last_page != reloc->cmdbuf.offset >> PAGE_SHIFT) {
			if (cmdbuf_page_addr)
				host1x_bo_kunmap(cmdbuf, last_page,
						 cmdbuf_page_addr);

			cmdbuf_page_addr = host1x_bo_kmap(cmdbuf,
					reloc->cmdbuf.offset >> PAGE_SHIFT);
			last_page = reloc->cmdbuf.offset >> PAGE_SHIFT;

			if (unlikely(!cmdbuf_page_addr)) {
				pr_err("Could not map cmdbuf for relocation\n");
				return -ENOMEM;
			}
		}

		target = cmdbuf_page_addr + (reloc->cmdbuf.offset & ~PAGE_MASK);
patch_reloc:
		*target = reloc_addr;
	}

	if (cmdbuf_page_addr)
		host1x_bo_kunmap(cmdbuf, last_page, cmdbuf_page_addr);

	return 0;
}

int host1x_job_pin(struct host1x_job *job, struct device *dev)
{
	int err;
	unsigned int i, j;
	struct host1x *host = dev_get_drvdata(dev->parent);

	/* bump syncpoints refcount */
	host1x_syncpt_get(job->syncpt);

	/* perform basic validations */
	err = host1x_firewall_check_job(host, job, dev);
	if (err)
		goto out;
	/* pin memory */
	err = pin_job(host, job);
	if (err)
		goto out;

	if (IS_ENABLED(CONFIG_TEGRA_HOST1X_FIREWALL)) {
		err = host1x_firewall_copy_gathers(host, job, dev);
		if (err)
			goto out;
	}

	/* patch gathers */
	for (i = 0; i < job->num_gathers; i++) {
		struct host1x_job_gather *g = &job->gathers[i];

		/* process each gather mem only once */
		if (g->handled)
			continue;

		/* copy_gathers() sets gathers base if firewall is enabled */
		if (!IS_ENABLED(CONFIG_TEGRA_HOST1X_FIREWALL))
			g->base = job->gather_addr_phys[i];

		for (j = i + 1; j < job->num_gathers; j++) {
			if (job->gathers[j].bo == g->bo) {
				job->gathers[j].handled = true;
				job->gathers[j].base = g->base;
			}
		}

		err = do_relocs(job, g);
		if (err)
			break;
	}

out:
	if (err)
		host1x_job_unpin(job);

	return err;
}
EXPORT_SYMBOL(host1x_job_pin);

void host1x_job_unpin(struct host1x_job *job)
{
	struct host1x *host = dev_get_drvdata(job->channel->dev->parent);
	unsigned int i;

	for (i = 0; i < job->num_unpins; i++) {
		struct host1x_job_unpin_data *unpin = &job->unpins[i];

		if (!IS_ENABLED(CONFIG_TEGRA_HOST1X_FIREWALL) &&
		    unpin->size && host->domain) {
			iommu_unmap(host->domain, job->addr_phys[i],
				    unpin->size);
			free_iova(&host->iova,
				iova_pfn(&host->iova, job->addr_phys[i]));
		}

		host1x_bo_unpin(unpin->bo, unpin->sgt);
		host1x_bo_put(unpin->bo);
	}

	if (job->gather_copy_size)
		dma_free_wc(job->channel->dev, job->gather_copy_size,
			    job->gather_copy_mapped, job->gather_copy);

	if (job->syncpt)
		host1x_syncpt_put(job->syncpt);

	job->num_unpins = 0;
	job->gather_copy_size = 0;
	job->syncpt = NULL;
}
EXPORT_SYMBOL(host1x_job_unpin);

/*
 * Debug routine used to dump job entries
 */
void host1x_job_dump(struct device *dev, struct host1x_job *job)
{
	dev_dbg(dev, "    SYNCPT_ID   %d\n", job->syncpt->id);
	dev_dbg(dev, "    SYNCPT_VAL  %d\n", job->syncpt_end);
	dev_dbg(dev, "    FIRST_GET   0x%x\n", job->first_get);
	dev_dbg(dev, "    TIMEOUT     %d\n", job->timeout);
	dev_dbg(dev, "    NUM_SLOTS   %d\n", job->num_slots);
	dev_dbg(dev, "    NUM_HANDLES %d\n", job->num_unpins);
}
