// SPDX-License-Identifier: GPL-2.0
/*
 * dmaengine driver skeleton for the ROHM/BeagleV-Fire FPGA fabric DMA
 * controller found at 0x60010000, currently driven "by hand" from
 * drivers/iio/adc/dma-sampler.c (see dma_sampler_start_transfer() and
 * dma_sampler_irq_handler() there for the existing register-poking
 * implementation this driver is meant to replace).
 *
 * This file is a SKELETON only: function bodies contain the structural
 * dmaengine/virt-dma bookkeeping calls, but the actual FPGA register
 * accesses are left as TODO comments. It is meant as a starting point for
 * turning this custom descriptor-based DMA IP into a standard `dma_chan`
 * that other drivers (e.g. a future SPI-offload provider, or IIO's
 * industrialio-buffer-dmaengine.c) can request via dma_request_chan().
 *
 * Background for readers new to the dmaengine framework
 * -------------------------------------------------------
 * The Linux "dmaengine" subsystem (include/linux/dmaengine.h) is a generic
 * API that lets *consumer* drivers (e.g. an SPI/ADC/UART driver) request a
 * DMA channel by name from devicetree and issue transfers without knowing
 * anything about the underlying DMA hardware. A *provider* driver (this
 * file) implements a `struct dma_device` with a set of callbacks
 * (`device_alloc_chan_resources`, `device_prep_dma_cyclic`,
 * `device_issue_pending`, `device_tx_status`, ...) and registers one or
 * more `struct dma_chan` with the core via `dma_async_device_register()`.
 *
 * Most real-world DMA controllers manage a queue of "descriptors" (one
 * descriptor = one transfer request). The generic helper library
 * "virt-dma" (drivers/dma/virt-dma.h) provides a `struct virt_dma_chan`
 * that keeps 4 descriptor lists (allocated/submitted/issued/completed) and
 * a tasklet that invokes completion callbacks, so individual drivers don't
 * have to reinvent that bookkeeping. This skeleton builds directly on
 * virt-dma, as the vast majority of simple DMA engine drivers do.
 *
 * Our specific hardware only ever needs *cyclic* transfers: the FPGA
 * sampler continuously produces half-buffers ("periods") of ADC samples
 * that must be copied from the LSRAM aperture into a fixed ping-pong DDR
 * buffer, forever, until the capture is stopped. This maps directly onto
 * dmaengine's DMA_CYCLIC transaction type and `device_prep_dma_cyclic()`
 * API (the same API used by audio/ADC drivers that stream into a circular
 * buffer), so we do not need to implement the more general
 * `device_prep_slave_sg()` scatter-gather API for one-shot transfers.
 */

#include <linux/bitops.h>
#include <linux/dmaengine.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include "virt-dma.h"

#define FPGA_SAMPLER_DMA_ADDRESS	0x60010000
#define FPGA_SAMPLER_DMA_SIZE		0x1000

/*
 * struct fpga_sampler_dma_desc - one cyclic transfer request
 * @vd:         base virt-dma descriptor; MUST be the first member so that
 *              container_of() from a "struct virt_dma_desc *" works.
 * @buf_addr:   physical/DMA address of the start of the ping-pong buffer.
 * @buf_len:    total length of the ping-pong buffer, in bytes.
 * @period_len: length of a single period (i.e. one "half buffer") in
 *              bytes; the hardware raises an interrupt after each period
 *              and this driver reports that period as complete to the
 *              dmaengine core, which in turn lets the IIO buffer core
 *              hand that chunk of samples to userspace.
 *
 * This mirrors what a single call to dmaengine_prep_dma_cyclic() from a
 * consumer driver describes: "keep copying periods from this circular
 * buffer, notify me after every period, until I tell you to stop".
 */
struct fpga_sampler_dma_desc {
	struct virt_dma_desc vd;
	dma_addr_t buf_addr;
	size_t buf_len;
	size_t period_len;
};

