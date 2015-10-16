/* Copyright 2015, The MathWorks Inc. */

/*
 * MathWorks client driver for Xilinx AXI VDMA engine
 */

/*
 * Ref. drivers/staging/video/xvdma.h, 
 *      drivers/staging/video/xvdma.c
 */

#ifndef _MW_VDMA_H_
#define _MW_VDMA_H_

#define DRIVER_NAME "mw-vdma"

#define MAX_DEVICES    4
#define MAX_FRAMES     5
#define DMA_CHAN_RESET 10


#ifdef _DEBUG
#define MW_DBG_text(txt) printk(KERN_INFO DRIVER_NAME txt)
#define MW_DBG_printf(txt,...) printk(KERN_INFO DRIVER_NAME txt,__VA_ARGS__)
#else
#define MW_DBG_printf(txt,...)
#define MW_DBG_text(txt)
#endif

#define	IP2DEV(x)	(x->pdev->dev)
#define from_vchan_to_dev(vx) (vx->chan->dev->device)

struct mwvdma_chan_params {
   unsigned int hsize; 
   unsigned int vsize; 
   unsigned int nframes; 
   unsigned int bpp; 
}__aligned(64)__;

enum VDMA_FRAME_STATUS {
    FULL = 0,
    PARTIAL
};

enum VDMA_CHAN {
    TX = 0,
    RX
};
enum VDMA_CHAN_STATUS {
    MAPPED = 0,
    READY,
    RUNNING,
    STOP_REQUESTED,
    UNMAPPED
};

#define MWVDMA_IOCTL_BASE 'U'
#define MWVDMA_GET_DEV_CFG       _IO(MWVDMA_IOCTL_BASE,1)
#define MWVDMA_SET_DEV_CFG       _IO(MWVDMA_IOCTL_BASE,2)
#define MWVDMA_PREP_CHANNEL      _IO(MWVDMA_IOCTL_BASE,3)
#define MWVDMA_START_TRANSFER    _IO(MWVDMA_IOCTL_BASE,4)
#define MWVDMA_STOP_TRANSFER     _IO(MWVDMA_IOCTL_BASE,5)
#define MWVDMA_FREE_CHANNEL      _IO(MWVDMA_IOCTL_BASE,6)

struct vdma_dev {
    const char               *name;
    struct platform_device   *pdev;
    struct device            *dev;
    struct resource          *mem;
    struct cdev              cdev;
    void __iomem             *regs;
    dev_t                    dev_id;
    char                     has_mem;
    dma_addr_t               phys;
    char                     *virt;
    size_t                   size;
    struct mutex             lock;
};

struct vdma_chan {
    struct list_head               list;
    dma_addr_t                     phys;
    char                           *virt;
    struct dma_chan                *chan;
    enum dma_ctrl_flags            flags;
    enum dma_transfer_direction    direction;
    unsigned long                  frame_count;
    unsigned long                  frame_queued;
    unsigned char                  nframes;
    unsigned long                  length;
    struct completion              dma_complete;
    spinlock_t                     slock;
    dma_cookie_t                   cookie;
    struct dma_async_tx_descriptor *desc;
    struct dma_interleaved_template xt;
    struct scatterlist              sg[32];
    enum VDMA_CHAN_STATUS           status;
};


#endif
