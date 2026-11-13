// SPDX-License-Identifier: GPL-2.0
/*
 * The Helix video codec of the Ingenic X2000 for programs: /dev/helix.
 *
 * The Helix runs a job from a chain of register writes that it reads from
 * memory, and works on buffers it is given by physical address. This driver
 * has what a program cannot do itself: the codec's power, its interrupt,
 * memory at a known physical address and what the caches need around a job.
 * Everything that reads a picture or builds a chain is the program's (Datel
 * OS: k1os-camera, helix*.zig). Nothing here parses data: the vendor's driver
 * read JPEG headers in the kernel without checking a length, and one damaged
 * camera frame stopped the SoC.
 *
 * A program opens /dev/helix, asks for memory once (K1OS_HELIX_ALLOC), maps
 * it, writes a chain and its input there and runs the job (K1OS_HELIX_RUN),
 * which returns when the codec is done and stopped. The jobs of all programs
 * run one after another.
 *
 * The codec keeps its power between jobs and loses it half a second after the
 * last one. Its power gate takes 0.74 ms to open, and the clock framework
 * waits for that with interrupts off: the vendor's driver, which switched the
 * power for every job, did so sixty times a second under a camera stream.
 *
 * The memory is mapped cached: uncached, as the vendor's driver mapped its
 * buffers, a copy is 20 times slower on this SoC. A job therefore names the
 * ranges the program wrote for the codec and the ranges the codec writes for
 * the program, and the driver does for them what the DMA API does for a
 * driver's own buffers. The program must not touch those ranges while the job
 * runs. Whole cache lines are meant: a range's neighbours within 64 bytes
 * share its fate.
 *
 * The device is root's: a chain can make the codec read and write any memory.
 * The driver checks only that a chain lies in the caller's memory and ends.
 */

#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

#include <asm/mipsregs.h>

/* ---- the interface; helix.zig has the same in Zig ---- */

#define K1OS_HELIX_RANGES	8

struct k1os_helix_alloc {
	__u32 bytes;		/* in: wanted; out: got, whole pages */
	__u32 phys;		/* out: the address the codec has for the first byte */
};

struct k1os_helix_range {
	__u32 offset;		/* in the memory of K1OS_HELIX_ALLOC */
	__u32 bytes;		/* 0: an unused entry */
};

struct k1os_helix_run {
	__u32 chain;		/* in: offset of the chain, a multiple of 128 */
	__u32 timeout_ms;	/* in: how long the job may take */
	/* in: what the program wrote and the codec reads (the chain goes without saying) */
	struct k1os_helix_range to_codec[K1OS_HELIX_RANGES];
	/* in: what the codec writes and the program reads */
	struct k1os_helix_range from_codec[K1OS_HELIX_RANGES];
	__u32 status;		/* out: the scheduler's status register */
	__u32 bytes;		/* out, H.264: bytes of slice data */
	__u32 escapes;		/* out, H.264: escape bytes added to them */
	__u32 reserved;
};

/* ENXIO: no memory yet. EINVAL: a chain or range outside the memory, or a
 * chain without end. ETIMEDOUT: no interrupt in time (status is filled in);
 * the codec was stopped and takes the next job. EIO: the codec did not stop;
 * it stays powered and takes no more jobs. */
#define K1OS_HELIX_ALLOC	_IOWR('H', 0, struct k1os_helix_alloc)
#define K1OS_HELIX_RUN		_IOWR('H', 1, struct k1os_helix_run)

/* ---- the codec ---- */

#define SCH_GLBC		0x00000
#define SCH_GLBC_HIAXI		BIT(9)
/* end of frame, bitstream error, TLB error, chain error, bitstream buffer full */
#define SCH_GLBC_JOB_IRQS	(BIT(16) | BIT(17) | BIT(18) | BIT(20) | BIT(28))
#define SCH_GLBC_ALL_IRQS	(GENMASK(21, 16) | GENMASK(29, 28))
#define CFGC_SW_RESET		0x00004
#define CFGC_SW_RESET_RST	BIT(1)
#define CFGC_SW_RESET_BUS_IDLE	BIT(2)
#define SCH_STAT		0x00034
#define CFGC_ACM_CTRL		0x00084
#define CFGC_ACM_CTRL_RUN	BIT(0)
#define SDE_BYTES		0x90038
#define SDE_ESCAPES		0x9003c

/* A chain is pairs of words, a value and where it goes; the codec takes the
 * chain's address without its low seven bits. */
#define CHAIN_ALIGN		128
#define CHAIN_VALID		BIT(31)
#define CHAIN_LAST		BIT(30)
#define CHAIN_MAX_WORDS		16384

