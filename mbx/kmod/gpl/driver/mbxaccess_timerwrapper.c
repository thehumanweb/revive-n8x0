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

#include <asm/uaccess.h>

#if defined(__arm__)
#include <linux/fs.h>
#endif

#include "pvrsrverror.h"
#include "pvr_debug.h"

#include "mbxaccess_timerwrappertype.h"
#include "mbxaccess_timerwrapperinternalstype.h"
#include "mbxworker_thread_struct.h"
#include "malloc_debug.h"
#include "mbxaccess_virtmemwrapper_private.h"

extern MBX_WORKER_THREAD_STRUCT *gpsInterruptStructKernel;
extern void MBXAccess_KillFAsyncLimited(unsigned long timebreak);


static IMG_HANDLE HostAddTimerKernel(PFN_TIMER_FUNC pfnTimerFunc, IMG_VOID *pvData, IMG_UINT32 ui32MsTimeout);
static PVRSRV_ERROR HostRemoveTimerKernel(IMG_HANDLE hTimer);

/* Active timers, so that they can be stopped when the services go away */
#define MBX_MAX_TIMERS 8
static TIMER_CALLBACK_DATA *gpsTimers[MBX_MAX_TIMERS];

void MBXAccess_RemoveAllTimers(void)
{
	int i;

	for (i = 0; i < MBX_MAX_TIMERS; i++)
		if (gpsTimers[i]) {
			HostRemoveTimerKernel(gpsTimers[i]);
			gpsTimers[i] = NULL;
		}
}

void MBXRecoverHWInKS(IMG_VOID *data)
{
	if (!gpsInterruptStructKernel)
		return;
	
	gpsInterruptStructKernel->pvHWRecoveryData = data;
	gpsInterruptStructKernel->bHWRecovery = IMG_TRUE;
	gpsInterruptStructKernel->ui32HWRecoveryCounter = 1;

	
	MBXAccess_KillFAsyncLimited(3*HZ);
}

void* MBXTimerCBFuncs[MBXIDTIMERCB_END]={MBXRecoverHWInKS};

void MBXSyncSrv_HostRemoveTimerHandler(IMG_VOID *pvData)
{
	MBXUserDataForHostRemoveTimer infoFromUser;
	COPY_FROM_USER((void*)&infoFromUser, pvData, sizeof(MBXUserDataForHostRemoveTimer));
	HostRemoveTimerKernel(infoFromUser.handle);
}

void MBXSyncSrv_HostAddTimerHandler(IMG_VOID *pvData)
{
	MBXUserDataForHostAddTimer infoFromUser;
	COPY_FROM_USER((void*)&infoFromUser, pvData, sizeof(MBXUserDataForHostAddTimer));
	if ((unsigned int)infoFromUser.id >= MBXIDTIMERCB_END)
		return;

	
	infoFromUser.handle = HostAddTimerKernel(MBXTimerCBFuncs[infoFromUser.id], infoFromUser.data, infoFromUser.timeout);

	COPY_TO_USER(pvData,(void*)&infoFromUser, sizeof(MBXUserDataForHostAddTimer));
}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4,15,0))
static IMG_VOID HostTimerCallbackWrapper(struct timer_list *t)
{
	TIMER_CALLBACK_DATA *psTimerCBData = timer_container_of(psTimerCBData, t, sTimer);
#else
static IMG_VOID HostTimerCallbackWrapper(IMG_UINT32 ui32Data)
{
	TIMER_CALLBACK_DATA *psTimerCBData = (TIMER_CALLBACK_DATA*)ui32Data;
#endif

	
	psTimerCBData->pfnTimerFunc(psTimerCBData->pvData);

	
	mod_timer(&psTimerCBData->sTimer, psTimerCBData->ui32Delay + jiffies);
}


extern unsigned long lastFAsyncTime;
static IMG_HANDLE HostAddTimerKernel(PFN_TIMER_FUNC pfnTimerFunc, IMG_VOID *pvData, IMG_UINT32 ui32MsTimeout)
{
	TIMER_CALLBACK_DATA *psTimerCBData=NULL;

	
	if(!pfnTimerFunc)
	{
		PVR_DPF((PVR_DBG_ERROR, "passed invalid callback"));
		return IMG_NULL;
	}

	
	psTimerCBData = KMALLOC(sizeof(TIMER_CALLBACK_DATA), GFP_KERNEL);
	if(psTimerCBData == IMG_NULL)
	{
		PVR_DPF((PVR_DBG_ERROR, "Failed to allocate memory for TIMER_CALLBACK_DATA"));
		return IMG_NULL;
	}

	psTimerCBData->pfnTimerFunc = pfnTimerFunc;
	psTimerCBData->pvData = pvData;

	psTimerCBData->ui32Delay = ((HZ * ui32MsTimeout) < 1000)
								?   1
								:   ((HZ * ui32MsTimeout) / 1000);
	
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4,15,0))
	timer_setup(&psTimerCBData->sTimer, HostTimerCallbackWrapper, 0);
#else
	init_timer(&psTimerCBData->sTimer);

	
	psTimerCBData->sTimer.function = HostTimerCallbackWrapper;
	psTimerCBData->sTimer.data = (IMG_UINT32)psTimerCBData;
#endif
	psTimerCBData->sTimer.expires = psTimerCBData->ui32Delay + jiffies;

	lastFAsyncTime = jiffies;
	
	{
		int i;

		for (i = 0; i < MBX_MAX_TIMERS && gpsTimers[i]; i++)
			;
		if (i < MBX_MAX_TIMERS)
			gpsTimers[i] = psTimerCBData;
	}
	add_timer(&psTimerCBData->sTimer);
	
	return (IMG_HANDLE)psTimerCBData;
}

static PVRSRV_ERROR HostRemoveTimerKernel (IMG_HANDLE hTimer)
{
	TIMER_CALLBACK_DATA    *psTimerCBData = (TIMER_CALLBACK_DATA*)hTimer;
	
	
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6,2,0))
	timer_delete_sync(&psTimerCBData->sTimer);
#else
	del_timer(&psTimerCBData->sTimer);
#endif

	
	{
		int i;

		for (i = 0; i < MBX_MAX_TIMERS; i++)
			if (gpsTimers[i] == psTimerCBData)
				gpsTimers[i] = NULL;
	}
	KFREE(psTimerCBData); 
	
	return PVRSRV_OK;
}
