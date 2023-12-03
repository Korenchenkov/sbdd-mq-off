#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/bio.h>
#include <linux/bvec.h>
#include <linux/init.h>
#include <linux/wait.h>
#include <linux/stat.h>
#include <linux/slab.h>
#include <linux/numa.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/genhd.h>
#include <linux/blkdev.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/vmalloc.h>
#include <linux/moduleparam.h>
#include <linux/spinlock_types.h>
#include <linux/hdreg.h>
#include <trace/events/block.h>

#define SBDD_SECTOR_SHIFT      9
#define SBDD_SECTOR_SIZE       (1 << SBDD_SECTOR_SHIFT)
#define SBDD_MIB_SECTORS       (1 << (20 - SBDD_SECTOR_SHIFT))
#define SBDD_NAME              "sbdd"
#define SBDD_BDEV_MODE (FMODE_READ | FMODE_WRITE | FMODE_EXCL)


/* IOCTL */
#define SBDD_DO_IT _IOW( 0xad, 0, char * )
#define SBDD_NAME_0 SBDD_NAME "0"

struct sbdd {
	wait_queue_head_t       exitwait;
	spinlock_t              datalock;
	atomic_t                deleting;
	atomic_t                refs_cnt;
	sector_t                capacity;
	u8                      *data;
	int						is_active;
	struct gendisk          *gd;
	struct request_queue    *q;
	struct block_device 	*blkdev;
};


static	struct bio		*cloned_bio = NULL;
static	struct bio_set	*bs = NULL; 
static	struct sbdd     __sbdd;
static	int				__sbdd_major = 0;


module_param(__sbdd_major, int, 0);

static sector_t sbdd_xfer(struct bio_vec* bvec, sector_t pos, int dir)
{
	void *buff = page_address(bvec->bv_page) + bvec->bv_offset;
	sector_t len = bvec->bv_len >> SBDD_SECTOR_SHIFT;
	size_t offset;
	size_t nbytes;

	if (pos + len > __sbdd.capacity)
		len = __sbdd.capacity - pos;

	offset = pos << SBDD_SECTOR_SHIFT;
	nbytes = len << SBDD_SECTOR_SHIFT;

	spin_lock(&__sbdd.datalock);

	if (dir)
		memcpy(__sbdd.data + offset, buff, nbytes);
	else
		memcpy(buff, __sbdd.data + offset, nbytes);

	spin_unlock(&__sbdd.datalock);

	pr_debug("pos=%6llu len=%4llu %s\n", pos, len, dir ? "written" : "read");

	return len;
}

static void sbdd_xfer_bio(struct bio *bio)
{
	struct bvec_iter iter;
	struct bio_vec bvec;
	int dir = bio_data_dir(bio);
	sector_t pos = bio->bi_iter.bi_sector;

	bio_for_each_segment(bvec, bio, iter)
		pos += sbdd_xfer(&bvec, pos, dir);
}

static blk_qc_t sbdd_make_request(struct request_queue *q, struct bio *bio)
{
	printk(KERN_ALERT "sbdd: make request %-5s block %-12llu #pages %-4hu total-size "
        "%-10u\n", bio_data_dir(bio) == WRITE ? "write" : "read",
        bio->bi_iter.bi_sector, bio->bi_vcnt, bio->bi_iter.bi_size); 

    if (atomic_read(&__sbdd.deleting)) {
		pr_err("unable to process bio while deleting\n");
		bio_io_error(bio);
		return BLK_STS_IOERR;
	}

	atomic_inc(&__sbdd.refs_cnt);
	
	/* Clone bio request */ 
	bs = &fs_bio_set;
    cloned_bio = bio_clone_fast(bio, GFP_KERNEL, bs); 
    
	if (!cloned_bio) { 
        printk(KERN_ERR "Failed to clone bio request\n"); 
        return BLK_QC_T_NONE; 
    } 
    //cloned_bio->bi_end_io = bio_endio;
	//cloned_bio->bi_iter.bi_size = SBDD_SECTOR_SIZE ;
	bio_set_dev(bio, __sbdd.blkdev);
	
	//sbdd_xfer_bio(cloned_bio);
	
	submit_bio(bio); 
	
	//bio_endio(bio);

	if (atomic_dec_and_test(&__sbdd.refs_cnt))
		wake_up(&__sbdd.exitwait);

	return BLK_STS_OK;
}

static struct block_device *sbdd_bdev_open(char dev_path[])
{
    /* Open underlying device */
   
	printk(KERN_ALERT "Opened %s\n", dev_path);

	struct block_device *blkdev = blkdev_get_by_path(dev_path, SBDD_BDEV_MODE, &__sbdd);
    
	if (IS_ERR(blkdev)) {
        printk(KERN_ALERT "sbdd: error opening raw device <%lu>\n", PTR_ERR(blkdev));
        return NULL;
    }

	return blkdev;
}


static int sbdd_start(char dev_path[])
{
   
    printk ("Will open %s.\n", dev_path);

    if (!(__sbdd.blkdev = sbdd_bdev_open(dev_path)))
        return -ENODEV;
    
    /* Set up our internal device */
    __sbdd.capacity = get_capacity(__sbdd.blkdev->bd_disk);
    printk(KERN_ALERT "sbdd: Device real capacity: %llu\n", __sbdd.capacity);
		
	pr_info("allocating data\n");
	__sbdd.data = vzalloc(__sbdd.capacity);
	if (!__sbdd.data) {
		pr_err("unable to alloc data\n");
		return -ENOMEM;
	}

    set_capacity(__sbdd.gd, __sbdd.capacity); 

    printk(KERN_ALERT "sbdd: done initializing successfully\n");
    __sbdd.is_active = 1;


    return 0;

}