/*
 * struct fpga_sampler_dma_chan - our one and only DMA channel
 * @vc:        base virt-dma channel; provides the descriptor queue and
 *             completion tasklet plumbing.
 * @desc:      descriptor currently being played out by the hardware, or
 *             NULL if the channel is idle. Protected by vc.lock.
 * @next_half: which ping-pong half (0/1) the hardware should copy into
 *             next; mirrors the "half" tracking already present in
 *             dma-sampler.c's driver-private state today.
 *
 * The FPGA IP only implements a single DMA "channel" (one source, one
 * ping-pong destination), so unlike more complex DMA controllers we do
 * not need an array of channels here - just one embedded in the device
 * struct below.
 */
struct fpga_sampler_dma_chan {
	struct virt_dma_chan vc;
	struct fpga_sampler_dma_desc *desc;
	unsigned int next_half;
};

/*
 * struct fpga_sampler_dma - per-device state
 * @dma_dev: the registered dmaengine device (one per platform_device).
 * @chan:    the single hardware channel exposed by this IP.
 * @regs:    ioremap()'d MMIO window for the DMA_* registers at
 *           0x60010000 (DMA_INTR_0_*, DMA_DESC_0_*, DMA_START_OPERATION_REG
 *           etc. - see drivers/iio/adc/dma-sampler.c for the existing
 *           register map this driver takes over).
 * @irq:     DMA completion interrupt (PLIC source 121 on BeagleV-Fire).
 */
struct fpga_sampler_dma {
	struct dma_device dma_dev;
	struct fpga_sampler_dma_chan chan;
	void __iomem *regs;
	int irq;
};

static inline struct fpga_sampler_dma_chan *to_fpga_sampler_dma_chan(struct dma_chan *chan)
{
	return container_of(to_virt_chan(chan), struct fpga_sampler_dma_chan, vc);
}

static inline struct fpga_sampler_dma_desc *to_fpga_sampler_dma_desc(struct virt_dma_desc *vd)
{
	return container_of(vd, struct fpga_sampler_dma_desc, vd);
}

static inline struct fpga_sampler_dma *to_fpga_sampler_dma(struct dma_chan *chan)
{
	return container_of(chan->device, struct fpga_sampler_dma, dma_dev);
}

/**
 * fpga_sampler_dma_desc_free() - free a completed/discarded descriptor
 * @vd: the virt-dma descriptor to free
 *
 * Registered as `vc.desc_free` and invoked by the virt-dma core once a
 * descriptor is no longer needed (either after normal completion when the
 * consumer does not want to reuse it, or when terminating all transfers).
 * All we need to do here is free the memory backing our container struct;
 * there is no hardware state to release since the descriptor is purely a
 * software bookkeeping object (the actual programming into the FPGA
 * DMA_DESC_0_* registers happens later, in fpga_sampler_dma_start_transfer()).
 */
static void fpga_sampler_dma_desc_free(struct virt_dma_desc *vd)
{
	struct fpga_sampler_dma_desc *desc = to_fpga_sampler_dma_desc(vd);

	kfree(desc);
}

/**
 * fpga_sampler_dma_alloc_chan_resources() - prepare channel for use
 * @chan: channel being requested by a consumer (via dma_request_chan())
 *
 * Called once when a consumer driver first requests this channel (e.g.
 * from an SPI-offload provider's .rx_stream_request_dma_chan()
 * implementation, or directly from IIO's dmaengine buffer helper). This is
 * the place to:
 *   - clear/acknowledge any stale interrupt state left over in the FPGA
 *     DMA_INTR_0_* registers,
 *   - request/enable any clocks needed by the DMA IP (if applicable),
 *   - otherwise make sure the hardware is in a known, idle state before
 *     the first transfer is queued.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int fpga_sampler_dma_alloc_chan_resources(struct dma_chan *chan)
{
	struct fpga_sampler_dma *dmac = to_fpga_sampler_dma(chan);

	/* TODO: clear/mask DMA_INTR_0_* registers via dmac->regs, mirroring
	 * the "clear interrupts" block currently done in
	 * dma_sampler_probe()/dma_sampler_start_transfer().
	 */

	return 0;
}

