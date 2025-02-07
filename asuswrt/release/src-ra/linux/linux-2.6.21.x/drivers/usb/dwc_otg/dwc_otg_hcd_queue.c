/* ==========================================================================
 * $File: //dwh/usb_iip/dev/software/otg_ipmate/linux/drivers/dwc_otg_hcd_queue.c $
 * $Revision: 1.1.1.1 $
 * $Date: 2010/12/02 04:37:32 $
 * $Change: 537387 $
 *
 * Synopsys HS OTG Linux Software Driver and documentation (hereinafter,
 * "Software") is an Unsupported proprietary work of Synopsys, Inc. unless
 * otherwise expressly agreed to in writing between Synopsys and you.
 * 
 * The Software IS NOT an item of Licensed Software or Licensed Product under
 * any End User Software License Agreement or Agreement for Licensed Product
 * with Synopsys or any supplement thereto. You are permitted to use and
 * redistribute this Software in source and binary forms, with or without
 * modification, provided that redistributions of source code must retain this
 * notice. You may not view, use, disclose, copy or distribute this file or
 * any information contained herein except pursuant to this license grant from
 * Synopsys. If you do not agree with this notice, including the disclaimer
 * below, then you are not authorized to use the Software.
 * 
 * THIS SOFTWARE IS BEING DISTRIBUTED BY SYNOPSYS SOLELY ON AN "AS IS" BASIS
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE HEREBY DISCLAIMED. IN NO EVENT SHALL SYNOPSYS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 * ========================================================================== */
#ifndef DWC_DEVICE_ONLY

/**
 * @file
 *
 * This file contains the functions to manage Queue Heads and Queue
 * Transfer Descriptors.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/list.h>
#include <linux/interrupt.h>
#include <linux/string.h>
//#include <asm/arch/lm.h>
#include <asm/rt2880/lm.h>
//#include <asm/arch/irqs.h>
#include <linux/dma-mapping.h>
#include <linux/version.h>

#include "dwc_otg_driver.h"
#include "dwc_otg_hcd.h"
#include "dwc_otg_regs.h"

/**
 * This function allocates and initializes a QH.
 *
 * @param hcd The HCD state structure for the DWC OTG controller.
 * @param[in] urb Holds the information about the device/endpoint that we need
 * to initialize the QH.
 *
 * @return Returns pointer to the newly allocated QH, or NULL on error. */
dwc_otg_qh_t *dwc_otg_hcd_qh_create (dwc_otg_hcd_t *hcd, struct urb *urb)
{
	dwc_otg_qh_t *qh;

	/* Allocate memory */
	/** @todo add memflags argument */
	qh = dwc_otg_hcd_qh_alloc ();
	if (qh == NULL) {
		return NULL;
	}

	dwc_otg_hcd_qh_init (hcd, qh, urb);
	return qh;
}

/** Free each QTD in the QH's QTD-list then free the QH.  QH should already be
 * removed from a list.  QTD list should already be empty if called from URB
 * Dequeue.
 *
 * @param[in] hcd HCD instance.
 * @param[in] qh The QH to free.
 */
void dwc_otg_hcd_qh_free (dwc_otg_hcd_t *hcd, dwc_otg_qh_t *qh)
{
	dwc_otg_qtd_t *qtd;
	struct list_head *pos;
	unsigned long flags;

	/* Free each QTD in the QTD list */
	SPIN_LOCK_IRQSAVE(&hcd->lock, flags)
	for (pos = qh->qtd_list.next;
	     pos != &qh->qtd_list;
	     pos = qh->qtd_list.next)
	{
		list_del (pos);
		qtd = dwc_list_to_qtd (pos);
		dwc_otg_hcd_qtd_free (qtd);
	}
	SPIN_UNLOCK_IRQRESTORE(&hcd->lock, flags)

	if (qh->dw_align_buf) {
		dma_free_coherent((dwc_otg_hcd_to_hcd(hcd))->self.controller,
				  hcd->core_if->core_params->max_transfer_size,
				  qh->dw_align_buf,
				  qh->dw_align_buf_dma);
	}
	
	kfree (qh);
	return;
}

/** Initializes a QH structure.
 *
 * @param[in] hcd The HCD state structure for the DWC OTG controller.
 * @param[in] qh The QH to init.
 * @param[in] urb Holds the information about the device/endpoint that we need
 * to initialize the QH. */