#define TIMEOUT_MAX_MS		5000
#define STOP_TIMEOUT_US		20000
/* without a job for this long, the codec's power goes */
#define IDLE_MS			500

struct k1os_helix {
	struct device *dev;
	void __iomem *regs;
	struct clk *power;
	int irq;
	struct miscdevice misc;
	struct mutex job;		/* one job at a time; the fields below */
	struct completion done;
	u32 status;			/* as the interrupt found it */
	struct delayed_work idle;
	unsigned long last_job;		/* in jiffies */
	bool powered;
	bool stuck;			/* the codec did not stop */
};

struct k1os_helix_client {
	struct k1os_helix *helix;
	struct mutex lock;
	void *virt;			/* the kernel's own view, kept to free it */
	dma_addr_t phys;
	size_t bytes;
};

/*
 * Stops the codec: its interrupts off, then a reset, which it grants once it
 * has no transfer left on the bus. Only then may its power go: a block that
 * loses power in the middle of a transfer stops the bus, and the SoC with it.
 */
static int k1os_helix_stop(struct k1os_helix *h)
{
	u32 v;

	writel(readl(h->regs + SCH_GLBC) & ~SCH_GLBC_ALL_IRQS, h->regs + SCH_GLBC);
	writel(CFGC_SW_RESET_RST, h->regs + CFGC_SW_RESET);
	return readl_poll_timeout(h->regs + CFGC_SW_RESET, v,
				  v & CFGC_SW_RESET_BUS_IDLE, 50, STOP_TIMEOUT_US);
}

static irqreturn_t k1os_helix_irq(int irq, void *data)
{
	struct k1os_helix *h = data;

	h->status = readl(h->regs + SCH_STAT);
	writel(readl(h->regs + SCH_GLBC) & ~SCH_GLBC_ALL_IRQS, h->regs + SCH_GLBC);
	complete(&h->done);
	return IRQ_HANDLED;
}

/* Takes the codec's power once no job has come for a while. */
static void k1os_helix_idle(struct work_struct *work)
{
	struct k1os_helix *h = container_of(to_delayed_work(work),
					    struct k1os_helix, idle);

	mutex_lock(&h->job);
	/* a job that ran since this was scheduled has scheduled it again */
	if (h->powered && !h->stuck &&
	    time_after_eq(jiffies, h->last_job + msecs_to_jiffies(IDLE_MS))) {
		clk_disable_unprepare(h->power);
		h->powered = false;
	}
	mutex_unlock(&h->job);
}

/*
 * The vendor's driver sets these bits of the CPU's Config7 register, and
 * clears the ones above them, on the CPU that starts a job. What they are for
 * is not documented; the codec has only ever run with them.
 */
static void k1os_helix_cpu_setup(void)
{
	unsigned long flags;

	local_irq_save(flags);
	write_c0_config7((read_c0_config7() | 0x340) & 0x3ff);
	local_irq_restore(flags);
}

static bool k1os_helix_range_ok(const struct k1os_helix_client *c,
				const struct k1os_helix_range *r)
{
	return r->offset <= c->bytes && r->bytes <= c->bytes - r->offset;
}

/*
 * The length of the chain at an offset. It has to end inside the caller's
 * memory: the codec would read on, into someone else's.
 */
static int k1os_helix_chain_bytes(const struct k1os_helix_client *c, u32 offset,
				  u32 *bytes)
{
	const u32 *entry;
	u32 words, max;

	if (offset % CHAIN_ALIGN || offset >= c->bytes)
		return -EINVAL;
	max = min_t(size_t, (c->bytes - offset) / 4, CHAIN_MAX_WORDS);
	/* the program wrote it through its cached mapping: read it cached too */
	entry = phys_to_virt(c->phys + offset);
	for (words = 0; words + 2 <= max; words += 2) {
		u32 where = READ_ONCE(entry[words + 1]);

		if (!(where & CHAIN_VALID))
			return -EINVAL;
		if (where & CHAIN_LAST) {
			*bytes = (words + 2) * 4;
			return 0;
		}
	}
	return -EINVAL;
}

static void k1os_helix_sync(const struct k1os_helix_client *c,
			    const struct k1os_helix_range *r,
			    enum dma_data_direction dir, bool for_codec)
{
	struct device *dev = c->helix->dev;
	int i;

	for (i = 0; i < K1OS_HELIX_RANGES; i++) {
		if (!r[i].bytes)
			continue;
		if (for_codec)
			dma_sync_single_for_device(dev, c->phys + r[i].offset,
						   r[i].bytes, dir);
		else
			dma_sync_single_for_cpu(dev, c->phys + r[i].offset,
						r[i].bytes, dir);
	}
}