static int sbdd_ioctl(struct block_device *bdev, fmode_t mode,
		     unsigned int cmd, unsigned long arg)
{
    char path[80];
	void __user *argp = (void __user *)arg;    

    switch (cmd)
    {
    case SBDD_DO_IT:
        printk(KERN_ALERT "\n*** I DID IT!!!!!!! ***\n\n");

        if (copy_from_user(path, argp, sizeof(path)))
            return -EFAULT;

        return sbdd_start(path);
    default:
        return -ENOTTY;
    }
}


int sbdd_getgeo(struct block_device * block_device, struct hd_geometry * geo)
{
	long size;

	/* We have no real geometry, of course, so make something up. */
	size = __sbdd.capacity;
	geo->cylinders = (size & ~0x3f) >> 6;
	geo->heads = 4;
	geo->sectors = 1;
	geo->start = 0;
	return 0;
}
/*
 * The device operations structure.
 */
static struct block_device_operations  __sbdd_bdev_ops = {
	.owner = THIS_MODULE,
	.getgeo = sbdd_getgeo,
	.ioctl  = sbdd_ioctl,
};

static int __init sbdd_init(void)
{
	int ret = 0;


	spin_lock_init(&__sbdd.datalock);
	init_waitqueue_head(&__sbdd.exitwait);

	/* blk_alloc_queue() instead of blk_init_queue() so it won't set up the
     * queue for requests.
     */
    pr_info("allocating queue\n");
	__sbdd.q = blk_alloc_queue(GFP_KERNEL);
	if (!__sbdd.q) {
		pr_err("call blk_alloc_queue() failed\n");
		return -EINVAL;
	}

    blk_queue_make_request(__sbdd.q, sbdd_make_request);

    /* Configure queue */
	blk_queue_logical_block_size(__sbdd.q, SBDD_SECTOR_SIZE); 

	/* Get registered */
	if ((__sbdd_major  = register_blkdev(0, SBDD_NAME)) < 0)
    {
		printk(KERN_ALERT "sbdd: unable to get major number\n");
		goto error_after_alloc_queue;
	}

	/* Gendisk structure */
	if (!(__sbdd.gd = alloc_disk(1)))
		goto error_after_redister_blkdev;

	__sbdd.gd->major = __sbdd_major;
	__sbdd.gd->first_minor = 0;
	__sbdd.gd->fops = &__sbdd_bdev_ops;
	strcpy(__sbdd.gd->disk_name, SBDD_NAME_0);
	__sbdd.gd->queue = __sbdd.q;
	
    /*
	Allocating gd does not make it available, add_disk() required.
	After this call, gd methods can be called at any time. Should not be
	called before the driver is fully initialized and ready to process reqs.
	*/
	pr_info("adding disk\n");
    add_disk(__sbdd.gd);

    printk(KERN_ALERT "sbdd: init done\n");

	return ret;

error_after_redister_blkdev:
	unregister_blkdev(__sbdd_major, SBDD_NAME);
error_after_alloc_queue:
    blk_cleanup_queue(__sbdd.q);
	return -EFAULT;
	
}

static void sbdd_delete(void)
{
	atomic_set(&__sbdd.deleting, 1);

	wait_event(__sbdd.exitwait, !atomic_read(&__sbdd.refs_cnt));

    /* Cleanup and release resources */
    if (__sbdd.blkdev) {
		blkdev_put(__sbdd.blkdev, SBDD_BDEV_MODE);
		bdput(__sbdd.blkdev);        
    }
	__sbdd.blkdev = NULL; 

	/* gd will be removed only after the last reference put */
	if (__sbdd.gd) {
		pr_info("deleting disk\n");
		del_gendisk(__sbdd.gd);
	}

	if (__sbdd.q) {
		pr_info("cleaning up queue\n");
		blk_cleanup_queue(__sbdd.q);
	}

	if (__sbdd.gd)
		put_disk(__sbdd.gd);

	if (__sbdd.data) {
		pr_info("freeing data\n");
		vfree(__sbdd.data);
	}

	memset(&__sbdd, 0, sizeof(struct sbdd));

	if (__sbdd_major > 0) {
		pr_info("unregistering blkdev\n");
		unregister_blkdev(__sbdd_major, SBDD_NAME);
		__sbdd_major = 0;
	}
}


/*
Note __exit is for the compiler to place this code in a special ELF section.
Sometimes such functions are simply discarded (e.g. when module is built
directly into the kernel). There is also __exitdata note.
*/
static void __exit sbdd_exit(void)
{
	pr_info("exiting...\n");
	sbdd_delete();
	pr_info("exiting complete\n");
}

/* Called on module loading. Is mandatory. */
module_init(sbdd_init);

/* Called on module unloading. Unloading module is not allowed without it. */
module_exit(sbdd_exit);

/* Note for the kernel: a free license module. A warning will be outputted without it. */
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Simple Block Device Driver");
