/*
 * mw-vdma.c
 * 
 * Copyright 2015, MathWorks, Inc.
 * 
 * MathWorks AXI Video DMA client driver
 * 
 */

#define DEBUG
#define _DEBUG

#include <linux/amba/xilinx_dma.h>
#include <linux/cdev.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/fcntl.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/sysctl.h>
#include <linux/uaccess.h>
#include <linux/dma-contiguous.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include "mw-vdma.h"


struct task_struct *ktest_task;

dev_t g_vdma_id = 0;
static unsigned char g_vdma_minor = 0;
static struct vdma_chan mwchan[2];
static DECLARE_WAIT_QUEUE_HEAD(thread_wait);

static short int stride = 0;
static short int testmode = 0;
module_param(testmode, short, S_IRUGO);
MODULE_PARM_DESC(testmode, "Specify testmode: 0=Testmode off, 1=Test mode on.");

static unsigned int hsize = 1920;
module_param(hsize, uint, S_IRUGO);
MODULE_PARM_DESC(hsize, "Horizontal video frame size.");

static unsigned int vsize = 1080;
module_param(vsize, uint, S_IRUGO);
MODULE_PARM_DESC(vsize, "Vertical video frame size.");

static unsigned int numframes= 5;
module_param(numframes, uint, S_IRUGO);
MODULE_PARM_DESC(numframes, "Number of video frames in buffer.");

static short int bpp = 4;
module_param(bpp, short, S_IRUGO);
MODULE_PARM_DESC(bpp, "Bytes-per-pixer for the video data.");

/*
 * forward declarations
 */
static int vdma_start_frame_transfer(struct vdma_chan *vchan);
static int vdma_stop_frame_transfer(struct vdma_chan *vchan);
static int vdma_mmap(struct file *fp, struct vm_area_struct *vma);
int thread_function(void *data) ;
static void print_channel_info(struct vdma_chan * vchan) ;

/*
 * channel_code
 * 0 == vdma0 - one channel
 * 1 == vdma0,vdma1 - 2 channels
 * 2 == vdma0,vdma1,vdma2 - 3 channels 
 * 3 == vdma0,vdma1,vdma2,vdma3 - 4 channels
 */
static unsigned int channel_code;

/*
 * @brief vdma_open
 */
static int vdma_open(struct inode *i_node, struct file *f) {
    struct vdma_dev *mwdev;
    if (NULL == i_node){
        pr_err("NULL node pointer\n");
        return -ENODEV;
    }
    mwdev = container_of (i_node->i_cdev, struct vdma_dev, cdev);
    f->private_data = mwdev;
    dev_dbg(&IP2DEV(mwdev),"Open VDMA device descriptor\n");
    return 0;
}

/*
 * @brief vdma_close
 */
static int vdma_close(struct inode *i_node, struct file *fp) {
    struct vdma_dev *mwdev = fp->private_data;
    if (NULL == mwdev) {
        return -ENODEV;
    }
    dev_dbg(&IP2DEV(mwdev),"Close VDMA device descriptor\n");
    return 0;
}

/*
 * @brief vdma_extmem_open 
 */
static void vdma_extmem_open(struct vm_area_struct *vma) {
    struct vdma_dev * mwdev = vma->vm_private_data;
    dev_info(&IP2DEV(mwdev), "MAP external memory, virt %lx, phys %lx \n", vma->vm_start, vma->vm_pgoff << PAGE_SHIFT);
}

/*
 * @brief vdma_extmem_close
 */
static void vdma_extmem_close(struct vm_area_struct *vma) {
    struct vdma_dev * mwdev = vma->vm_private_data;
    dev_info(&IP2DEV(mwdev), "UNMAP external memory.\n");
    if (mwdev->size) {
        dev_info(&IP2DEV(mwdev), "free dma memory.\n");
        dma_free_coherent(&IP2DEV(mwdev), mwdev->size, mwdev->virt, mwdev->phys);
        mwdev->size = 0;
        mwdev->virt = NULL;
        mwdev->phys = 0;
    }
}

/*
 * @brief vdma_mmio_open
 */
static void vdma_mmio_open(struct vm_area_struct *vma)
{
    struct vdma_dev * mwdev = vma->vm_private_data;
    dev_info(&IP2DEV(mwdev), "MAP AXI4-Register space, virt %lx, phys %lx \n", vma->vm_start, vma->vm_pgoff << PAGE_SHIFT);
}

