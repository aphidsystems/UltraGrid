/* dvbm_txu.c
 *
 * Linux driver functions for Linear Systems Ltd. DVB Master III Tx.
 *
 * Copyright (C) 2003-2010 Linear Systems Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either Version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public Licence for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 *
 * Linear Systems can be contacted at <http://www.linsys.ca/>.
 *
 */

#include <linux/kernel.h> /* KERN_INFO */

#include <linux/fs.h> /* inode, file, file_operations */
#include <linux/sched.h> /* pt_regs */
#include <linux/pci.h> /* pci_resource_start () */
#include <linux/slab.h> /* kzalloc () */
#include <linux/list.h> /* INIT_LIST_HEAD () */
#include <linux/spinlock.h> /* spin_lock_init () */
#include <linux/init.h> /* __devinit */
#include <linux/errno.h> /* error codes */
#include <linux/interrupt.h> /* irqreturn_t */
#include <linux/mutex.h> /* mutex_init () */

#include <asm/bitops.h> /* set_bit () */

#include "asicore.h"
#include "../include/master.h"
#include "mdev.h"
#include "mdma.h"
#include "dvbm.h"
#include "plx9080.h"
#include "dvbm_fdu.h"

static const char dvbm_txu_name[] = DVBM_NAME_TXU;
static const char dvbm_txe_name[] = DVBM_NAME_TXE;

/* Static function prototypes */
static irqreturn_t IRQ_HANDLER(dvbm_txu_irq_handler,irq,dev_id,regs);

static DEVICE_ATTR(uid,S_IRUGO,
	dvbm_fdu_show_uid,NULL);

/**
 * dvbm_txu_pci_probe - PCI insertion handler for a DVB Master III Tx
 * @pdev: PCI device
 *
 * Handle the insertion of a DVB Master III Tx.
 * Returns a negative error code on failure and 0 on success.
 **/
int __devinit
dvbm_txu_pci_probe (struct pci_dev *pdev)
{
	int err;
	unsigned int cap;
	struct master_dev *card;

	err = dvbm_pci_probe_generic (pdev);
	if (err < 0) {
		goto NO_PCI;
	}

	/* Initialize the driver_data pointer so that dvbm_txu_pci_remove()
	 * doesn't try to free it if an error occurs */
	pci_set_drvdata (pdev, NULL);

	/* Allocate and initialize a board info structure */
	if ((card = (struct master_dev *)
		kzalloc (sizeof (*card), GFP_KERNEL)) == NULL) {
		err = -ENOMEM;
		goto NO_MEM;
	}

	card->bridge_addr = ioremap_nocache (pci_resource_start (pdev, 0),
		pci_resource_len (pdev, 0));
	card->core.port = pci_resource_start (pdev, 2);
	card->version = master_inl (card, DVBM_FDU_CSR) >> 16;
	switch (pdev->device) {
	default:
	case DVBM_PCI_DEVICE_ID_LINSYS_DVBTXU:
		card->name = dvbm_txu_name;
		card->capabilities = 0;
		break;
	case DVBM_PCI_DEVICE_ID_LINSYS_DVBTXE:
		card->name = dvbm_txe_name;
		card->capabilities = MASTER_CAP_UID;
		break;
	}
	card->id = pdev->device;
	card->irq = pdev->irq;
	card->irq_handler = dvbm_txu_irq_handler;
	INIT_LIST_HEAD(&card->iface_list);
	/* Lock for ICSR */
	spin_lock_init (&card->irq_lock);
	/* Lock for IBSTR, IPSTR, FTR, TCSR */
	spin_lock_init (&card->reg_lock);
	mutex_init (&card->users_mutex);
	card->parent = &pdev->dev;

	/* Print the firmware version */
	printk (KERN_INFO "%s: %s detected, firmware version %u.%u (0x%04X)\n",
		dvbm_driver_name, card->name,
		card->version >> 8, card->version & 0x00ff, card->version);

	/* Store the pointer to the board info structure
	 * in the PCI info structure */
	pci_set_drvdata (pdev, card);

	/* Reset the FPGA */
	master_outl (card, DVBM_FDU_TCSR, DVBM_FDU_TCSR_RST);

	/* Reset the PCI 9056 */
	plx_reset_bridge (card->bridge_addr);

	/* Setup the PCI 9056 */
	writel (PLX_INTCSR_PCIINT_ENABLE |
		PLX_INTCSR_PCILOCINT_ENABLE |
		PLX_INTCSR_DMA0INT_ENABLE,
		card->bridge_addr + PLX_INTCSR);
	writel (PLX_DMAMODE_32BIT | PLX_DMAMODE_READY |
		PLX_DMAMODE_LOCALBURST | PLX_DMAMODE_CHAINED |
		PLX_DMAMODE_INT | PLX_DMAMODE_CLOC |
		PLX_DMAMODE_DEMAND | PLX_DMAMODE_INTPCI,
		card->bridge_addr + PLX_DMAMODE0);
	/* Dummy read to flush PCI posted writes */
	readl (card->bridge_addr + PLX_INTCSR);

	/* Register a DVB Master device */
	if ((err = dvbm_register (card)) < 0) {
		goto NO_DEV;
	}

	/* Add device attributes */
	if (card->capabilities & MASTER_CAP_UID) {
		if ((err = device_create_file (card->dev,
			&dev_attr_uid)) < 0) {
			printk (KERN_WARNING
				"%s: unable to create file 'uid'\n",
				dvbm_driver_name);
		}
	}

	/* Register a transmit interface */
	cap = ASI_CAP_TX_MAKE204 | ASI_CAP_TX_FINETUNING |
		ASI_CAP_TX_BYTECOUNTER | ASI_CAP_TX_SETCLKSRC |
		ASI_CAP_TX_FIFOUNDERRUN | ASI_CAP_TX_LARGEIB |
		ASI_CAP_TX_INTERLEAVING | ASI_CAP_TX_DATA |
		ASI_CAP_TX_27COUNTER |
		ASI_CAP_TX_TIMESTAMPS |
		ASI_CAP_TX_NULLPACKETS;
	if (card->version >= 0x0e07) {
		cap |= ASI_CAP_TX_PTIMESTAMPS;
	}
	if ((err = asi_register_iface (card,
		&plx_dma_ops,
		DVBM_FDU_FIFO,
		MASTER_DIRECTION_TX,
		&dvbm_fdu_txfops,
		&dvbm_fdu_txops,
		cap,
		4,
		ASI_CTL_TRANSPORT_DVB_ASI)) < 0) {
		goto NO_IFACE;
	}

	return 0;

NO_IFACE:
NO_DEV:
NO_MEM:
	dvbm_txu_pci_remove (pdev);
NO_PCI:
	return err;
}

