/**********************************************************************
 *
 * Copyright(c) 2008 Imagination Technologies Ltd. All rights reserved.
 * 
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 * 
 * This program is distributed in the hope it will be useful but, except 
 * as otherwise stated in writing, without any warranty; without even the 
 * implied warranty of merchantability or fitness for a particular purpose. 
 * See the GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 * 
 * The full GNU General Public License is included in this distribution in
 * the file called "COPYING".
 *
 * Contact Information:
 * Imagination Technologies Ltd. <gpl-support@imgtec.com>
 * Home Park Estate, Kings Langley, Herts, WD4 8LZ, UK 
 *
 ******************************************************************************/

#ifndef AUTOCONF_INCLUDED
#include <linux/config.h>
#if defined(CONFIG_MODVERSIONS) && !defined(MODVERSIONS)
   #include <linux/modversions.h>
   #define MODVERSIONS
#endif
#endif


#include <linux/init.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/fs.h>

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,29))
#include <linux/uaccess.h>
#else
#include <asm/uaccess.h>
#endif

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,15))
#include <linux/platform_device.h>
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,15))
typedef u32 pm_message_t;
#endif

#include "sysconfig.h"
#include "mbxaccessuser.h"
#include "services.h"
#include "mbxaccess_mbxsysdata.h"
#include "mbxaccess_mbxmem.h"
#include "pvr_debug.h"
#include "pvrmmap_private.h"
#include "mmap.h"
#include "mutex.h"

#include "mbxaccess_procwrapper.h"
#include "mbxaccess_iowrapper.h"
#include "mbxaccess_memwrapper.h"
#include "mbxaccess_virtmemwrapper.h"
#include "mbxaccess_timerwrapper.h"
#include "mbxaccess_mmapwrapper.h"
#include "mbxaccessinternals.h"
#include "mbxaccessapitype.h"
#include "mbxaccess.h"
#include "mbxworker_thread_struct.h"
#include "mbxaccess_virtmemwrapper_private.h"

#include "malloc_debug.h"

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,16))
#include <linux/clk.h>
#endif
#include <linux/delay.h>
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,15) && LINUX_VERSION_CODE < KERNEL_VERSION(2,6,27) && !defined(ARMVP))
#include <asm/arch/clock.h>
#endif


#define MBX_SERVICEAPI_NOTINITIALIZED	0

#include <linux/pm.h>

MODULE_AUTHOR("Imagination Technologies Ltd. <gpl-support@imgtec.com>");
MODULE_LICENSE("GPL");

PVRSRV_LINUX_MUTEX gPVRSRVLock;

module_init(MBXAccess_Init);
module_exit(MBXAccess_Exit);

#if (LINUX_VERSION_CODE < KERNEL_VERSION(5,6,0))
struct timeval tv_before;
struct timeval tv_after;
#endif

static int   driverInitialised = 0;

int MBXMajor = 0;
int MBXMinor = 0;
int NumMBXs  = 1;

MBXDriverParams globalDriverParams;

IMG_CPU_VIRTADDR gpvLinBase = 0;
MBX_WORKER_THREAD_STRUCT *gpsInterruptStructKernel;

PVRSRV_LINUX_MUTEX MBX_Virtmem_lMallocHeadLock;

int gi32MBXInitialized = MBX_SERVICEAPI_NOTINITIALIZED;

/* The file that initialised the services (mbxdaemon's) */
static struct file *gpsServicesFile;

extern int mbxaccess_trace;

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6,0,0))
/*
 * dma-bufs (omapdrm scanout buffers) the MBX renders into: their physical
 * address for MBXMapSysPhysToDevVAddr, the attachment kept (the buffer
 * pinned) until the file that asked is closed.
 */
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include "mbxaccess_dmabuf.h"

MODULE_IMPORT_NS("DMA_BUF");

#define MBX_MAX_DMABUFS 16
static struct {
	struct file *owner;
	struct dma_buf *buf;
	struct dma_buf_attachment *att;
	struct sg_table *sgt;
} gsDmaBufs[MBX_MAX_DMABUFS];
static struct platform_device *gpsDmaBufDev;

static int dmabuf_phys(struct file *filp, unsigned long arg, int *slotp);

static int MBXAccess_DmaBufPhys(struct file *filp, unsigned long arg)
{
	return dmabuf_phys(filp, arg, NULL);
}