/*
 * @brief vdma_mmio_close
 */
static void vdma_mmio_close(struct vm_area_struct *vma) {
    struct vdma_dev * mwdev = vma->vm_private_data;
    dev_info(&IP2DEV(mwdev), "Close Memory mapped IO region.\n");
}

/*
 * @brief vdma_mmap_fault
 */
static int vdma_mmio_fault(struct vm_area_struct *vma, struct vm_fault *vmf) {
    struct vdma_dev * mwdev = vma->vm_private_data;
    struct page *thisPage;
    unsigned long offset;
    offset = (vmf->pgoff - vma->vm_pgoff) << PAGE_SHIFT;
    thisPage = virt_to_page(mwdev->mem->start + offset);
    get_page(thisPage);
    vmf->page = thisPage;
    return 0;
}

/*
 * @brief vdma_alloc_memory
 */
static int vdma_alloc_memory(struct vdma_dev *mwdev, size_t bufsize) {
    if (mwdev == NULL) {
        dev_err(&IP2DEV(mwdev),"mwdev device pointer returned null...");
        return -ENOMEM;
    }
    if (mwdev->virt != NULL) {
        dev_err(&IP2DEV(mwdev), "DMA memory already allocated\n");		
        return -EEXIST;
    }
    mwdev->virt = dma_alloc_coherent(&IP2DEV(mwdev), bufsize, &mwdev->phys, GFP_KERNEL);
    if (mwdev->virt == NULL) {
        dev_err(&IP2DEV(mwdev), "Failed to allocate continguous memory\nUsing multiple buffers\n");
        return -ENOMEM;
    } else {
        dev_info(&IP2DEV(mwdev), "Address of DMA buffer = 0x%p, Length = %u Bytes\n", \
                (void *)virt_to_phys(mwdev->virt), bufsize);
        mwdev->size = bufsize;
    }
    return 0;
}


/*
 * @brief vdma_ioctl
 */

static long vdma_ioctl(struct file *fp, unsigned int cmd, unsigned long arg) {
    struct vdma_dev *mwdev;

    if (NULL == fp->private_data){
        pr_err("Device data is not available!");
        return -ENODEV;
    } else {
        mwdev = fp->private_data;
    }
    dev_dbg(&IP2DEV(mwdev), "In VDMA - ioctl.\n");

    return 0;
}
struct vm_operations_struct vdma_mmio_ops = {
    .open           = vdma_mmio_open,
    .close          = vdma_mmio_close,
    .fault          = vdma_mmio_fault,
}; 

struct vm_operations_struct vdma_extmem_ops = {
    .open           = vdma_extmem_open,
    .close          = vdma_extmem_close,
};

/*
 * @brief vdma_mmap
 */
static int vdma_mmap(struct file *fp, struct vm_area_struct *vma) {
    struct vdma_dev *mwdev = fp->private_data;
    size_t size = vma->vm_end - vma->vm_start;
    int status = 0;
    vma->vm_private_data = mwdev;
    dev_info(&IP2DEV(mwdev), "[MMAP] size:%X pgoff: %lx\n", size, vma->vm_pgoff);
    switch(vma->vm_pgoff) {
        case 0: 
            /* mmap the Memory Mapped I/O's base address */
            vma->vm_flags |= VM_IO | VM_DONTDUMP;
            vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
            if (remap_pfn_range(vma, vma->vm_start,
                        mwdev->mem->start >> PAGE_SHIFT, size, vma->vm_page_prot)) {
                return -EAGAIN;
            }
            vma->vm_ops = &vdma_mmio_ops;
            break;
        default:
            /* mmap the DMA region */
            status = vdma_alloc_memory(mwdev, size);
            if ((status) && (status != -EEXIST))  {
                return -ENOMEM;
            }
            dev_dbg(&IP2DEV(mwdev), "dma setup_cdev successful\n");
            status = 0;
            if (mwdev->virt == NULL){
                return -EINVAL;
            }
            vma->vm_pgoff = 0; status = dma_mmap_coherent(&IP2DEV(mwdev), vma, mwdev->virt,
                    mwdev->phys, mwdev->size);
            if (status) {
                dev_dbg(&IP2DEV(mwdev),"Remapping memory failed, error: %d\n", status);
                return status;
            }
            vma->vm_ops = &vdma_extmem_ops;
            dev_dbg(&IP2DEV(mwdev),"%s: mapped dma addr 0x%08lx at 0x%08lx, size %d\n",
                    __func__, (unsigned long)mwdev->phys, vma->vm_start,
                    mwdev->size);
            break;
    }
    return status;
}