#define SCHEDULE_SLOP 10
void dwc_otg_hcd_qh_init(dwc_otg_hcd_t *hcd, dwc_otg_qh_t *qh, struct urb *urb)
{
	char *speed, *type;
	memset (qh, 0, sizeof (dwc_otg_qh_t));

	/* Initialize QH */
	switch (usb_pipetype(urb->pipe)) {
	case PIPE_CONTROL:
		qh->ep_type = USB_ENDPOINT_XFER_CONTROL;
		break;
	case PIPE_BULK:
		qh->ep_type = USB_ENDPOINT_XFER_BULK;
		break;
	case PIPE_ISOCHRONOUS:
		qh->ep_type = USB_ENDPOINT_XFER_ISOC;
		break;
	case PIPE_INTERRUPT: 
		qh->ep_type = USB_ENDPOINT_XFER_INT;
		break;
	}

	qh->ep_is_in = usb_pipein(urb->pipe) ? 1 : 0;

	qh->data_toggle = DWC_OTG_HC_PID_DATA0;
	qh->maxp = usb_maxpacket(urb->dev, urb->pipe, !(usb_pipein(urb->pipe)));
	INIT_LIST_HEAD(&qh->qtd_list);
	INIT_LIST_HEAD(&qh->qh_list_entry);
	qh->channel = NULL;

	/* FS/LS Enpoint on HS Hub 
	 * NOT virtual root hub */
	qh->do_split = 0;
	if (((urb->dev->speed == USB_SPEED_LOW) || 
	     (urb->dev->speed == USB_SPEED_FULL)) &&
	    (urb->dev->tt) && (urb->dev->tt->hub->devnum != 1)) 
	{
		DWC_DEBUGPL(DBG_HCD, "QH init: EP %d: TT found at hub addr %d, for port %d\n", 
			   usb_pipeendpoint(urb->pipe), urb->dev->tt->hub->devnum, 
			   urb->dev->ttport);
		qh->do_split = 1;
	}

	if (qh->ep_type == USB_ENDPOINT_XFER_INT ||
	    qh->ep_type == USB_ENDPOINT_XFER_ISOC) {
		/* Compute scheduling parameters once and save them. */
		hprt0_data_t hprt;

		/** @todo Account for split transfers in the bus time. */
		int bytecount = dwc_hb_mult(qh->maxp) * dwc_max_packet(qh->maxp);

		/* FIXME: work-around patch by Steven */
		qh->usecs = NS_TO_US(usb_calc_bus_time(urb->dev->speed,
					       usb_pipein(urb->pipe),
					       (qh->ep_type == USB_ENDPOINT_XFER_ISOC),
					       bytecount));

		/* Start in a slightly future (micro)frame. */
		qh->sched_frame = dwc_frame_num_inc(hcd->frame_number,
						     SCHEDULE_SLOP);
		qh->interval = urb->interval;
#if 0
		/* Increase interrupt polling rate for debugging. */
		if (qh->ep_type == USB_ENDPOINT_XFER_INT) {
			qh->interval = 8;
		}
#endif		
		hprt.d32 = dwc_read_reg32(hcd->core_if->host_if->hprt0);
		if ((hprt.b.prtspd == DWC_HPRT0_PRTSPD_HIGH_SPEED) && 
		    ((urb->dev->speed == USB_SPEED_LOW) || 
		     (urb->dev->speed == USB_SPEED_FULL))) {
			qh->interval *= 8;
			qh->sched_frame |= 0x7;
			qh->start_split_frame = qh->sched_frame;
		}

	}

	DWC_DEBUGPL(DBG_HCD, "DWC OTG HCD QH Initialized\n");
	DWC_DEBUGPL(DBG_HCDV, "DWC OTG HCD QH  - qh = %p\n", qh);
	DWC_DEBUGPL(DBG_HCDV, "DWC OTG HCD QH  - Device Address = %d\n",
		    urb->dev->devnum);
	DWC_DEBUGPL(DBG_HCDV, "DWC OTG HCD QH  - Endpoint %d, %s\n",
		    usb_pipeendpoint(urb->pipe),
		    usb_pipein(urb->pipe) == USB_DIR_IN ? "IN" : "OUT");

	switch(urb->dev->speed) {
	case USB_SPEED_LOW: 
		speed = "low";	
		break;
	case USB_SPEED_FULL: 
		speed = "full";	
		break;
	case USB_SPEED_HIGH: 
		speed = "high";	
		break;
	default: 
		speed = "?";	
		break;
	}
	DWC_DEBUGPL(DBG_HCDV, "DWC OTG HCD QH  - Speed = %s\n", speed);
	
	switch (qh->ep_type) {
	case USB_ENDPOINT_XFER_ISOC: 
		type = "isochronous";	
		break;
	case USB_ENDPOINT_XFER_INT: 
		type = "interrupt";	
		break;
	case USB_ENDPOINT_XFER_CONTROL: 
		type = "control";	
		break;
	case USB_ENDPOINT_XFER_BULK: 
		type = "bulk";	
		break;
	default: 
		type = "?";	
		break;
	}
	DWC_DEBUGPL(DBG_HCDV, "DWC OTG HCD QH  - Type = %s\n",type);
	
#ifdef DEBUG
	if (qh->ep_type == USB_ENDPOINT_XFER_INT) {
		DWC_DEBUGPL(DBG_HCDV, "DWC OTG HCD QH - usecs = %d\n",
			    qh->usecs);
		DWC_DEBUGPL(DBG_HCDV, "DWC OTG HCD QH - interval = %d\n",
			    qh->interval);
	}
#endif	
	qh->dw_align_buf = NULL;
	return;
}