/* MBXAccess_DmaBufPhys(), and the slot holding the attachment */
static int dmabuf_phys(struct file *filp, unsigned long arg, int *slotp)
{
	struct mbxaccess_dmabuf req;
	struct dma_buf *buf;
	struct dma_buf_attachment *att;
	struct sg_table *sgt;
	struct scatterlist *sg;
	dma_addr_t next;
	int i, slot;

	if (!gpsDmaBufDev)
		return -ENODEV;
	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;
	for (slot = 0; slot < MBX_MAX_DMABUFS && gsDmaBufs[slot].buf; slot++)
		;
	if (slot == MBX_MAX_DMABUFS)
		return -ENOSPC;
	buf = dma_buf_get(req.fd);
	if (IS_ERR(buf))
		return PTR_ERR(buf);
	att = dma_buf_attach(buf, &gpsDmaBufDev->dev);
	if (IS_ERR(att)) {
		dma_buf_put(buf);
		return PTR_ERR(att);
	}
	sgt = dma_buf_map_attachment_unlocked(att, DMA_BIDIRECTIONAL);
	if (IS_ERR(sgt)) {
		dma_buf_detach(buf, att);
		dma_buf_put(buf);
		return PTR_ERR(sgt);
	}
	req.size = buf->size;
	req.phys = sg_dma_address(sgt->sgl);
	req.contiguous = 1;
	next = req.phys;
	for_each_sgtable_dma_sg(sgt, sg, i) {
		if (sg_dma_address(sg) != next)
			req.contiguous = 0;
		next = sg_dma_address(sg) + sg_dma_len(sg);
	}
	gsDmaBufs[slot].owner = filp;
	gsDmaBufs[slot].buf = buf;
	gsDmaBufs[slot].att = att;
	gsDmaBufs[slot].sgt = sgt;
	if (slotp)
		*slotp = slot;
	if (mbxaccess_trace)
		printk(KERN_INFO "mbxaccess: [%d] dma-buf %u bytes at 0x%08x%s\n", current->pid,
		       req.size, req.phys, req.contiguous ? "" : " (not contiguous)");
	return copy_to_user((void __user *)arg, &req, sizeof(req)) ? -EFAULT : 0;
}

/* An armed dma-buf: the vmap that backs the next matching allocation */
static struct {
	pid_t tgid;
	IMG_UINT32 size;
	void *vaddr;
	struct dma_buf *buf;	/* for the area the allocation gets to hold */
	struct file *owner;
} gsArmed[MBX_MAX_DMABUFS];

/* A dma-buf's attachment slot given back (gsDmaBufs) */
static void dmabuf_slot_free(int i)
{
	dma_buf_unmap_attachment_unlocked(gsDmaBufs[i].att, gsDmaBufs[i].sgt, DMA_BIDIRECTIONAL);
	dma_buf_detach(gsDmaBufs[i].buf, gsDmaBufs[i].att);
	dma_buf_put(gsDmaBufs[i].buf);
	gsDmaBufs[i].buf = NULL;
}

/*
 * Arm: the next allocation of its size is backed by a vmap of the dma-buf's
 * pages, and holds the dma-buf. The attachment is only needed to find the
 * pages: it goes back at once, or a long-lived client (a compositor
 * importing buffer after buffer) runs out of the MBX_MAX_DMABUFS slots
 * (ENOSPC), which were freed only when it closed /dev/mbx.
 */
static int MBXAccess_DmaBufArm(struct file *filp, unsigned long arg)
{
	struct mbxaccess_dmabuf req;
	struct dma_buf *buf;
	struct page **pages;
	struct sg_page_iter it;
	unsigned int i, n;
	void *vaddr;
	int err, slot, att;

	err = dmabuf_phys(filp, arg, &att);
	if (err)
		return err;
	err = -EFAULT;
	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		goto out;
	err = -EINVAL;
	if (req.contiguous && (req.phys & ~PAGE_MASK))
		goto out;
	for (slot = 0; slot < MBX_MAX_DMABUFS && gsArmed[slot].vaddr; slot++)
		;
	err = -ENOSPC;
	if (slot == MBX_MAX_DMABUFS)
		goto out;
	n = PAGE_ALIGN(req.size) >> PAGE_SHIFT;
	err = -ENOMEM;
	pages = kvmalloc_array(n, sizeof(*pages), GFP_KERNEL);
	if (!pages)
		goto out;
	if (req.contiguous) {
		/* omapdrm's: one block, its scatterlist without pages */
		for (i = 0; i < n; i++)
			pages[i] = pfn_to_page((req.phys >> PAGE_SHIFT) + i);
	} else {
		/*
		 * scattered pages (the system dma-heap's: a Wayland client's
		 * buffer that is never scanned out): the services map the
		 * allocation page by page into the MBX's MMU all the same
		 */
		i = 0;
		for_each_sgtable_page(gsDmaBufs[att].sgt, &it, 0) {
			if (i == n)
				break;
			pages[i++] = sg_page_iter_page(&it);
		}
		err = -EINVAL;
		if (i < n) {
			kvfree(pages);
			goto out;
		}
	}
	/* no VM_MAP_PUT_PAGES: vfree() only unmaps, the exporter keeps the pages */
	vaddr = vmap(pages, n, VM_MAP, pgprot_writecombine(PAGE_KERNEL));
	kvfree(pages);
	err = -ENOMEM;
	if (!vaddr)
		goto out;
	/*
	 * The allocation it backs holds the dma-buf: the exporter frees the
	 * pages once nothing maps them any more (the services daemon keeps its
	 * own mapping until the MBX is done with them), not when the client
	 * exits
	 */
	buf = dma_buf_get(req.fd);
	if (IS_ERR(buf)) {
		vunmap(vaddr);
		err = PTR_ERR(buf);
		goto out;
	}
	gsArmed[slot].buf = buf;
	gsArmed[slot].owner = filp;
	gsArmed[slot].tgid = current->tgid;
	gsArmed[slot].size = n << PAGE_SHIFT;
	gsArmed[slot].vaddr = vaddr;
	err = 0;
out:
	dmabuf_slot_free(att);
	return err;
}