struct file_operations vdma_cdev_fops = {
    .owner          = THIS_MODULE,
    .open           = vdma_open,
    .release        = vdma_close,
    .mmap	    = vdma_mmap,
    .unlocked_ioctl = vdma_ioctl,
};

static int vdma_of_probe(struct platform_device *pdev) {
    dev_t devt;
    int status = 0;
    struct vdma_dev *mwdev = NULL;
    struct device *dev = &pdev->dev;
    struct device_node *np = pdev->dev.of_node;
    struct vdma_chan *vchan;
    const char *pval;
    int len;

    devt = MKDEV(MAJOR(g_vdma_id), g_vdma_minor++);
    mwdev = devm_kzalloc(dev, sizeof(struct vdma_dev), GFP_KERNEL);
    if (NULL == mwdev) {
        pr_err("NULL pointer returned while allocating memory for vdma device\n");
        return -ENOMEM; 
    }
    dev_set_drvdata(dev, mwdev);
    mwdev->dev_id = devt;
    cdev_init(&mwdev->cdev, &vdma_cdev_fops);
    mwdev->cdev.owner = THIS_MODULE;
    mutex_init(&mwdev->lock); 
    status = cdev_add(&mwdev->cdev, devt, 1);
    if (status){
        dev_err(dev, "cdev_add operation failed\n");
        return status;
    }
    of_property_read_u8(np, "mw-vdma,use-mmio", &mwdev->has_mem);
    if (mwdev->has_mem == 1) {
        dev_dbg(dev, "Create and MAP MMIO resource region\n");
        mwdev->mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
        if (!mwdev->mem){
            dev_err(dev, "Failed allocate platform resource\n");
            return -ENOENT;
        }
        mwdev->regs = devm_ioremap_resource(dev, mwdev->mem);
        if (!mwdev->regs) {
            dev_err(dev, "Failed in devm_ioremap_resource\n");
            return -ENODEV;
        }
        platform_set_drvdata(pdev,mwdev);
    }
    pval = of_get_property(np, "dmas", &len);
    if (!pval){
        dev_err(dev,"Failed to read the required property \"dmas\".\nSee device tree bindings for xilinx_vdma for more information.\n");
        return status;
    }
    pval = of_get_property(np, "dma-names", &len);
    if (!pval){
        dev_err(dev,"Failed to read the required property \"dma-names\".\nSee device tree bindings for xilinx_vdma for more information.\n");
        return status;
    }
    vchan = &mwchan[TX];
    vchan->chan = dma_request_slave_channel(&pdev->dev, "vdma0");
    vchan->direction = DMA_MEM_TO_DEV;
    if (IS_ERR(vchan->chan)){
        dev_err(dev,"Unable to obtain tx-channel vdma0\n");
        return PTR_ERR(vchan->chan);
    }

    vchan = &mwchan[RX];
    vchan->chan = dma_request_slave_channel(&pdev->dev, "vdma1");
    vchan->direction = DMA_DEV_TO_MEM;
    if (IS_ERR(vchan->chan)){
        dev_err(dev,"Unable to obtain rx-channel vdma0\n");
        return PTR_ERR(vchan->chan);
    }
    mwdev->dev   = dev;
    mwdev->pdev  = pdev;
    if(np->data == NULL) {
        np->data = mwdev;
    }
    if (testmode == 1) {
        dev_info(dev, "Running the test-thread\n");
        ktest_task = kthread_run(&thread_function, mwdev, "mw-vdma-bist");
        dev_info(dev, "Kernel thread: %s\n", ktest_task->comm);
    }
    return 0; 
}