static long k1os_helix_run(struct k1os_helix_client *c,
			   struct k1os_helix_run __user *arg)
{
	struct k1os_helix *h = c->helix;
	struct k1os_helix_run run;
	unsigned long left;
	u32 chain_bytes;
	long ret;
	int i;

	if (copy_from_user(&run, arg, sizeof(run)))
		return -EFAULT;
	if (!c->bytes)
		return -ENXIO;
	ret = k1os_helix_chain_bytes(c, run.chain, &chain_bytes);
	if (ret)
		return ret;
	for (i = 0; i < K1OS_HELIX_RANGES; i++) {
		if (!k1os_helix_range_ok(c, &run.to_codec[i]) ||
		    !k1os_helix_range_ok(c, &run.from_codec[i]))
			return -EINVAL;
	}
	run.timeout_ms = clamp_t(u32, run.timeout_ms, 1, TIMEOUT_MAX_MS);
	run.status = 0;
	run.bytes = 0;
	run.escapes = 0;

	ret = mutex_lock_interruptible(&h->job);
	if (ret)
		return ret;
	if (h->stuck) {
		ret = -EIO;
		goto unlock;
	}

	/* What the program wrote goes to memory, and what the codec is about
	 * to write leaves the caches. */
	dma_sync_single_for_device(h->dev, c->phys + run.chain, chain_bytes,
				   DMA_TO_DEVICE);
	k1os_helix_sync(c, run.to_codec, DMA_TO_DEVICE, true);
	k1os_helix_sync(c, run.from_codec, DMA_FROM_DEVICE, true);

	if (!h->powered) {
		ret = clk_prepare_enable(h->power);
		if (ret)
			goto unlock;
		h->powered = true;
	}
	k1os_helix_cpu_setup();

	if (k1os_helix_stop(h)) {
		dev_err(h->dev, "the codec's bus is busy before a job\n");
		h->stuck = true;
		ret = -EIO;
		goto unlock;
	}

	reinit_completion(&h->done);
	enable_irq(h->irq);
	writel(SCH_GLBC_HIAXI | SCH_GLBC_JOB_IRQS, h->regs + SCH_GLBC);
	writel((c->phys + run.chain) | CFGC_ACM_CTRL_RUN, h->regs + CFGC_ACM_CTRL);

	left = wait_for_completion_timeout(&h->done,
					   msecs_to_jiffies(run.timeout_ms));
	/* waits for a handler that is still running, too */
	disable_irq(h->irq);
	if (left || try_wait_for_completion(&h->done)) {
		run.status = h->status;
	} else {
		run.status = readl(h->regs + SCH_STAT);
		ret = -ETIMEDOUT;
	}
	run.bytes = readl(h->regs + SDE_BYTES);
	run.escapes = readl(h->regs + SDE_ESCAPES);

	if (k1os_helix_stop(h)) {
		/* It may still be writing: it keeps its power, and the memory
		 * of this client is never given to anyone else. */
		dev_err(h->dev, "the codec did not stop, status %08x\n", run.status);
		h->stuck = true;
		ret = -EIO;
		goto unlock;
	}
	h->last_job = jiffies;
	mod_delayed_work(system_wq, &h->idle, msecs_to_jiffies(IDLE_MS));

	k1os_helix_sync(c, run.from_codec, DMA_FROM_DEVICE, false);
unlock:
	mutex_unlock(&h->job);
	if (ret != -EIO && copy_to_user(arg, &run, sizeof(run)))
		return -EFAULT;
	return ret;
}

static long k1os_helix_alloc(struct k1os_helix_client *c,
			     struct k1os_helix_alloc __user *arg)
{
	struct k1os_helix_alloc a;
	dma_addr_t phys;
	size_t bytes;
	void *virt;
	long ret = 0;

	if (copy_from_user(&a, arg, sizeof(a)))
		return -EFAULT;
	bytes = PAGE_ALIGN(a.bytes);
	if (!a.bytes || bytes < a.bytes)
		return -EINVAL;

	mutex_lock(&c->lock);
	if (c->bytes) {
		ret = -EBUSY;
		goto unlock;
	}
	/* From the pool of the node's memory-region, or not at all. The pool
	 * hands out powers of two: 9 MiB take 16 MiB of it. */
	virt = dma_alloc_coherent(c->helix->dev, bytes, &phys, GFP_KERNEL);
	if (!virt) {
		ret = -ENOMEM;
		goto unlock;
	}
	c->virt = virt;
	c->phys = phys;
	c->bytes = bytes;
	a.bytes = bytes;
	a.phys = phys;
unlock:
	mutex_unlock(&c->lock);
	/* on a fault here the memory stays with the file and goes when it closes */
	if (!ret && copy_to_user(arg, &a, sizeof(a)))
		ret = -EFAULT;
	return ret;
}