/**
 * Checks that a channel is available for a periodic transfer.
 *
 * @return 0 if successful, negative error code otherise.
 */
static int periodic_channel_available(dwc_otg_hcd_t *hcd)
{
	/*
	 * Currently assuming that there is a dedicated host channnel for each
	 * periodic transaction plus at least one host channel for
	 * non-periodic transactions.
	 */
	int status;
	int num_channels;

	num_channels = hcd->core_if->core_params->host_channels;
	if ((hcd->periodic_channels + hcd->non_periodic_channels < num_channels) &&
	    (hcd->periodic_channels < num_channels - 1)) {
		status = 0;
	}
	else {
		DWC_NOTICE("%s: Total channels: %d, Periodic: %d, Non-periodic: %d\n",
			   __func__, num_channels, hcd->periodic_channels,
			   hcd->non_periodic_channels);
		status = -ENOSPC;
	}

	return status;
}

/**
 * Checks that there is sufficient bandwidth for the specified QH in the
 * periodic schedule. For simplicity, this calculation assumes that all the
 * transfers in the periodic schedule may occur in the same (micro)frame.
 *
 * @param hcd The HCD state structure for the DWC OTG controller.
 * @param qh QH containing periodic bandwidth required.
 *
 * @return 0 if successful, negative error code otherwise.
 */
static int check_periodic_bandwidth(dwc_otg_hcd_t *hcd, dwc_otg_qh_t *qh)
{
	int 		status;
	uint16_t 	max_claimed_usecs;

	status = 0;

	if (hcd->core_if->core_params->speed == DWC_SPEED_PARAM_HIGH) {
		/*
		 * High speed mode.
		 * Max periodic usecs is 80% x 125 usec = 100 usec.
		 */
		max_claimed_usecs = 100 - qh->usecs;
	} else {
		/*
		 * Full speed mode.
		 * Max periodic usecs is 90% x 1000 usec = 900 usec.
		 */
		max_claimed_usecs = 900 - qh->usecs;
	}

	if (hcd->periodic_usecs > max_claimed_usecs) {
		DWC_NOTICE("%s: already claimed usecs %d, required usecs %d\n",
			   __func__, hcd->periodic_usecs, qh->usecs);
		status = -ENOSPC;
	}

	return status;
}
			
/**
 * Checks that the max transfer size allowed in a host channel is large enough
 * to handle the maximum data transfer in a single (micro)frame for a periodic
 * transfer.
 *
 * @param hcd The HCD state structure for the DWC OTG controller.
 * @param qh QH for a periodic endpoint.
 *
 * @return 0 if successful, negative error code otherwise.
 */
static int check_max_xfer_size(dwc_otg_hcd_t *hcd, dwc_otg_qh_t *qh)
{
	int		status;
	uint32_t 	max_xfer_size;
	uint32_t	max_channel_xfer_size;

	status = 0;

	max_xfer_size = dwc_max_packet(qh->maxp) * dwc_hb_mult(qh->maxp);
	max_channel_xfer_size = hcd->core_if->core_params->max_transfer_size;

	if (max_xfer_size > max_channel_xfer_size) {
		DWC_NOTICE("%s: Periodic xfer length %d > "
			    "max xfer length for channel %d\n",
			    __func__, max_xfer_size, max_channel_xfer_size);
		status = -ENOSPC;
	}

	return status;
}