/**
 * fpga_sampler_dma_free_chan_resources() - release channel after last use
 * @chan: channel being released (consumer called dma_release_channel())
 *
 * Counterpart to fpga_sampler_dma_alloc_chan_resources(). Should ensure any
 * in-flight transfer is stopped (defensively - the consumer is expected to
 * have already called device_terminate_all()/dmaengine_terminate_sync()
 * before releasing the channel) and that all queued/completed descriptors
 * are freed via the virt-dma helper.
 */
static void fpga_sampler_dma_free_chan_resources(struct dma_chan *chan)
{
	struct fpga_sampler_dma_chan *dchan = to_fpga_sampler_dma_chan(chan);

	/* TODO: make sure the hardware DMA engine is stopped (defensive; the
	 * core should have already called device_terminate_all()).
	 */

	vchan_free_chan_resources(&dchan->vc);
}

/**
 * fpga_sampler_dma_prep_dma_cyclic() - build a cyclic transfer descriptor
 * @chan:       channel the transfer is being prepared for
 * @buf_addr:   DMA address of the start of the ping-pong buffer supplied
 *              by the consumer (e.g. the buffer allocated by IIO's
 *              dmaengine buffer core)
 * @buf_len:    total length of that buffer, in bytes
 * @period_len: length of each period ("half buffer") within it, in bytes;
 *              for this hardware this should match
 *              SAMPLER_BUFFER_BYTE_COUNT_HALF from dma-sampler.c
 * @direction:  transfer direction; only DMA_DEV_TO_MEM makes sense here,
 *              since the FPGA sampler LSRAM is always the source and DDR
 *              is always the destination
 * @flags:      standard dmaengine prep flags (unused for now)
 *
 * This is the core "set up a repeating transfer" entry point used by
 * consumers that want a circular/ping-pong buffer kept continuously full,
 * which is exactly our use case. It must NOT touch hardware registers or
 * start any transfer - it only records what the transfer *should* look
 * like once issued. Actually programming the hardware happens later, in
 * device_issue_pending() / fpga_sampler_dma_start_transfer(), and is
 * re-triggered periodically from the IRQ handler for each subsequent
 * period.
 *
 * Return: a `struct dma_async_tx_descriptor *` wrapping our descriptor
 * (via vchan_tx_prep()), or an ERR_PTR()/NULL on failure.
 */
static struct dma_async_tx_descriptor *
fpga_sampler_dma_prep_dma_cyclic(struct dma_chan *chan, dma_addr_t buf_addr,
				 size_t buf_len, size_t period_len,
				 enum dma_transfer_direction direction,
				 unsigned long flags)
{
	struct fpga_sampler_dma_chan *dchan = to_fpga_sampler_dma_chan(chan);
	struct fpga_sampler_dma_desc *desc;

	if (direction != DMA_DEV_TO_MEM)
		return NULL;

	/* TODO: validate buf_len/period_len against hardware limits (e.g.
	 * SAMPLER_BUFFER_BYTE_COUNT_HALF) and that buf_len is an integer
	 * multiple of period_len (required for ping-pong operation).
	 */

	desc = kzalloc(sizeof(*desc), GFP_NOWAIT);
	if (!desc)
		return NULL;

	desc->buf_addr = buf_addr;
	desc->buf_len = buf_len;
	desc->period_len = period_len;

	return vchan_tx_prep(&dchan->vc, &desc->vd, flags);
}

/**
 * fpga_sampler_dma_start_transfer() - program the hardware for one period
 * @dmac:  device state
 * @dchan: our single channel
 *
 * Internal helper (not a dmaengine callback) that reprograms the FPGA DMA
 * descriptor registers (DMA_DESC_0_SOURCE_ADDR_REG/DEST_ADDR_REG/
 * BYTE_COUNT_REG/CONFIG_REG) and kicks off the transfer for the *next*
 * period of the currently-active cyclic descriptor, alternating between
 * the two ping-pong halves via dchan->next_half - directly analogous to
 * dma_sampler_start_transfer() in the current dma-sampler.c driver.
 *
 * Called from fpga_sampler_dma_issue_pending() to start the very first
 * period, and again from the IRQ handler after each period completes, to
 * keep the ping-pong stream going for as long as the descriptor remains
 * queued (i.e. until fpga_sampler_dma_terminate_all() is called).
 *
 * Caller must hold dchan->vc.lock.
 */