/*
 * For VirtualAllocateReserve: an armed dma-buf of this size, or NULL; its
 * reference to the dma-buf goes to the caller (*ppvDmaBuf)
 */
void *MBXAccess_TakeArmedVMap(IMG_UINT32 ui32Bytes, void **ppvDmaBuf)
{
	void *vaddr;
	int i;

	for (i = 0; i < MBX_MAX_DMABUFS; i++)
		/*
		 * mbxdaemon reserves on the client's behalf: match on size (an arm
		 * is one-shot, and the client allocates right after arming)
		 */
		if (gsArmed[i].vaddr && gsArmed[i].size == PAGE_ALIGN(ui32Bytes)) {
			vaddr = gsArmed[i].vaddr;
			*ppvDmaBuf = gsArmed[i].buf;
			gsArmed[i].vaddr = NULL;
			gsArmed[i].buf = NULL;
			if (mbxaccess_trace)
				printk(KERN_INFO "mbxaccess: [%d] allocation of %u bytes backed by a dma-buf\n",
				       current->pid, ui32Bytes);
			return vaddr;
		}
	return NULL;
}

static void MBXAccess_DmaBufRelease(struct file *filp)
{
	int i;

	/* arms no allocation took: the client went first */
	for (i = 0; i < MBX_MAX_DMABUFS; i++)
		if (gsArmed[i].vaddr && (!filp || gsArmed[i].owner == filp)) {
			vunmap(gsArmed[i].vaddr);
			dma_buf_put(gsArmed[i].buf);
			gsArmed[i].vaddr = NULL;
			gsArmed[i].buf = NULL;
		}

	for (i = 0; i < MBX_MAX_DMABUFS; i++)
		if (gsDmaBufs[i].buf && (!filp || gsDmaBufs[i].owner == filp))
			dmabuf_slot_free(i);
}
#endif

extern int mbxaccess_trace;
static void MBXAccess_GfxPowerDown(void);

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,36))
static long MBXAccess_UnlockedIoctl(struct file *filp, unsigned int cmd, unsigned long arg);
#endif

static struct file_operations MBXAccess_fops =
{
	.owner   = THIS_MODULE,
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,36))
	.unlocked_ioctl = MBXAccess_UnlockedIoctl,
#else
	.ioctl   = MBXAccess_Ioctl,
#endif
	.open    = MBXAccess_Open,
	.release = MBXAccess_Release,
	.mmap    = MBXAccess_Mmap,
	.fasync  = MBXAccess_FAsync
};

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0))
static struct resource device_resources[] =
{
	{
		.start = SOC_REG_BASE,
		.end = SOC_REG_BASE + SOC_REG_RANGE - 1,
		.flags = IORESOURCE_MEM
	},
	{
		.start = MBX_REG_SYS_PHYS_BASE,
		.end = MBX_REG_SYS_PHYS_BASE + MBX_REG_RANGE - 1,
		.flags = IORESOURCE_MEM
	},
	{
		.start = MBX_SP_3D_SYS_PHYS_BASE,
		.end = MBX_SP_3D_SYS_PHYS_BASE + MBX_SP_3D_RANGE - 1,
		.flags = IORESOURCE_MEM
	},
	{
		.start = MBX_SP_2D_SYS_PHYS_BASE,
		.end = MBX_SP_2D_SYS_PHYS_BASE + MBX_SP_2D_RANGE - 1,
		.flags = IORESOURCE_MEM
	},
	{
		.start = MBX_SP_TA_CTRL_SYS_PHYS_BASE,
		.end = MBX_SP_TA_CTRL_SYS_PHYS_BASE + MBX_SP_TA_CTRL_RANGE - 1,
		.flags = IORESOURCE_MEM
	}
};
#else
struct device_class powervr_devclass = {
    .name       = "powervr",
};

static ssize_t MBXAccess_ShowDev(struct device *pDevice, char *buf, size_t count, loff_t off)
{
	ssize_t ret = off ? 0 : snprintf(&buf[off], count, "%d:0\n", MBXMajor);
	return ret;
}

static DEVICE_ATTR(dev, S_IRUGO, MBXAccess_ShowDev, NULL);

static int MBXAccess_DriverScale(struct bus_op_point *op, u32 level)
{
	return 0;
}
#endif 

static struct platform_device powervr_device =
{
	.name           = DEVNAME,
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0))
	.id             = -1,
	.num_resources  = (sizeof (device_resources) / sizeof (struct resource)),
	.resource       = device_resources,
#else 
	.id             = 0,
#endif 
	.dev            =
	{
	   .release    = MBXAccess_DeviceRelease
	}
};

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,15))
static struct platform_driver powervr_driver =
{
	.driver = {
		.name		= DEVNAME,
		.owner		= THIS_MODULE,
		.bus		= &platform_bus_type,
	},
	.probe      = MBXAccess_DriverProbe,
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6,11,0))
	.remove     = MBXAccess_DriverRemoveVoid,