/**
 * Schedules an interrupt or isochronous transfer in the periodic schedule.
 *
 * @param hcd The HCD state structure for the DWC OTG controller.
 * @param qh QH for the periodic transfer. The QH should already contain the
 * scheduling information.
 *
 * @return 0 if successful, negative error code otherwise.
 */
static int schedule_periodic(dwc_otg_hcd_t *hcd, dwc_otg_qh_t *qh)
{
	int status = 0;

	status = periodic_channel_available(hcd);
	if (status) {
		DWC_NOTICE("%s: No host channel available for periodic "
			   "transfer.\n", __func__);
		return status;
	}

	status = check_periodic_bandwidth(hcd, qh);
	if (status) {
		DWC_NOTICE("%s: Insufficient periodic bandwidth for "
			   "periodic transfer.\n", __func__);
		return status;
	}

	status = check_max_xfer_size(hcd, qh);
	if (status) {
		DWC_NOTICE("%s: Channel max transfer size too small "
			    "for periodic transfer.\n", __func__);
		return status;
	}

	/* Always start in the inactive schedule. */
	list_add_tail(&qh->qh_list_entry, &hcd->periodic_sched_inactive);

	/* Reserve the periodic channel. */
	hcd->periodic_channels++;

	/* Update claimed usecs per (micro)frame. */
	hcd->periodic_usecs += qh->usecs;

	/* Update average periodic bandwidth claimed and # periodic reqs for usbfs. */
	hcd_to_bus(dwc_otg_hcd_to_hcd(hcd))->bandwidth_allocated += qh->usecs / qh->interval;
	if (qh->ep_type == USB_ENDPOINT_XFER_INT) {
		hcd_to_bus(dwc_otg_hcd_to_hcd(hcd))->bandwidth_int_reqs++;
		DWC_DEBUGPL(DBG_HCD, "Scheduled intr: qh %p, usecs %d, period %d\n",
			    qh, qh->usecs, qh->interval);
	} else {
		hcd_to_bus(dwc_otg_hcd_to_hcd(hcd))->bandwidth_isoc_reqs++;
		DWC_DEBUGPL(DBG_HCD, "Scheduled isoc: qh %p, usecs %d, period %d\n",
			    qh, qh->usecs, qh->interval);
	}
		
	return status;
}

/**
 * This function adds a QH to either the non periodic or periodic schedule if
 * it is not already in the schedule. If the QH is already in the schedule, no
 * action is taken.
 *
 * @return 0 if successful, negative error code otherwise.
 */
int dwc_otg_hcd_qh_add (dwc_otg_hcd_t *hcd, dwc_otg_qh_t *qh)
{
	unsigned long flags;
	int status = 0;

	SPIN_LOCK_IRQSAVE(&hcd->lock, flags)

	if (!list_empty(&qh->qh_list_entry)) {
		/* QH already in a schedule. */
		goto done;
	}

	/* Add the new QH to the appropriate schedule */
	if (dwc_qh_is_non_per(qh)) {
		/* Always start in the inactive schedule. */
		list_add_tail(&qh->qh_list_entry, &hcd->non_periodic_sched_inactive);
	} else {
		status = schedule_periodic(hcd, qh);
	}

 done:
	SPIN_UNLOCK_IRQRESTORE(&hcd->lock, flags)

	return status;
}

/**
 * Removes an interrupt or isochronous transfer from the periodic schedule.
 *
 * @param hcd The HCD state structure for the DWC OTG controller.
 * @param qh QH for the periodic transfer.
 */
