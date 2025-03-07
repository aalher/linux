// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/*
 * Copyright(c) 2015, 2016 Intel Corporation.
 */

#include <linux/pci.h>
#include <linux/delay.h>
#include <linux/bitmap.h>

#include "hfi.h"
#include "common.h"
#include "sdma.h"

#define LINK_UP_DELAY  500  /* in microseconds */

static void set_mgmt_allowed(struct hfi1_pportdata *ppd)
{
	u32 frame;
	struct hfi1_devdata *dd = ppd->dd;

	if (ppd->neighbor_type == NEIGHBOR_TYPE_HFI) {
		ppd->mgmt_allowed = 1;
	} else {
		read_8051_config(dd, REMOTE_LNI_INFO, GENERAL_CONFIG, &frame);
		ppd->mgmt_allowed = (frame >> MGMT_ALLOWED_SHIFT)
		& MGMT_ALLOWED_MASK;
	}
}

/*
 * Our neighbor has indicated that we are allowed to act as a fabric
 * manager, so place the full management partition key in the second
 * (0-based) pkey array position. Note that we should already have
 * the limited management partition key in array element 1, and also
 * that the port is not yet up when add_full_mgmt_pkey() is invoked.
 */
static void add_full_mgmt_pkey(struct hfi1_pportdata *ppd)
{
	struct hfi1_devdata *dd = ppd->dd;

	/* Sanity check - ppd->pkeys[2] should be 0, or already initialized */
	if (!((ppd->pkeys[2] == 0) || (ppd->pkeys[2] == FULL_MGMT_P_KEY)))
		dd_dev_warn(dd, "%s pkey[2] already set to 0x%x, resetting it to 0x%x\n",
			    __func__, ppd->pkeys[2], FULL_MGMT_P_KEY);
	ppd->pkeys[2] = FULL_MGMT_P_KEY;
	(void)hfi1_set_ib_cfg(ppd, HFI1_IB_CFG_PKEYS, 0);
	hfi1_event_pkey_change(ppd->dd, ppd->port);
}

static void signal_ib_event(struct hfi1_pportdata *ppd, enum ib_event_type ev)
{
	struct ib_event event;
	struct hfi1_devdata *dd = ppd->dd;

	/*
	 * Only call ib_dispatch_event() if the IB device has been
	 * registered.  HFI1_INITED is set iff the driver has successfully
	 * registered with the IB core.
	 */
	if (!(dd->flags & HFI1_INITTED))
		return;
	event.device = &dd->verbs_dev.rdi.ibdev;
	event.element.port_num = ppd->port;
	event.event = ev;
	ib_dispatch_event(&event);
}

/**
 * handle_linkup_change - finish linkup/down state changes
 * @dd: valid device
 * @linkup: link state information
 *
 * Handle a linkup or link down notification.
 * The HW needs time to finish its link up state change. Give it that chance.
 *
 * This is called outside an interrupt.
 *
 */
void handle_linkup_change(struct hfi1_devdata *dd, u32 linkup)
{
	struct hfi1_pportdata *ppd = &dd->pport[0];
	enum ib_event_type ev;

	if (!(ppd->linkup ^ !!linkup))
		return;	/* no change, nothing to do */

	if (linkup) {
		/*
		 * Quick linkup and all link up on the simulator does not
		 * trigger or implement:
		 *	- VerifyCap interrupt
		 *	- VerifyCap frames
		 * But rather moves directly to LinkUp.
		 *
		 * Do the work of the VerifyCap interrupt handler,
		 * handle_verify_cap(), but do not try moving the state to
		 * LinkUp as we are already there.
		 *
		 * NOTE: This uses this device's vAU, vCU, and vl15_init for
		 * the remote values.  Both sides must be using the values.
		 */
		if (quick_linkup || dd->icode == ICODE_FUNCTIONAL_SIMULATOR) {
			set_up_vau(dd, dd->vau);
			set_up_vl15(dd, dd->vl15_init);
			assign_remote_cm_au_table(dd, dd->vcu);
		}

		ppd->neighbor_guid =
			read_csr(dd, DC_DC8051_STS_REMOTE_GUID);
		ppd->neighbor_type =
			read_csr(dd, DC_DC8051_STS_REMOTE_NODE_TYPE) &
				 DC_DC8051_STS_REMOTE_NODE_TYPE_VAL_MASK;
		ppd->neighbor_port_number =
			read_csr(dd, DC_DC8051_STS_REMOTE_PORT_NO) &
				 DC_DC8051_STS_REMOTE_PORT_NO_VAL_SMASK;
		ppd->neighbor_fm_security =
			read_csr(dd, DC_DC8051_STS_REMOTE_FM_SECURITY) &
				 DC_DC8051_STS_LOCAL_FM_SECURITY_DISABLED_MASK;
		dd_dev_info(dd,
			    "Neighbor Guid %llx, Type %d, Port Num %d\n",
			    ppd->neighbor_guid, ppd->neighbor_type,
			    ppd->neighbor_port_number);

		/* HW needs LINK_UP_DELAY to settle, give it that chance */
		udelay(LINK_UP_DELAY);

		/*
		 * 'MgmtAllowed' information, which is exchanged during
		 * LNI, is available at this point.
		 */
		set_mgmt_allowed(ppd);

		if (ppd->mgmt_allowed)
			add_full_mgmt_pkey(ppd);

		/* physical link went up */
		ppd->linkup = 1;
		ppd->offline_disabled_reason =
			HFI1_ODR_MASK(OPA_LINKDOWN_REASON_NONE);

		/* link widths are not available until the link is fully up */
		get_linkup_link_widths(ppd);

	} else {
		/* physical link went down */
		ppd->linkup = 0;

		/* clear HW details of the previous connection */
		ppd->actual_vls_operational = 0;
		reset_link_credits(dd);

		/* freeze after a link down to guarantee a clean egress */
		start_freeze_handling(ppd, FREEZE_SELF | FREEZE_LINK_DOWN);

		ev = IB_EVENT_PORT_ERR;

		hfi1_set_uevent_bits(ppd, _HFI1_EVENT_LINKDOWN_BIT);

		/* if we are down, the neighbor is down */
		ppd->neighbor_normal = 0;

		/* notify IB of the link change */
		signal_ib_event(ppd, ev);
	}
}

/*
 * Handle receive or urgent interrupts for user contexts.  This means a user
 * process was waiting for a packet to arrive, and didn't want to poll.
 */
void handle_user_interrupt(struct hfi1_ctxtdata *rcd)
{
	struct hfi1_devdata *dd = rcd->dd;
	unsigned long flags;

	spin_lock_irqsave(&dd->uctxt_lock, flags);
	if (bitmap_empty(rcd->in_use_ctxts, HFI1_MAX_SHARED_CTXTS))
		goto done;

	if (test_and_clear_bit(HFI1_CTXT_WAITING_RCV, &rcd->event_flags)) {
		wake_up_interruptible(&rcd->wait);
		hfi1_rcvctrl(dd, HFI1_RCVCTRL_INTRAVAIL_DIS, rcd);
	} else if (test_and_clear_bit(HFI1_CTXT_WAITING_URG,
							&rcd->event_flags)) {
		rcd->urgent++;
		wake_up_interruptible(&rcd->wait);
	}
done:
	spin_unlock_irqrestore(&dd->uctxt_lock, flags);
}