static void fpga_sampler_dma_start_transfer(struct fpga_sampler_dma *dmac,
					    struct fpga_sampler_dma_chan *dchan)
{
	struct fpga_sampler_dma_desc *desc = dchan->desc;
	dma_addr_t dst;

	if (!desc)
		return;

	dst = desc->buf_addr + dchan->next_half * desc->period_len;

	/* TODO: wait for/confirm the sampler side has a half-buffer ready
	 * (equivalent of dma_sampler_buffer_is_ready() today - though with a
	 * real dmaengine split, this readiness signalling more properly
	 * belongs to the SPI-offload provider driver rather than here).
	 *
	 * TODO: iowrite32() the source address (LSRAM half offset), dst,
	 * desc->period_len and DMA_DESC_0_CONFIG into dmac->regs, then
	 * iowrite32(DMA_START_BIT_0, ...DMA_START_OPERATION_REG) to kick off
	 * the transfer - mirroring dma_sampler_start_transfer().
	 */

	dchan->next_half = !dchan->next_half;
}

/**
 * fpga_sampler_dma_issue_pending() - start processing queued descriptors
 * @chan: channel to kick
 *
 * Called by the dmaengine core (via dma_async_issue_pending()) after a
 * consumer has called dmaengine_submit() on one or more descriptors. Our
 * responsibility is to move any newly-submitted descriptors onto the
 * "issued" list (vchan_issue_pending() does this bookkeeping for us) and,
 * if the hardware is currently idle, pick up the next descriptor and
 * program the first period into hardware via
 * fpga_sampler_dma_start_transfer().
 *
 * Since this hardware only supports a single active transfer at a time,
 * if dchan->desc is already set we simply return - the newly issued
 * descriptor(s) will be picked up automatically once the current cyclic
 * transfer is terminated and the next one is issued.
 */
static void fpga_sampler_dma_issue_pending(struct dma_chan *chan)
{
	struct fpga_sampler_dma *dmac = to_fpga_sampler_dma(chan);
	struct fpga_sampler_dma_chan *dchan = to_fpga_sampler_dma_chan(chan);
	unsigned long flags;

	spin_lock_irqsave(&dchan->vc.lock, flags);

	if (vchan_issue_pending(&dchan->vc) && !dchan->desc) {
		struct virt_dma_desc *vd = vchan_next_desc(&dchan->vc);

		if (vd) {
			dchan->desc = to_fpga_sampler_dma_desc(vd);
			fpga_sampler_dma_start_transfer(dmac, dchan);
		}
	}

	spin_unlock_irqrestore(&dchan->vc.lock, flags);
}

/**
 * fpga_sampler_dma_terminate_all() - abort any in-progress transfer
 * @chan: channel to stop
 *
 * Called by the consumer (e.g. via dmaengine_terminate_sync()) to
 * unconditionally stop DMA activity, typically when an IIO buffer is being
 * disabled. Must stop the hardware from generating further completion
 * interrupts/transfers, drop our reference to the active descriptor, and
 * hand all queued/active/completed descriptors to
 * vchan_dma_desc_free_list() so they get freed via
 * fpga_sampler_dma_desc_free().
 *
 * Return: 0 on success, negative errno on failure.
 */