static void deschedule_periodic(dwc_otg_hcd_t *hcd, dwc_otg_qh_t *qh)
{
	list_del_init(&qh->qh_list_entry);

	/* Release the periodic channel reservation. */
	hcd->periodic_channels--;

	/* Update claimed usecs per (micro)frame. */
	hcd->periodic_usecs -= qh->usecs;

	/* Update average periodic bandwidth claimed and # periodic reqs for usbfs. */
	hcd_to_bus(dwc_otg_hcd_to_hcd(hcd))->bandwidth_allocated -= qh->usecs / qh->interval;

	if (qh->ep_type == USB_ENDPOINT_XFER_INT) {
		hcd_to_bus(dwc_otg_hcd_to_hcd(hcd))->bandwidth_int_reqs--;
		DWC_DEBUGPL(DBG_HCD, "Descheduled intr: qh %p, usecs %d, period %d\n",
			    qh, qh->usecs, qh->interval);
	} else {
		hcd_to_bus(dwc_otg_hcd_to_hcd(hcd))->bandwidth_isoc_reqs--;
		DWC_DEBUGPL(DBG_HCD, "Descheduled isoc: qh %p, usecs %d, period %d\n",
			    qh, qh->usecs, qh->interval);
	}
}

/** 
 * Removes a QH from either the non-periodic or periodic schedule.  Memory is
 * not freed.
 *
 * @param[in] hcd The HCD state structure.
 * @param[in] qh QH to remove from schedule. */
void dwc_otg_hcd_qh_remove (dwc_otg_hcd_t *hcd, dwc_otg_qh_t *qh)
{
	unsigned long flags;

	SPIN_LOCK_IRQSAVE(&hcd->lock, flags);

	if (list_empty(&qh->qh_list_entry)) {
		/* QH is not in a schedule. */
		goto done;
	}

	if (dwc_qh_is_non_per(qh)) {
		if (hcd->non_periodic_qh_ptr == &qh->qh_list_entry) {
			hcd->non_periodic_qh_ptr = hcd->non_periodic_qh_ptr->next;
		}
		list_del_init(&qh->qh_list_entry);
	} else {
		deschedule_periodic(hcd, qh);
	}

 done:
	SPIN_UNLOCK_IRQRESTORE(&hcd->lock, flags)
}

/**
 * Deactivates a QH. For non-periodic QHs, removes the QH from the active
 * non-periodic schedule. The QH is added to the inactive non-periodic
 * schedule if any QTDs are still attached to the QH.
 *
 * For periodic QHs, the QH is removed from the periodic queued schedule. If
 * there are any QTDs still attached to the QH, the QH is added to either the
 * periodic inactive schedule or the periodic ready schedule and its next
 * scheduled frame is calculated. The QH is placed in the ready schedule if
 * the scheduled frame has been reached already. Otherwise it's placed in the
 * inactive schedule. If there are no QTDs attached to the QH, the QH is
 * completely removed from the periodic schedule.
 */
void dwc_otg_hcd_qh_deactivate(dwc_otg_hcd_t *hcd, dwc_otg_qh_t *qh, int sched_next_periodic_split)
{
	unsigned long flags;
	SPIN_LOCK_IRQSAVE(&hcd->lock, flags);

	if (dwc_qh_is_non_per(qh)) {
		dwc_otg_hcd_qh_remove(hcd, qh);
		if (!list_empty(&qh->qtd_list)) {
			/* Add back to inactive non-periodic schedule. */
			dwc_otg_hcd_qh_add(hcd, qh);
		}
	} else {
		uint16_t frame_number =	dwc_otg_hcd_get_frame_number(dwc_otg_hcd_to_hcd(hcd));

		if (qh->do_split) {
			/* Schedule the next continuing periodic split transfer */
			if (sched_next_periodic_split) {

				qh->sched_frame = frame_number;
				if (dwc_frame_num_le(frame_number,
						     dwc_frame_num_inc(qh->start_split_frame, 1))) {
					/*
					 * Allow one frame to elapse after start
					 * split microframe before scheduling
					 * complete split, but DONT if we are
					 * doing the next start split in the
					 * same frame for an ISOC out.
					 */
					if ((qh->ep_type != USB_ENDPOINT_XFER_ISOC) || (qh->ep_is_in != 0)) {
						qh->sched_frame = dwc_frame_num_inc(qh->sched_frame, 1);
					}
				}
			} else {
				qh->sched_frame = dwc_frame_num_inc(qh->start_split_frame,
								     qh->interval);
				if (dwc_frame_num_le(qh->sched_frame, frame_number)) {
					qh->sched_frame = frame_number;
				}
				qh->sched_frame |= 0x7;
				qh->start_split_frame = qh->sched_frame;
			}
		} else {
			qh->sched_frame = dwc_frame_num_inc(qh->sched_frame, qh->interval);
			if (dwc_frame_num_le(qh->sched_frame, frame_number)) {
				qh->sched_frame = frame_number;
			}
		}

		if (list_empty(&qh->qtd_list)) {
			dwc_otg_hcd_qh_remove(hcd, qh);
		} else {
			/*
			 * Remove from periodic_sched_queued and move to
			 * appropriate queue.
			 */
			if (qh->sched_frame == frame_number) {
				list_move(&qh->qh_list_entry,
					  &hcd->periodic_sched_ready);
			} else {
				list_move(&qh->qh_list_entry,
					  &hcd->periodic_sched_inactive);
			}
		}
	}

	SPIN_UNLOCK_IRQRESTORE(&hcd->lock, flags);
}