static long k1os_helix_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct k1os_helix_client *c = file->private_data;

	switch (cmd) {
	case K1OS_HELIX_ALLOC:
		return k1os_helix_alloc(c, (void __user *)arg);
	case K1OS_HELIX_RUN:
		return k1os_helix_run(c, (void __user *)arg);
	default:
		return -ENOTTY;
	}
}

static int k1os_helix_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct k1os_helix_client *c = file->private_data;
	unsigned long pages = vma_pages(vma);
	int ret = -ENXIO;

	mutex_lock(&c->lock);
	if (c->bytes) {
		unsigned long have = c->bytes >> PAGE_SHIFT;

		ret = -EINVAL;
		if (vma->vm_pgoff <= have && pages <= have - vma->vm_pgoff)
			/* with the protection it came with: cached */
			ret = remap_pfn_range(vma, vma->vm_start,
					      PHYS_PFN(c->phys) + vma->vm_pgoff,
					      pages << PAGE_SHIFT, vma->vm_page_prot);
	}
	mutex_unlock(&c->lock);
	return ret;
}

static int k1os_helix_open(struct inode *inode, struct file *file)
{
	/* misc_open() left the miscdevice here */
	struct k1os_helix *h = container_of(file->private_data, struct k1os_helix, misc);
	struct k1os_helix_client *c;

	c = kzalloc(sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;
	c->helix = h;
	mutex_init(&c->lock);
	file->private_data = c;
	return 0;
}

static int k1os_helix_release(struct inode *inode, struct file *file)
{
	struct k1os_helix_client *c = file->private_data;
	struct k1os_helix *h = c->helix;

	/* No job of this file runs any more, and its mappings are gone. */
	if (c->bytes && !READ_ONCE(h->stuck)) {
		/* nothing of it stays in the caches for whoever gets it next */
		dma_sync_single_for_device(h->dev, c->phys, c->bytes, DMA_BIDIRECTIONAL);
		dma_free_coherent(h->dev, c->bytes, c->virt, c->phys);
	}
	kfree(c);
	return 0;
}

static const struct file_operations k1os_helix_fops = {
	.owner = THIS_MODULE,
	.open = k1os_helix_open,
	.release = k1os_helix_release,
	.unlocked_ioctl = k1os_helix_ioctl,
	.mmap = k1os_helix_mmap,
	.llseek = noop_llseek,
};

static void k1os_helix_release_pool(void *dev)
{
	of_reserved_mem_device_release(dev);
}

static int k1os_helix_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct k1os_helix *h;
	int ret;

	h = devm_kzalloc(dev, sizeof(*h), GFP_KERNEL);
	if (!h)
		return -ENOMEM;
	h->dev = dev;
	mutex_init(&h->job);
	init_completion(&h->done);
	INIT_DELAYED_WORK(&h->idle, k1os_helix_idle);

	h->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(h->regs))
		return PTR_ERR(h->regs);

	/* a power gate behind the codec's clock gate */
	h->power = devm_clk_get(dev, "power_helix");
	if (IS_ERR(h->power))
		return dev_err_probe(dev, PTR_ERR(h->power), "no power gate\n");

	ret = of_reserved_mem_device_init(dev);
	if (ret)
		return dev_err_probe(dev, ret, "no memory-region\n");
	ret = devm_add_action_or_reset(dev, k1os_helix_release_pool, dev);
	if (ret)
		return ret;

	/* The interrupt is enabled only while a job runs: with its power
	 * gone, the codec has no say about its line. */
	h->irq = platform_get_irq(pdev, 0);
	if (h->irq < 0)
		return h->irq;
	ret = devm_request_irq(dev, h->irq, k1os_helix_irq, IRQF_NO_AUTOEN,
			       dev_name(dev), h);
	if (ret)
		return dev_err_probe(dev, ret, "no interrupt\n");

	h->misc.minor = MISC_DYNAMIC_MINOR;
	h->misc.name = "helix";
	h->misc.fops = &k1os_helix_fops;
	h->misc.mode = 0600;
	h->misc.parent = dev;
	ret = misc_register(&h->misc);
	if (ret)
		return ret;

	dev_info(dev, "the Helix codec is /dev/helix\n");
	return 0;
}

static const struct of_device_id k1os_helix_of_match[] = {
	{ .compatible = "ingenic,x2000-helix" },
	{}
};

/* No unbinding: open files would be left with a device that is gone. */
static struct platform_driver k1os_helix_driver = {
	.probe = k1os_helix_probe,
	.driver = {
		.name = "k1os-helix",
		.of_match_table = k1os_helix_of_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(k1os_helix_driver);

MODULE_DESCRIPTION("The Helix codec of the Ingenic X2000 for programs");
MODULE_LICENSE("GPL");