#else
	.remove     = MBXAccess_DriverRemove,
#endif
	.suspend    = MBXAccess_DriverSuspend,
	.resume     = MBXAccess_DriverResume,
	.shutdown   = MBXAccess_DriverShutdown,
};
#else
static struct device_driver powervr_driver =
{
	.name       = DEVNAME,
	.bus        = &platform_bus_type,
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0))
	.devclass	= &powervr_devclass,
	.scale		= MBXAccess_DriverScale,
#endif
	.probe      = MBXAccess_DriverProbe,
	.remove     = MBXAccess_DriverRemove,
	.suspend    = MBXAccess_DriverSuspend,
	.resume     = MBXAccess_DriverResume,
	.shutdown   = MBXAccess_DriverShutdown,
};
#endif


unsigned long lastFAsyncTime = 0;

static int PerDeviceSysInitialise(IMG_PVOID pHostOSDevice);
static int PerDeviceSysDeInitialise(IMG_PVOID pHostOSDevice);

static int PerDeviceSysInitialise(IMG_PVOID pHostOSDevice)
{
#if defined(OMAP2430)
#if defined(DEBUG) || defined(TIMING)
	struct device * pDevice = (struct device *)pHostOSDevice;
	struct clk *pGpt12_fck;
	struct clk *pGpt12_ick;

	
	pGpt12_fck = clk_get(pDevice, "gpt12_fck");
	pGpt12_ick = clk_get(pDevice, "gpt12_ick");

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,10)
	clk_enable(pGpt12_fck);
	clk_enable(pGpt12_ick);
#else
	clk_use(pGpt12_fck);
	clk_use(pGpt12_ick);
#endif 
#else
	PVR_UNREFERENCED_PARAMETER(pHostOSDevice);
#endif 
#else 
	PVR_UNREFERENCED_PARAMETER(pHostOSDevice);
#endif 
	return 0;
}

static int PerDeviceSysDeInitialise(IMG_PVOID pHostOSDevice)
{
#if defined(OMAP2430)
#if defined(DEBUG) || defined(TIMING)
	struct device * pDevice = (struct device *)pHostOSDevice;
	struct clk *pGpt12_fck;
	struct clk *pGpt12_ick;

	
	pGpt12_fck = clk_get(pDevice, "gpt12_fck");
	pGpt12_ick = clk_get(pDevice, "gpt12_ick");

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,10)
	clk_disable(pGpt12_fck);
	clk_put(pGpt12_fck);
	
	clk_disable(pGpt12_ick);
	clk_put(pGpt12_ick);
#else
	clk_unuse(pGpt12_fck);
	clk_unuse(pGpt12_ick);
#endif 
#else
	PVR_UNREFERENCED_PARAMETER(pHostOSDevice);
#endif 
#else
	PVR_UNREFERENCED_PARAMETER(pHostOSDevice);
#endif 
	return 0;
}

struct MBXAccess device;

static int MBXAccess_Ioctl(struct inode *inode, struct file *filp,
				 unsigned int cmd, unsigned long arg)
{
	int err = 0;

	PVR_DPF((PVR_DBG_MESSAGE, "Requesting service cmd=%d", cmd));
	LinuxLockMutex(&gPVRSRVLock);
	
	switch (cmd)
	{
		case MBXCallSyncKernelService:
		{
			
			MBXCallSyncKernelServiceHandler(inode, filp, arg);
		}
		break;
		case MBXInitDrv:
		{
			if (mbxaccess_trace)
				printk(KERN_INFO "mbxaccess: [%d] MBXInitDrv\n", current->pid);
			err = MBXAccess_InitDrv();
			gpsServicesFile = filp;
		}
		break;
		case MBXDeInitDrv:
		{
			if (mbxaccess_trace)
				printk(KERN_INFO "mbxaccess: [%d] MBXDeInitDrv\n", current->pid);
			err = MBXAccess_DeInitDrv();
		}
		break;
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6,0,0))
		case MBXDmaBufPhys:
		{
			err = MBXAccess_DmaBufPhys(filp, arg);
		}
		break;
		case MBXDmaBufArm:
		{
			err = MBXAccess_DmaBufArm(filp, arg);
		}
		break;
#endif
		case MBXRecyleDriver:
		{
			if (mbxaccess_trace)
				printk(KERN_INFO "mbxaccess: [%d] MBXRecyleDriver\n", current->pid);
			if(driverInitialised)
			{
				MBXAccess_DeInitDrv();
			}
			PVRMMapCleanup();
		}
		break;
		case MBXFailed:
		{
			MBXAccess_InitFailed();
		}
		break;
		case MBXOpenComplete:
		{
			err = MBXAccess_OpenComplete();
		}
		break;
		case MBXCloseComplete:
		{
			err = MBXAccess_CloseComplete();
		}
		break;
		default:
			;
	};

	LinuxUnLockMutex(&gPVRSRVLock);
	return err;
}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,36))
static long MBXAccess_UnlockedIoctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	return MBXAccess_Ioctl(file_inode(filp), filp, cmd, arg);
}
#endif