static int fpga_sampler_dma_terminate_all(struct dma_chan *chan)
{
	struct fpga_sampler_dma *dmac = to_fpga_sampler_dma(chan);
	struct fpga_sampler_dma_chan *dchan = to_fpga_sampler_dma_chan(chan);
	unsigned long flags;
	LIST_HEAD(head);

	spin_lock_irqsave(&dchan->vc.lock, flags);

	/* TODO: mask DMA_INTR_0_* and/or otherwise stop the hardware from
	 * starting/continuing any transfer, mirroring what
	 * dma_sampler_iio_dma_buffer_abort() does today.
	 */

	dchan->desc = NULL;
	vchan_get_all_descriptors(&dchan->vc, &head);

	spin_unlock_irqrestore(&dchan->vc.lock, flags);

	vchan_dma_desc_free_list(&dchan->vc, &head);

	return 0;
}

/**
 * fpga_sampler_dma_synchronize() - wait for termination to fully complete
 * @chan: channel to synchronize
 *
 * Called after fpga_sampler_dma_terminate_all() to block until any
 * in-flight IRQ handler/tasklet activity for this channel has finished, so
 * the consumer can safely free buffers afterwards. The virt-dma helper
 * vchan_synchronize() takes care of waiting for the completion tasklet;
 * if a hardware-specific "wait for DMA idle" step is ever needed (e.g.
 * polling a busy bit), add it here before calling vchan_synchronize().
 */
static void fpga_sampler_dma_synchronize(struct dma_chan *chan)
{
	struct fpga_sampler_dma_chan *dchan = to_fpga_sampler_dma_chan(chan);

	/* TODO: if needed, poll hardware for "DMA idle" here before
	 * synchronizing software state.
	 */

	vchan_synchronize(&dchan->vc);
}

/**
 * fpga_sampler_dma_tx_status() - report progress of a transfer
 * @chan:    channel being queried
 * @cookie:  transfer identifier returned by dmaengine_submit()
 * @txstate: output parameter to fill in with status/residue information
 *
 * Called by consumers (e.g. via dmaengine_tx_status()) to check whether a
 * transfer has completed, is still in progress, or errored, and how many
 * bytes remain ("residue"). For a purely cyclic, free-running transfer
 * like ours, residue reporting is typically approximate/best-effort;
 * dma_cookie_status() plus vchan's descriptor lists give the basic
 * DMA_COMPLETE / DMA_IN_PROGRESS / DMA_ERROR answer, which is sufficient to
 * start with.
 *
 * Return: the transfer's current dma_status.
 */
static enum dma_status fpga_sampler_dma_tx_status(struct dma_chan *chan,
						  dma_cookie_t cookie,
						  struct dma_tx_state *txstate)
{
	return dma_cookie_status(chan, cookie, txstate);
}

/**
 * fpga_sampler_dma_irq_handler() - DMA completion interrupt handler
 * @irq: interrupt number
 * @p:   struct fpga_sampler_dma * passed at devm_request_irq() time
 *
 * Fires once per completed period (PLIC source 121 on BeagleV-Fire, same
 * physical interrupt line used today in dma_sampler_irq_handler()). Must:
 *   1. Acknowledge/clear the interrupt in the FPGA DMA_INTR_0_* registers.
 *   2. Report the just-completed period to the dmaengine core via
 *      vchan_cyclic_callback(&desc->vd), which schedules the virt-dma
 *      tasklet that invokes the consumer's per-period callback (this is
 *      what eventually lets IIO's dmaengine buffer core notify userspace
 *      that new samples are available).
 *   3. If the cyclic descriptor is still active (i.e. not yet
 *      terminated), immediately program and start the *next* period via
 *      fpga_sampler_dma_start_transfer(), so the ping-pong capture keeps
 *      running back-to-back with no gaps - mirroring the unconditional
 *      "start next transfer" behavior in today's
 *      dma_sampler_irq_handler().
 *
 * Return: IRQ_HANDLED once the interrupt has been acknowledged and (if
 * applicable) the next period has been started.
 */