/**
 * dvbm_txu_irq_handler - DVB Master III Tx interrupt service routine
 * @irq: interrupt number
 * @dev_id: pointer to the device data structure
 * @regs: processor context
 **/
static irqreturn_t
IRQ_HANDLER(dvbm_txu_irq_handler,irq,dev_id,regs)
{
	struct master_dev *card = dev_id;
	unsigned int intcsr = readl (card->bridge_addr + PLX_INTCSR);
	unsigned int status, interrupting_iface = 0;
	struct master_iface *iface = list_entry (card->iface_list.next,
		struct master_iface, list);

	if (intcsr & PLX_INTCSR_DMA0INT_ACTIVE) {
		/* Read the interrupt type and clear it */
		status = readb (card->bridge_addr + PLX_DMACSR0);
		writeb (PLX_DMACSR_ENABLE | PLX_DMACSR_CLINT,
			card->bridge_addr + PLX_DMACSR0);

		/* Increment the buffer pointer */
		mdma_advance (iface->dma);

		/* Flag end-of-chain */
		if (status & PLX_DMACSR_DONE) {
			set_bit (ASI_EVENT_TX_BUFFER_ORDER, &iface->events);
			set_bit (0, &iface->dma_done);
		}

		interrupting_iface |= 0x1;
	}
	if (intcsr & PLX_INTCSR_PCILOCINT_ACTIVE) {
		/* Clear the source of the interrupt */
		spin_lock (&card->irq_lock);
		status = master_inl (card, DVBM_FDU_ICSR);
		master_outl (card, DVBM_FDU_ICSR, status);
		spin_unlock (&card->irq_lock);

		if (status & DVBM_FDU_ICSR_TXUIS) {
			set_bit (ASI_EVENT_TX_FIFO_ORDER,
				&iface->events);
			interrupting_iface |= 0x1;
		}
		if (status & DVBM_FDU_ICSR_TXDIS) {
			set_bit (ASI_EVENT_TX_DATA_ORDER,
				&iface->events);
			interrupting_iface |= 0x1;
		}
	}

	if (interrupting_iface) {
		/* Dummy read to flush PCI posted writes */
		readb (card->bridge_addr + PLX_DMACSR0);

		wake_up (&iface->queue);
		return IRQ_HANDLED;
	}
	return IRQ_NONE;
}