static int vdma_of_remove(struct platform_device *pdev){
    struct vdma_dev *mwdev = platform_get_drvdata(pdev);
    struct vdma_chan *vchan;
   
    kthread_stop(ktest_task);
    dev_dbg(&IP2DEV(mwdev), "Freeing channels\n");
    vchan = &mwchan[RX];
    dmaengine_terminate_all(vchan->chan);
    dma_release_channel(vchan->chan);

    vchan = &mwchan[TX];
    dmaengine_terminate_all(vchan->chan);
    dma_release_channel(vchan->chan);
    /*Remove CDEV MAP*/
    cdev_del(&mwdev->cdev);
    return 0;
}

static int vdma_prep_desc(struct vdma_dev *mwdev, unsigned int chan_id, struct mwvdma_chan_params *usrbuf){
    struct vdma_chan  *vchan = &mwchan[chan_id];
    struct dma_chan   *chan = vchan->chan;
    struct dma_device *chan_dev = chan->device;
    int i;
    int ret;
    static unsigned int mwchan_offset = 0;
    void *sg_buff;
    struct xilinx_vdma_config cfg;
    
    dev_dbg(&IP2DEV(mwdev),"Prepare descriptors.. channel id %d\n", chan_id);
    /* Set VDMA channel configuration */
    memset(&cfg, 0, sizeof(struct xilinx_vdma_config));
    cfg.frm_cnt_en = 1;
    cfg.coalesc = 1;
    if (chan_id == TX) {
        cfg.park = 1;
        cfg.gen_lock = 1;
        cfg.master  = 0;
    } else {
        cfg.park = 0;
        cfg.gen_lock = 1;
        cfg.master  = 1;
    }
    cfg.reset   = 1;

    ret = xilinx_vdma_channel_set_config(vchan->chan, &cfg);
    if (ret){
        dev_err(&IP2DEV(mwdev), "Error resetting VDMA channel %s\n", dma_chan_name(vchan->chan));
        return -EINVAL;
    }
    cfg.reset   = 0;
    ret = xilinx_vdma_channel_set_config(vchan->chan, &cfg);
    if (ret){
        dev_err(&IP2DEV(mwdev), "Error setting VDMA channel configuration for %s\n", dma_chan_name(vchan->chan));
        return -EINVAL;
    }
 

    vchan->flags   = DMA_CTRL_ACK | DMA_PREP_INTERRUPT;
    vchan->length  = (hsize*vsize*bpp); // usrbuf->length; // TODO 
    vchan->nframes = numframes;           // usrbuf->nframes; // TODO
    vchan->virt    = &(mwdev->virt[mwchan_offset]);
    vchan->phys    = mwdev->phys + mwchan_offset; /* because this is contiguous memory */
    mwchan_offset  += (vchan->length * vchan->nframes);

    if (NULL==vchan->virt){
        dev_err(&IP2DEV(mwdev),"Un-allocated DMA memory!\n");
        return -ENOMEM;
    }

    sg_init_table(vchan->sg, vchan->nframes);
    for (i=0;i < vchan->nframes;i++){
        sg_buff = &(vchan->virt[(vchan->length)*i]);
        sg_set_buf(&vchan->sg[i], sg_buff, vchan->length);
    }
    ret = dma_map_sg(chan_dev->dev, vchan->sg, vchan->nframes, vchan->direction);
    if (ret == 0) {
        dev_err(&IP2DEV(mwdev), "Unable to do dma_map_sg\n");
        return -ENOMEM;
    } else {
        dev_info(&IP2DEV(mwdev), "DMA MAP success.\n");
    }
    spin_lock_init(&vchan->slock);

    spin_lock_bh(&vchan->slock);
    vchan->status = MAPPED;
    spin_unlock_bh(&vchan->slock);
    vchan->xt.dir        = vchan->direction;  
    if (vchan->xt.dir == DMA_DEV_TO_MEM) {
        vchan->xt.src_sgl       = false;
        vchan->xt.dst_sgl       = true;
        vchan->xt.dst_start     = vchan->sg[0].dma_address;
        vchan->xt.sgl[0].size   = hsize * bpp;// usrbuf->hsize; // TODO
        vchan->xt.sgl[0].icg    = 0;
        vchan->xt.frame_size    = 1;
        vchan->xt.numf          = vsize;// usrbuf->vsize; // TODO
    } else {
        vchan->xt.src_sgl       = true;
        vchan->xt.dst_sgl       = false;
        vchan->xt.src_start     = vchan->sg[0].dma_address;
        vchan->xt.sgl[0].size   = hsize * bpp;// usrbuf->hsize; // TODO
        vchan->xt.sgl[0].icg    = 0;
        vchan->xt.frame_size    = 1;
        vchan->xt.numf          = vsize;// usrbuf->vsize; // TODO
    }
    spin_lock_bh(&vchan->slock);
    vchan->status = READY;
    spin_unlock_bh(&vchan->slock);
    init_completion(&vchan->dma_complete);
    print_channel_info(vchan);
    return 0;
}