static int MBXAccess_Open(struct inode *inode, struct file *filp)
{
	int err = 0;

	PVR_UNREFERENCED_PARAMETER(inode);
	PVR_UNREFERENCED_PARAMETER(filp);

	globalDriverParams.MBXDriverParam_PVRSRV_DEVICE_NODE_SIZE = 0;
	globalDriverParams.MBXDriverParam_SYS_DATA_SIZE = 0;
	globalDriverParams.debugLevel = 0;

	filp->private_data = &device;

	return err;
}

void MBXAccess_KillFAsync(void)
{
	lastFAsyncTime = jiffies;
	kill_fasync(&device.asyncqueue, SIGIO, POLL_IN);
}

void MBXAccess_KillFAsyncVSync(void)
{
	lastFAsyncTime = jiffies;
	kill_fasync(&device.asyncqueue, SIGIO, POLL_IN);
}

void MBXAccess_KillFAsyncLimited(unsigned long timebreak)
{
	if (jiffies - lastFAsyncTime > timebreak)
	{
		MBXAccess_KillFAsync();
	}
}

static int MBXAccess_FAsync(int fd, struct file *filp, int mode)
{
	struct MBXAccess* dev = filp->private_data;
	return fasync_helper(fd, filp, mode, &dev->asyncqueue);
}

/*
 * The services died or are going away: nothing in the kernel may keep using
 * their memory (the interrupt struct) or calling back into them.
 */
void MBXAccess_ServicesGone(void)
{
	extern void MBXAccess_RemoveAllTimers(void);
	extern void MBXAccess_FreeIrqOnExit(void);

	MBXAccess_RemoveAllTimers();
	MBXAccess_FreeIrqOnExit();
	gpsInterruptStructKernel = NULL;
}

static int MBXAccess_Release(struct inode *inode, struct file *filp)
{
	int err = 0;
	
	PVR_UNREFERENCED_PARAMETER(inode);
	
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6,0,0))
	LinuxLockMutex(&gPVRSRVLock);
	MBXAccess_DmaBufRelease(filp);
	LinuxUnLockMutex(&gPVRSRVLock);
#endif
	if (filp == gpsServicesFile)
	{
		extern void RemoveProcEntries(void);

		LinuxLockMutex(&gPVRSRVLock);
		MBXAccess_ServicesGone();
		if (driverInitialised)
			MBXAccess_DeInitDrv();
		MBXAccess_GfxPowerDown();
		gi32MBXInitialized = MBX_SERVICEAPI_NOTINITIALIZED;
		RemoveProcEntries();
		gpsServicesFile = NULL;
		LinuxUnLockMutex(&gPVRSRVLock);
	}
	if(filp->f_flags & FASYNC)
	{
		
		MBXAccess_FAsync(-1, filp, 0);
	}
	return err;
}

static int MBXAccess_Mmap(struct file* pFile, struct vm_area_struct* ps_vma)
{
	int err = 0;
	err = PVRMMap(pFile, ps_vma);
	return err;
}


static int MBXAccess_DriverProbe(struct STRUCT_DEVICE *pDevice)
{
	int ret = 0;

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0))
	device_create_file(pDevice, &dev_attr_dev);
#endif
	if (PerDeviceSysInitialise((IMG_PVOID)pDevice) != 0)
	{
		ret = -EINVAL;
		goto err_out;
	}
	else
	{
		ret = 0;
	}

	return ret;
err_out:
	return ret;
}

static int MBXAccess_DriverRemove(struct STRUCT_DEVICE *pDevice)
{
	int ret = 0;

	if (PerDeviceSysDeInitialise((IMG_PVOID)pDevice) != 0)
	{
		ret = -EINVAL;
	}
	else
	{
		ret = 0;
	}
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0))
	device_remove_file(pDevice, &dev_attr_dev);
#endif

	return ret;
}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6,11,0))
static void MBXAccess_DriverRemoveVoid(struct STRUCT_DEVICE *pDevice)
{
	MBXAccess_DriverRemove(pDevice);
}
#endif

static int MBXAccess_DriverSuspend(struct STRUCT_DEVICE *device, pm_message_t state
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,15))
								   , u32 level
#endif
								  )
{
	
	return 0;
}

static void MBXAccess_DriverShutdown(struct STRUCT_DEVICE *pDevice)
{
	PVR_UNREFERENCED_PARAMETER(pDevice);
}

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,15))
static int MBXAccess_DriverResume(struct STRUCT_DEVICE *device, u32 level)
#else
static int MBXAccess_DriverResume(struct STRUCT_DEVICE *device)
#endif
{
	
	return 0;
}

static void MBXAccess_DeviceRelease(struct device *pDevice)
{
	PVR_UNREFERENCED_PARAMETER(pDevice);
}

static void MBXAccess_InitFailed(void)
{
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,23))
	unregister_chrdev(MBXMajor, DRVNAME);
#else
	if(!unregister_chrdev(MBXMajor, DRVNAME))