static irqreturn_t fpga_sampler_dma_irq_handler(int irq, void *p)
{
	struct fpga_sampler_dma *dmac = p;
	struct fpga_sampler_dma_chan *dchan = &dmac->chan;

	/* TODO: clear/ack DMA_INTR_0_* registers via dmac->regs. */

	spin_lock(&dchan->vc.lock);

	if (dchan->desc) {
		vchan_cyclic_callback(&dchan->desc->vd);
		fpga_sampler_dma_start_transfer(dmac, dchan);
	}

	spin_unlock(&dchan->vc.lock);

	return IRQ_HANDLED;
}

static int fpga_sampler_dma_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct fpga_sampler_dma *dmac;
	struct dma_device *dma_dev;
	int ret;

	dmac = devm_kzalloc(dev, sizeof(*dmac), GFP_KERNEL);
	if (!dmac)
		return -ENOMEM;

	platform_set_drvdata(pdev, dmac);

	dmac->regs = devm_ioremap(dev, FPGA_SAMPLER_DMA_ADDRESS,
				  FPGA_SAMPLER_DMA_SIZE);
	if (!dmac->regs)
		return dev_err_probe(dev, -EINVAL,
				     "failed to map dma registers\n");

	dmac->irq = platform_get_irq(pdev, 0);
	if (dmac->irq < 0)
		return dmac->irq;

	ret = devm_request_irq(dev, dmac->irq, fpga_sampler_dma_irq_handler,
			       0, dev_name(dev), dmac);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request irq\n");

	/*
	 * dma_dev capability mask + callback wiring: this is the standard
	 * dmaengine provider registration boilerplate. DMA_CYCLIC is the
	 * only capability we advertise, since that's the only transfer type
	 * this hardware (and dma_sampler's use case) needs.
	 */
	dma_dev = &dmac->dma_dev;
	dma_cap_set(DMA_CYCLIC, dma_dev->cap_mask);

	dma_dev->dev = dev;
	dma_dev->device_alloc_chan_resources = fpga_sampler_dma_alloc_chan_resources;
	dma_dev->device_free_chan_resources = fpga_sampler_dma_free_chan_resources;
	dma_dev->device_prep_dma_cyclic = fpga_sampler_dma_prep_dma_cyclic;
	dma_dev->device_issue_pending = fpga_sampler_dma_issue_pending;
	dma_dev->device_terminate_all = fpga_sampler_dma_terminate_all;
	dma_dev->device_synchronize = fpga_sampler_dma_synchronize;
	dma_dev->device_tx_status = fpga_sampler_dma_tx_status;

	INIT_LIST_HEAD(&dma_dev->channels);

	dmac->chan.vc.desc_free = fpga_sampler_dma_desc_free;
	vchan_init(&dmac->chan.vc, dma_dev);

	ret = dma_async_device_register(dma_dev);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to register dma device\n");

	/* TODO: register this device as a DMA controller for devicetree
	 * lookups, e.g. via of_dma_controller_register(), so that consumers
	 * can request our channel by name through the standard "dmas"/
	 * "dma-names" devicetree properties (see how spi-axi-spi-engine.c's
	 * offload glue requests its "offloadN-rx" channel for the pattern
	 * this driver's channel is meant to support).
	 */

	return 0;
}

static void fpga_sampler_dma_remove(struct platform_device *pdev)
{
	struct fpga_sampler_dma *dmac = platform_get_drvdata(pdev);

	/* TODO: of_dma_controller_free() counterpart, if registered above. */

	dma_async_device_unregister(&dmac->dma_dev);
}

static const struct of_device_id fpga_sampler_dma_of_match[] = {
	{ .compatible = "rohm,fpga-sampler-dma" },
	{ }
};
MODULE_DEVICE_TABLE(of, fpga_sampler_dma_of_match);

static struct platform_driver fpga_sampler_dma_driver = {
	.driver = {
		.name = "fpga-sampler-dma",
		.of_match_table = fpga_sampler_dma_of_match,
	},
	.probe = fpga_sampler_dma_probe,
	.remove = fpga_sampler_dma_remove,
};
module_platform_driver(fpga_sampler_dma_driver);

MODULE_DESCRIPTION("Skeleton dmaengine driver for the FPGA fabric DMA controller");
MODULE_LICENSE("GPL");