static void print_channel_info(struct vdma_chan * vchan) {
    dev_dbg(&from_vchan_to_dev(vchan), "#---- Printing channel information ####\n");
    dev_dbg(&from_vchan_to_dev(vchan), "vchan->chan- name   - %s\n",dma_chan_name(vchan->chan));
    dev_dbg(&from_vchan_to_dev(vchan), "vchan->chan- addr   - %p\n",vchan->chan);
    dev_dbg(&from_vchan_to_dev(vchan), "vchan->chan- dir    - %d\n",vchan->direction);
    dev_dbg(&from_vchan_to_dev(vchan), "vchan->chan- addr   - %p\n",(void*)vchan->sg[0].dma_address);
    dev_dbg(&from_vchan_to_dev(vchan), "vchan->chan- virt   - %p\n",vchan->virt);
}

static void vdma_complete(struct vdma_chan *vchan) {
    dev_info(&from_vchan_to_dev(vchan), "VDMA complete - update pointer.\n");
    if (1 == testmode) {
        dev_info(&from_vchan_to_dev(vchan), "Starting next frame...");
        vdma_start_frame_transfer(vchan);
    }
}

static int vdma_start_frame_transfer(struct vdma_chan *vchan) {
    static int i = 0;
    static int j = 0;
    int *ctr;
    enum VDMA_CHAN_STATUS chan_status;
    unsigned long tx_tmo = msecs_to_jiffies(10000);
    int status;
    struct xilinx_vdma_config cfg;

    if (vchan->xt.dir == DMA_DEV_TO_MEM) {
        ctr = &i;
    } else {
        ctr = &j;
    }

    spin_lock_bh(&vchan->slock);
    chan_status = vchan->status;
    spin_unlock_bh(&vchan->slock);

    memset(&cfg, 0, sizeof(struct xilinx_vdma_config));
    if (chan_status == STOP_REQUESTED){
        dev_info(&from_vchan_to_dev(vchan), "Stop requested for %s - return\n",dma_chan_name(vchan->chan));
        return 0;
    }

    if (vchan->xt.dir == DMA_DEV_TO_MEM) {
        vchan->xt.dst_start     = vchan->sg[*ctr].dma_address;
    } else {
        vchan->xt.src_start     = vchan->sg[*ctr].dma_address;
    }

    dev_info(&from_vchan_to_dev(vchan), "phys-addr = 0x%p\n", (void*)vchan->sg[*ctr].dma_address);
    vchan->desc = dmaengine_prep_interleaved_dma(vchan->chan, &vchan->xt, vchan->flags);
    if (!vchan->desc) {
        dev_err(&from_vchan_to_dev(vchan), "Failed to prepare DMA transfer\n");
        return -ENODEV;
    } else {
        dev_info(&from_vchan_to_dev(vchan),  "DMA prep-interleaved done.\n");
    }
    vchan->desc->callback       = (dma_async_tx_callback)vdma_complete;
    vchan->desc->callback_param = vchan;
    vchan->desc->cookie         = dmaengine_submit(vchan->desc);
    if (dma_submit_error(vchan->desc->cookie)){
        dev_err(&from_vchan_to_dev(vchan), "Failed to submit cookie \n");
        return -ENOMEM;
    } else {
        dev_info(&from_vchan_to_dev(vchan), "DMA engine submit done.\n");
    }
    dma_async_issue_pending(vchan->chan);
    spin_lock_bh(&vchan->slock);
    vchan->status = RUNNING;
    spin_unlock_bh(&vchan->slock);
    *ctr = (*ctr+1) % vchan->nframes;
    tx_tmo = wait_for_completion_timeout(&vchan->dma_complete, tx_tmo);
    status = dma_async_is_tx_complete(vchan->chan, vchan->desc->cookie, NULL, NULL);
    if (tx_tmo == 0){
        dev_info(&from_vchan_to_dev(vchan), "Transfer timedout - reset channel.\n");
        cfg.reset   = 1;
        xilinx_vdma_channel_set_config(vchan->chan, &cfg);
    } else if (status != DMA_COMPLETE){
        dev_info(&from_vchan_to_dev(vchan), "Status is \'%s\'\n", status == DMA_ERROR? "error" : "in progress");
    }
    return 0;
}