#endif
	{
		MBXMajor = 0;
	}
}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,16))
#define SUCCESSFUL(x) if(x) { err = -ENOMEM; goto init_exit; }
#else
#define SUCCESSFUL(x)
#endif
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6,0,0))
/*
 * Nothing else powers the GFX block on current kernels (no hwmod for the
 * OMAP2420's): it is on while the services are initialised, brought up as
 * TI's EnableSystemClocks() (libmbxservicesdaemon) does. Order matters: an
 * MPU access to the MBX before its clocks run and its reset is pulsed
 * leaves the interconnect answering every later access with an external
 * abort, until the next boot.
 */
#include <linux/io.h>
#include <linux/iopoll.h>

#define OMAP24XX_PRCM_BASE	0x48008000
#define GFXP_PRCM_CLKCFG_CTRL	0x080	/* VALID_CONFIG: apply CM_CLKSEL changes */
#define GFXP_PRCM_CLKCFG_STATUS	0x084
#define GFXP_RM_RSTCTRL_GFX		0x350
#define GFXP_RM_RSTST_GFX		0x358
#define GFXP_RM_RSTST_GFX_RST	0x10
#define GFXP_PM_PWSTCTRL_GFX		0x3e0

static const char *const gfx_clk_names[] = { "gfx_ick", "gfx_2d_fck", "gfx_3d_fck" };
static struct clk *gfx_clks[ARRAY_SIZE(gfx_clk_names)];

static int gfxPowered;

static void MBXAccess_GfxClocks(int on)
{
	void __iomem *prcm = ioremap(OMAP24XX_PRCM_BASE, 0x1000);
	struct clk *l3;
	u32 v;
	int i;

	if (!prcm)
		return;
	if (!on)
		writel(1, prcm + GFXP_RM_RSTCTRL_GFX);
	for (i = 0; i < ARRAY_SIZE(gfx_clk_names); i++) {
		if (on) {
			gfx_clks[i] = clk_get(NULL, gfx_clk_names[i]);
			if (IS_ERR(gfx_clks[i]) || clk_prepare_enable(gfx_clks[i])) {
				printk(KERN_ERR "mbxaccess: no clock %s\n", gfx_clk_names[i]);
				if (!IS_ERR(gfx_clks[i]))
					clk_put(gfx_clks[i]);
				gfx_clks[i] = NULL;
			}
		} else if (gfx_clks[i]) {
			clk_disable_unprepare(gfx_clks[i]);
			clk_put(gfx_clks[i]);
			gfx_clks[i] = NULL;
		}
	}
	if (on) {
		/* 2D and 3D cores at L3 / 2, as TI's and Nokia's settings */
		l3 = clk_get(NULL, "core_l3_ck");
		if (!IS_ERR(l3)) {
			for (i = 1; i < ARRAY_SIZE(gfx_clk_names); i++)
				if (gfx_clks[i])
					clk_set_rate(gfx_clks[i], clk_get_rate(l3) / 2);
			clk_put(l3);
		}
		if (!(readl(prcm + GFXP_PRCM_CLKCFG_STATUS) & 1))
			writel(1, prcm + GFXP_PRCM_CLKCFG_CTRL);
		/* pm24xx forces the GFX domain off; power on, pulse the reset */
		writel(0, prcm + GFXP_PM_PWSTCTRL_GFX);
		writel(GFXP_RM_RSTST_GFX_RST, prcm + GFXP_RM_RSTST_GFX);
		writel(1, prcm + GFXP_RM_RSTCTRL_GFX);
		writel(0, prcm + GFXP_RM_RSTCTRL_GFX);
		if (readl_poll_timeout(prcm + GFXP_RM_RSTST_GFX, v, v & GFXP_RM_RSTST_GFX_RST, 10, 10000))
			printk(KERN_ERR "mbxaccess: GFX reset did not complete\n");
		writel(GFXP_RM_RSTST_GFX_RST, prcm + GFXP_RM_RSTST_GFX);
	}
	iounmap(prcm);
}
#endif

static void MBXAccess_GfxPowerDown(void)
{
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6,0,0))
	if (gfxPowered) {
		MBXAccess_GfxClocks(0);
		gfxPowered = 0;
	}
#endif
}

static int MBXAccess_InitDrv(void)
{
	int err = 0;
	IMG_CPU_PHYADDR sRegsCPUPhysBase = {MBX_REG_SYS_PHYS_BASE};

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6,0,0))
	/*
	 * Clients call MBXInitDrv/MBXDeInitDrv too: power up once, and down only
	 * when the services go away (MBXAccess_GfxPowerDown())
	 */
	if (!gfxPowered) {
		MBXAccess_GfxClocks(1);
		gfxPowered = 1;
	}
#endif
	
	gpvLinBase = HostMapPhysToLin (sRegsCPUPhysBase, MBX_REG_RANGE, CACHETYPE_UNCACHED | EXTRA_CACHETYPE_SHARED);

	driverInitialised = 1;
	return err;
}

static int MBXAccess_DeInitDrv(void)
{
	int err = 0;

	driverInitialised = 0;

	if(gpvLinBase)
	{
		
	}
	return err;
}