/** 
 * This function allocates and initializes a QTD. 
 *
 * @param[in] urb The URB to create a QTD from.  Each URB-QTD pair will end up
 * pointing to each other so each pair should have a unique correlation.
 *
 * @return Returns pointer to the newly allocated QTD, or NULL on error. */
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,20)
dwc_otg_qtd_t *dwc_otg_hcd_qtd_create (struct urb *urb, int mem_flags)
#else
dwc_otg_qtd_t *dwc_otg_hcd_qtd_create (struct urb *urb, gfp_t mem_flags)
#endif
{
	dwc_otg_qtd_t *qtd;

	qtd = dwc_otg_hcd_qtd_alloc (mem_flags);
	if (qtd == NULL) {
		return NULL;
	}

	dwc_otg_hcd_qtd_init (qtd, urb);
	return qtd;
}

/** 
 * Initializes a QTD structure.
 *
 * @param[in] qtd The QTD to initialize.
 * @param[in] urb The URB to use for initialization.  */
void dwc_otg_hcd_qtd_init (dwc_otg_qtd_t *qtd, struct urb *urb)
{
	memset (qtd, 0, sizeof (dwc_otg_qtd_t));
	qtd->urb = urb;
	if (usb_pipecontrol(urb->pipe)) {
		/*
		 * The only time the QTD data toggle is used is on the data
		 * phase of control transfers. This phase always starts with
		 * DATA1.
		 */
		qtd->data_toggle = DWC_OTG_HC_PID_DATA1;
		qtd->control_phase = DWC_OTG_CONTROL_SETUP;
	}

	/* start split */
	qtd->complete_split = 0;
	qtd->isoc_split_pos = DWC_HCSPLIT_XACTPOS_ALL;
	qtd->isoc_split_offset = 0;

	/* Store the qtd ptr in the urb to reference what QTD. */
	urb->hcpriv = qtd;
	return;
}

/**
 * This function adds a QTD to the QTD-list of a QH.  It will find the correct
 * QH to place the QTD into.  If it does not find a QH, then it will create a
 * new QH. If the QH to which the QTD is added is not currently scheduled, it
 * is placed into the proper schedule based on its EP type.
 *
 * @param[in] qtd The QTD to add
 * @param[in] dwc_otg_hcd The DWC HCD structure
 *
 * @return 0 if successful, negative error code otherwise.
 */
int dwc_otg_hcd_qtd_add (dwc_otg_qtd_t *qtd,
			 dwc_otg_hcd_t *dwc_otg_hcd)
{
	struct usb_host_endpoint *ep;
	dwc_otg_qh_t *qh;
	unsigned long flags;
	int retval = 0;

	struct urb *urb = qtd->urb;

	SPIN_LOCK_IRQSAVE(&dwc_otg_hcd->lock, flags);

	/*
	 * Get the QH which holds the QTD-list to insert to. Create QH if it
	 * doesn't exist.
	 */
	ep = dwc_urb_to_endpoint(urb);
	qh = (dwc_otg_qh_t *)ep->hcpriv;
	if (qh == NULL) {
		qh = dwc_otg_hcd_qh_create (dwc_otg_hcd, urb);
		if (qh == NULL) {
			goto done;
		}
		ep->hcpriv = qh;
	}

	retval = dwc_otg_hcd_qh_add(dwc_otg_hcd, qh);
	if (retval == 0) {
		list_add_tail(&qtd->qtd_list_entry, &qh->qtd_list);
	}

 done:
	SPIN_UNLOCK_IRQRESTORE(&dwc_otg_hcd->lock, flags);

	return retval;
}

#endif /* DWC_DEVICE_ONLY */