static int vdma_stop_frame_transfer(struct vdma_chan *vchan){
    dmaengine_terminate_all(vchan->chan);
    return 0;
};

static int vdma_cleanup_desc(struct vdma_chan *vchan){
    dma_unmap_sg(&from_vchan_to_dev(vchan), vchan->sg, vchan->nframes, vchan->direction);
    return 0;
}

int thread_function(void *data) {
    struct vdma_dev *mwdev = data;
    struct mwvdma_chan_params usrbuf;
    int ret = 0;
    size_t total_buffer = hsize*vsize*bpp*MAX_FRAMES*2;
    int status;
    /*
     * Open file
     * MMAP external memory 
     * Prepare descriptors for Rx
     * Prepare descriptors for Tx
     * Start Rx <RX------------------------------|
     * Start Tx <TX------------------------------|
     * ........                                  |
     * ........                                  | 
     * ........                                  | 
     * in_callback_start_next_transfer(rx)----RX>|
     * ........                                  | 
     * ........                                  | 
     * ........                                  | 
     * in_callback_start_next_transfer(tx)----TX>|
     * Stop Rx
     * Stop Tx
     * Destroy descriptors for Rx
     * Destroy descriptors for Tx
     */

    dev_info(&IP2DEV(mwdev),"In thread-function mapping 0x%x size\n", total_buffer);
    status = vdma_alloc_memory(mwdev, total_buffer);
    if ((status) && (status != -EEXIST))  {
        return -ENOMEM;
    }
    ret = vdma_prep_desc(mwdev, RX, &usrbuf);
    if(ret) {
        dev_err(&IP2DEV(mwdev),"Error while setting up RX channel\n");
    } else {
        dev_info(&IP2DEV(mwdev),"Done setting up RX channel\n");
    }
    ret = vdma_prep_desc(mwdev, TX, &usrbuf);
    if(ret) {
        dev_err(&IP2DEV(mwdev), "Error while setting up TX channel\n");
    } else {
        dev_info(&IP2DEV(mwdev),"Done setting up TX channel\n");
    }
    dev_info(&IP2DEV(mwdev),"Start Tx transfer ...\n");
    vdma_start_frame_transfer(&mwchan[TX]);
    dev_info(&IP2DEV(mwdev),"Start Rx transfer ...\n");
    vdma_start_frame_transfer(&mwchan[RX]);
    while (!kthread_should_stop()){
        set_current_state(TASK_INTERRUPTIBLE);
        dev_info(&IP2DEV(mwdev),"set-schedule...\n");
        schedule();
    }
    /*
     * Cleanup RX
     */
    vdma_stop_frame_transfer(&mwchan[RX]);
    vdma_cleanup_desc(&mwchan[RX]);

    /*
     * Cleanup RX
     */
    vdma_stop_frame_transfer(&mwchan[TX]);
    vdma_cleanup_desc(&mwchan[TX]);

    return 0;
}


static const struct of_device_id mw_vdma_client_of_ids[] = {
    { .compatible = "mathworks,mw-vdma,1.00.a",},
    {},
}; 

static struct platform_driver mw_vdmaclient_driver = {
    .driver = {
        .name = "mw-axi-vdma-client",
        .owner = THIS_MODULE,
        .of_match_table = mw_vdma_client_of_ids,
    },
    .probe  = vdma_of_probe,
    .remove = vdma_of_remove,
};

module_platform_driver(mw_vdmaclient_driver);


MODULE_DEVICE_TABLE(of, mw_vdma_client_of_ids);
MODULE_AUTHOR("MathWorks, Inc.");
MODULE_DESCRIPTION("MathWorks AXI VDMA client");
MODULE_LICENSE("GPL v2");