static int __init MBXAccess_Init(void)
{
	int err = PVRSRV_OK;
	int i = 0;

	PVRMMapInit();

#if defined(DEBUG) && defined(FULL_DEBUG)
	PVRDebugSetLevel(DBGPRIV_ALLLEVELS);
#endif

	PVR_DPF((PVR_DBG_MESSAGE,"Module load starts"));

	MALLOC_DEBUG_INIT();
	LinuxInitMutex(&gPVRSRVLock);
	LinuxInitMutex(&MBX_Virtmem_lMallocHeadLock);

	gpsMBXDeviceList = (MBX_DEVICELIST*)KMALLOC(sizeof(MBX_DEVICELIST), GFP_KERNEL);
	gpsMBXDeviceList->psDeviceNodeList = IMG_NULL;
	for(i = 0; i < SYS_DEVICE_COUNT; i++)
	{
		gpsMBXDeviceList->sDeviceID[i].uiID = i;
		gpsMBXDeviceList->sDeviceID[i].bInUse = IMG_FALSE;
	}
	gpsMBXDeviceList->ui32NumDevices = SYS_DEVICE_COUNT;
	/*
	 * Device index 0 is the display class's: rc.pvr loads omaplcd before
	 * mbxdaemon, and libsrv_um's PVRSRVQueueRender() queues 3D work for
	 * device index 1. Without omaplcd the MBX would get 0 and the first
	 * render goes to an index with no command processors (mbxdaemon
	 * crashes in PVRSRVProcessQueues).
	 */
	gpsMBXDeviceList->sDeviceID[0].bInUse = IMG_TRUE;

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6,0,0))
	gpsDmaBufDev = platform_device_register_simple("mbxaccess-dmabuf", -1, NULL, 0);
	if (IS_ERR(gpsDmaBufDev))
		gpsDmaBufDev = NULL;
	else
		dma_coerce_mask_and_coherent(&gpsDmaBufDev->dev, DMA_BIT_MASK(32));
#endif
	MBXMajor = register_chrdev(0, DRVNAME, &MBXAccess_fops);
	if (MBXMajor <= 0)
	{
		PVR_DPF((PVR_DBG_ERROR,"MBX dev number allocation failure - %d", MBXMajor));
		err = -EBUSY;
	}
	else
	{
		PVR_DPF((PVR_DBG_MESSAGE,"MBX device number: %d", MBXMajor));
		err = 0;
	}
	PVR_DPF((PVR_DBG_MESSAGE,"Module load ends"));

	
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0))
	err = devclass_register(&powervr_devclass);
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,15)
	SUCCESSFUL(platform_driver_register(&powervr_driver));
#else 
	driver_register(&powervr_driver);
#endif 

#if defined(TIMING)
	if ((err = platform_device_register(&powervr_device)) != 0)
	{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,15)
		platform_driver_unregister(&powervr_driver);
#else
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0))
		devclass_unregister(&powervr_devclass);
#endif
		driver_unregister(&powervr_driver);
#endif
		PVR_DPF((PVR_DBG_ERROR, "Unable to create platform device err = %u", err));
		return err;
	}
#endif
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,16))
init_exit:
#endif
	return err;
}

static void __exit MBXAccess_Exit(void)
{
	if(driverInitialised)
		MBXAccess_DeInitDrv();
	MBXAccess_GfxPowerDown();
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6,0,0))
	MBXAccess_DmaBufRelease(NULL);
	if (gpsDmaBufDev)
		platform_device_unregister(gpsDmaBufDev);
#endif

	/* The services may have died without removing them */
	{
		extern void RemoveProcEntries(void);
		extern void MBXAccess_FreeIrqOnExit(void);

		RemoveProcEntries();
		MBXAccess_ServicesGone();
	}

#if defined(TIMING)
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,21))
	platform_device_unregister(&powervr_device);
#endif
#endif 

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,15))	
	platform_driver_unregister(&powervr_driver);
#else
	driver_unregister(&powervr_driver);
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0))
	devclass_unregister(&powervr_devclass);
#endif
	
	PVRMMapCleanup();

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,23))
	unregister_chrdev(MBXMajor, DRVNAME);
#else
	if (unregister_chrdev(MBXMajor, DRVNAME))
	{
		PVR_DPF((PVR_DBG_FATAL,"Unable to unregister device %d", MBXMajor));
	}
#endif
	KFREE(gpsMBXDeviceList);
	MALLOC_DEBUG_EXIT();
}


void MBXSyncSrv_GetServiceAPIInitStatus(IMG_VOID *pvData)
{
	int status = 0;
	
	status = gi32MBXInitialized;
	
	COPY_TO_USER(pvData,(void*)&status, sizeof(int));
}

void MBXSyncSrv_SetServiceAPIInitStatus(IMG_VOID *pvData)
{
	int infoFromUser = 0;

	COPY_FROM_USER((void*)&infoFromUser, pvData, sizeof(int));
	gi32MBXInitialized = infoFromUser;
}

SyncService syncServiceTable[MBXKERNELSYNCSERVICE_END] =
{
	
	 SYNC_SERVICE_ENTRY(MBXSyncSrv_RequestIrqHandler),

	 SYNC_SERVICE_ENTRY(MBXSyncSrv_FreeIrqHandler),
	 SYNC_SERVICE_ENTRY(MBXSyncSrv_HostMapPhysToLinHandler),
	 SYNC_SERVICE_ENTRY(MBXSyncSrv_PCIFindSlotHandler),
	 SYNC_SERVICE_ENTRY(MBXSyncSrv_HostPCIReadDwordHandler),

	
	 SYNC_SERVICE_ENTRY(MBXSyncSrv_PageToPhysHandler),

	
	 SYNC_SERVICE_ENTRY(MBXSyncSrv_VirtualAllocateReserveHandler),
	 SYNC_SERVICE_ENTRY(MBXSyncSrv_VirtualDeallocateUnreserveHandler),
	 SYNC_SERVICE_ENTRY(MBXSyncSrv_ConvertKVToPageHandler),

	
	 SYNC_SERVICE_ENTRY(MBXSyncSrv_HostFlushCpuCacheAreaHandler),
	 SYNC_SERVICE_ENTRY(MBXSyncSrv_PVRSRVFlushCpuCacheKMHandler),

	
	 SYNC_SERVICE_ENTRY(MBXSyncSrv_CreateProcEntriesHandler),
	 SYNC_SERVICE_ENTRY(MBXSyncSrv_RemoveProcEntriesHandler),

	
	 SYNC_SERVICE_ENTRY(MBXSyncSrv_HostAddTimerHandler),
	 SYNC_SERVICE_ENTRY(MBXSyncSrv_HostRemoveTimerHandler),

	
	 SYNC_SERVICE_ENTRY(MBXSyncSrv_MMapGetFullMapDataHandler),
	 SYNC_SERVICE_ENTRY(MBXSyncSrv_HostReadHWRegHandler),
	 SYNC_SERVICE_ENTRY(MBXSyncSrv_HostWriteHWRegHandler),

	
	 SYNC_SERVICE_ENTRY(MBXSyncSrv_LoadDriverParams),

	
	 SYNC_SERVICE_ENTRY(MBXSyncSrv_DeviceNodeCreate),
	 SYNC_SERVICE_ENTRY(MBXSyncSrv_DeviceNodeDestroy),
	 SYNC_SERVICE_ENTRY(MBXSyncSrv_GetDeviceNodeValue),
	 SYNC_SERVICE_ENTRY(MBXSyncSrv_SetDeviceNodeValue),

	 SYNC_SERVICE_ENTRY(MBXSyncSrv_GetServiceAPIInitStatus),
	 SYNC_SERVICE_ENTRY(MBXSyncSrv_SetServiceAPIInitStatus),
#if defined(DEBUG)
	 SYNC_SERVICE_ENTRY(MBXSyncSrv_PrintLineIdentifier)
#endif
};

int mbxaccess_trace;
module_param_named(trace, mbxaccess_trace, int, 0644);
MODULE_PARM_DESC(trace, "Log each sync kernel service call");

static const char *const syncServiceNames[MBXKERNELSYNCSERVICE_END] = {
	"RequestIrq", "FreeIrq", "HostMapPhysToLin", "PCIFindSlot",
	"HostPCIReadDword", "PageToPhys", "VirtualAllocateReserve",
	"VirtualDeallocateUnreserve", "ConvertKVToPage", "HostFlushCpuCacheArea",
	"PVRSRVFlushCpuCacheKM", "CreateProcEntries", "RemoveProcEntries",
	"HostAddTimer", "HostRemoveTimer", "MMapGetFullMapData", "HostReadHWReg",
	"HostWriteHWReg", "LoadDriverParams", "DeviceNodeCreate",
	"DeviceNodeDestroy", "GetDeviceNodeValue", "SetDeviceNodeValue",
	"GetServiceAPIInitStatus", "SetServiceAPIInitStatus",
};

static void MBXCallSyncKernelServiceHandler(struct inode *inode, struct file *filp, unsigned long arg)
{
	MBXInfoForKernelSyncService infoFromUser;
	COPY_FROM_USER((void*)&infoFromUser, (void*)(arg), sizeof(MBXInfoForKernelSyncService));
	if ((unsigned int)infoFromUser.id >= MBXKERNELSYNCSERVICE_END)
		return;
	if (mbxaccess_trace)
		printk(KERN_INFO "mbxaccess: [%d] %s\n", current->pid,
		       syncServiceNames[infoFromUser.id] ? syncServiceNames[infoFromUser.id] : "?");
#if defined(DEBUG)	
	PVR_DPF((PVR_DBG_VERBOSE, "Requesting %s", syncServiceTable[infoFromUser.id].psServiceName ));
	syncServiceTable[infoFromUser.id].pfnHandler((IMG_VOID*)infoFromUser.data);
#else
	syncServiceTable[infoFromUser.id]((IMG_VOID*)infoFromUser.data);
#endif
}

static int MBXAccess_OpenComplete(void)
{
	return 0;
}

static int MBXAccess_CloseComplete(void)
{
	return 0;
}

static void MBXSyncSrv_LoadDriverParams(IMG_VOID *pvData)
{
	COPY_FROM_USER((void*)&globalDriverParams, pvData, sizeof(MBXDriverParams));
}
