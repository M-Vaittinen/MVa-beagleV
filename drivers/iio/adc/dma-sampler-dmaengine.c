// SPDX-License-Identifier: GPL-2.0
/*
 * IIO driver for the ROHM/BeagleV-Fire FPGA fabric ADC sampler, using the
 * generic Linux dmaengine API (against drivers/dma/fpga-sampler-dma.c)
 * instead of directly poking the FPGA DMA_* registers.
 *
 * This file is a DELIBERATE PARALLEL/ALTERNATIVE to dma-sampler.c, not a
 * replacement. dma-sampler.c drives the FPGA DMA controller registers
 * directly from within the IIO driver (custom iio_dma_buffer_ops::submit
 * calling dma_sampler_start_transfer() itself). This driver instead
 * requests a channel from the fpga-sampler-dma dmaengine provider via
 * dma_request_chan() and lets that separate driver own the DMA_* register
 * pokes, so the two concerns (ADC/SPI-sampler control vs. DMA transfer
 * mechanics) are split across two drivers as they would be for a "normal"
 * IIO ADC. See TODO-dmaengine-only-plan.txt for the design rationale and
 * pros/cons discussion, and TO-CLARIFY.txt for open items.
 *
 * IMPORTANT design note - why this driver does NOT use the generic
 * drivers/iio/buffer/industrialio-buffer-dmaengine.c helper:
 *
 * That helper (devm_iio_dmaengine_buffer_setup_with_handle() and friends)
 * assumes the DMA provider supports one-shot slave transfers
 * (device_prep_slave_sg() / device_prep_peripheral_dma_vec() style: one
 * descriptor per queued IIO buffer block, re-submitted block-by-block).
 * drivers/dma/fpga-sampler-dma.c intentionally only implements
 * DMA_CYCLIC (device_prep_dma_cyclic()) because the underlying FPGA
 * hardware is a free-running ping-pong producer, not something that can
 * be kicked one discrete block at a time. So the generic dmaengine IIO
 * buffer helper cannot be used as-is against this provider (it would
 * call a NULL device_prep_slave_sg hook and fail) - see TO-CLARIFY.txt.
 *
 * Instead, this driver:
 *   - uses a plain devm_iio_kfifo_buffer_setup() software buffer (like
 *     many simple IIO ADC drivers do for their own triggered-buffer
 *     mode), together with iio_push_to_buffers() to hand samples to
 *     userspace one scan at a time from the DMA completion path, and
 *   - drives the dmaengine channel directly with a single persistent
 *     dmaengine_prep_dma_cyclic() descriptor spanning the whole
 *     ping-pong buffer, submitted once in ->postenable and terminated in
 *     ->predisable, with a per-period dmaengine callback
 *     (fpga_sampler_dmaengine_period_done()) that walks the samples in
 *     the half that just completed and pushes them into the kfifo.
 *
 * This trades a bit of copy overhead (kfifo push per-sample rather than
 * mmap'd DMA buffer blocks) for a MUCH simpler buffer model that maps
 * naturally onto a single continuous cyclic descriptor - no attempt is
 * made here to reuse iio_dma_buffer_block / iio_dma_buffer_queue
 * (dma-sampler.c's model) since those assume the provider completes
 * discrete blocks, not periods of one long-lived cyclic transfer.
 *
 * UNTESTED: like fpga-sampler-dma.c, this file has not been compiled or
 * run; it is provided for design comparison against dma-sampler.c. See
 * TO-CLARIFY.txt for known open questions (still applicable here, in
 * particular the "half ready" signalling item, and the first-sample
 * discard question below).
 */

#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/io.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#include <linux/iio/buffer.h>
#include <linux/iio/iio.h>
#include <linux/iio/kfifo_buf.h>

/*
 * sampler bits and registers (identical to dma-sampler.c; the ADC/SPI
 * sampler control side of things is unchanged by this driver - only the
 * DMA transfer mechanism differs).
 */
#define SAMPLER_ADDRESS			0x60000000
#define SAMPLER_SIZE			0x1000

#define SAMPLER_LSRAM_WORD_COUNT	18432
#define SAMPLER_SAMPLES_PER_LSRAM_WORD	4
#define SAMPLER_LSRAM_SAMPLE_COUNT	(SAMPLER_LSRAM_WORD_COUNT *	\
					 SAMPLER_SAMPLES_PER_LSRAM_WORD)

#define SAMPLER_SAMPLE_BYTES		2
#define SAMPLER_BUFFER_BYTE_COUNT	(SAMPLER_LSRAM_SAMPLE_COUNT *	\
					 SAMPLER_SAMPLE_BYTES)
#define SAMPLER_BUFFER_BYTE_COUNT_HALF	(SAMPLER_BUFFER_BYTE_COUNT / 2)
#define SAMPLER_HALF_SAMPLE_COUNT	(SAMPLER_LSRAM_SAMPLE_COUNT / 2)

#define SAMPLER_CONTROL_KEY_BITS	((u64)0xadca5a5a << 32)
#define SAMPLER_CONTROL_CAPTURE_BIT	BIT(1)
#define SAMPLER_CONTROL_CONTINUOUS_BIT	BIT(4)

#define SAMPLER_CONTROL_REG		0x00
#define SAMPLER_CAPTURE_COUNT_REG	0x10
#define SAMPLER_SPI_RATE_SEL_REG	0x30
#define SAMPLER_SPI_TX_WORD_REG		0x38

struct dma_sampler_rate_preset {
	u32 selector;
	u32 sample_rate_sps;
};

static const struct dma_sampler_rate_preset dma_sampler_rate_presets[] = {
	{ 0, 10000 },
	{ 1, 100000 },
	{ 2, 250000 },
	{ 3, 500000 },
	{ 4, 1000000 },
};

static const int dma_sampler_sample_freq_avail[] = {
	10000, 100000, 250000, 500000, 1000000,
};

/*
 * struct fpga_sampler_dmaengine_state - driver-private state
 * @sampler_regs:  ioremap()'d sampler MMIO window (ADC/SPI control side,
 *                 same registers dma-sampler.c uses).
 * @dma_chan:      dmaengine channel obtained from the fpga-sampler-dma
 *                 provider via dma_request_chan(dev, "rx").
 * @dma_cookie:    cookie of the currently-submitted cyclic descriptor,
 *                 used only for dmaengine_terminate_sync()/bookkeeping.
 * @dma_vaddr:     CPU-side virtual address of the ping-pong buffer
 *                 allocated with dma_alloc_coherent().
 * @dma_paddr:     matching DMA/bus address, passed to
 *                 dmaengine_prep_dma_cyclic().
 * @dma_buf_len:   total ping-pong buffer length in bytes (2 halves).
 * @sample_rate_sps: currently configured SPI/ADC sample rate.
 * @half:          which half (0/1) the *next expected* completion
 *                 callback refers to; used purely for bookkeeping/
 *                 sanity-checking, since the dmaengine provider is
 *                 responsible for actually alternating ping-pong halves
 *                 in hardware.
 */
struct fpga_sampler_dmaengine_state {
	void __iomem *sampler_regs;

	struct dma_chan *dma_chan;
	dma_cookie_t dma_cookie;

	void *dma_vaddr;
	dma_addr_t dma_paddr;
	size_t dma_buf_len;

	u32 sample_rate_sps;
	unsigned int half;
};

#define BD79104_VOLTAGE_CHANNEL(num)					\
	{								\
		.type = IIO_VOLTAGE,					\
		.indexed = 1,						\
		.channel = (num),					\
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE) |	\
					    BIT(IIO_CHAN_INFO_SAMP_FREQ), \
		.info_mask_shared_by_type_available =			\
					    BIT(IIO_CHAN_INFO_SAMP_FREQ), \
		.scan_index = (num),					\
		.scan_type = {						\
			.sign = 'u',					\
			.realbits = 12,					\
			.storagebits = 16,				\
			.shift = 4,					\
			.endianness = IIO_BE,				\
		},							\
	}

static const struct iio_chan_spec dma_sampler_channels[] = {
	BD79104_VOLTAGE_CHANNEL(0),
	BD79104_VOLTAGE_CHANNEL(1),
	BD79104_VOLTAGE_CHANNEL(2),
	BD79104_VOLTAGE_CHANNEL(3),
	BD79104_VOLTAGE_CHANNEL(4),
	BD79104_VOLTAGE_CHANNEL(5),
	BD79104_VOLTAGE_CHANNEL(6),
	BD79104_VOLTAGE_CHANNEL(7),
};

/*
 * Only one channel may be enabled at a time - same hardware limitation
 * as dma-sampler.c (single SPI_TX_WORD register).
 */
static const unsigned long dma_sampler_available_scan_masks[] = {
	BIT(0), BIT(1), BIT(2), BIT(3),
	BIT(4), BIT(5), BIT(6), BIT(7),
	0
};

static inline void dma_sampler_fpga_write(struct fpga_sampler_dmaengine_state *st,
					  unsigned int reg, unsigned int val,
					  bool flush)
{
	iowrite64(SAMPLER_CONTROL_KEY_BITS | val, st->sampler_regs + reg);
	if (flush)
		(void)ioread64(st->sampler_regs + reg);
}

static int dma_sampler_rate_to_selector(int sample_rate_sps, u32 *selector)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(dma_sampler_rate_presets); i++) {
		if (dma_sampler_rate_presets[i].sample_rate_sps == sample_rate_sps) {
			*selector = dma_sampler_rate_presets[i].selector;
			return 0;
		}
	}

	return -EINVAL;
}

static int bd79104_read_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *channel, int *val,
			    int *val2, long mask)
{
	struct fpga_sampler_dmaengine_state *st = iio_priv(indio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_SCALE:
		*val = 3300;
		*val2 = 12;
		return IIO_VAL_FRACTIONAL_LOG2;

	case IIO_CHAN_INFO_SAMP_FREQ:
		*val = st->sample_rate_sps;
		return IIO_VAL_INT;

	default:
		return -EINVAL;
	}
}

static int bd79104_write_raw(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *channel, int val,
			     int val2, long mask)
{
	struct fpga_sampler_dmaengine_state *st = iio_priv(indio_dev);
	u32 selector;
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_SAMP_FREQ:
		ret = dma_sampler_rate_to_selector(val, &selector);
		if (ret)
			return ret;

		ret = iio_device_claim_direct_mode(indio_dev);
		if (ret)
			return ret;

		dma_sampler_fpga_write(st, SAMPLER_SPI_RATE_SEL_REG, selector,
				       true);
		st->sample_rate_sps = val;

		iio_device_release_direct_mode(indio_dev);
		return 0;

	default:
		return -EINVAL;
	}
}

static int bd79104_read_avail(struct iio_dev *indio_dev,
			      struct iio_chan_spec const *channel,
			      const int **vals, int *type, int *length,
			      long mask)
{
	switch (mask) {
	case IIO_CHAN_INFO_SAMP_FREQ:
		*vals = dma_sampler_sample_freq_avail;
		*type = IIO_VAL_INT;
		*length = ARRAY_SIZE(dma_sampler_sample_freq_avail);
		return IIO_AVAIL_LIST;

	default:
		return -EINVAL;
	}
}

static const struct iio_info dma_sampler_info = {
	.read_raw = bd79104_read_raw,
	.write_raw = bd79104_write_raw,
	.read_avail = bd79104_read_avail,
};

/*
 * NOTE: same caveat as dma-sampler.c - the exact SPI command-word bit
 * layout is unverified; assumed to be the raw channel index.
 */
static inline u16 dma_sampler_channel_cmd(unsigned int chan)
{
	return (u16)chan;
}

static void dma_sampler_meas_ctrl(struct fpga_sampler_dmaengine_state *st,
				  bool start)
{
	if (start) {
		mmiowb();
		dma_sampler_fpga_write(st, SAMPLER_CONTROL_REG,
				       SAMPLER_CONTROL_CAPTURE_BIT |
				       SAMPLER_CONTROL_CONTINUOUS_BIT, true);
	} else {
		dma_sampler_fpga_write(st, SAMPLER_CONTROL_REG, 0, false);
		mmiowb();
	}
}

/**
 * fpga_sampler_dmaengine_period_done() - dmaengine per-period callback
 * @param: struct iio_dev * passed as the callback's private data
 *
 * Registered as the callback of the single persistent
 * dmaengine_prep_dma_cyclic() descriptor (see
 * fpga_sampler_dmaengine_postenable()). Invoked by the dmaengine
 * provider (fpga-sampler-dma.c, via vchan_cyclic_callback()) once per
 * completed ping-pong half, i.e. exactly the same event that drives
 * dma_sampler_irq_handler() -> iio_dma_buffer_block_done() in
 * dma-sampler.c.
 *
 * Unlike dma-sampler.c (which hands whole DMA-mapped blocks to
 * userspace via mmap), this driver pushes samples one scan at a time
 * into a software kfifo via iio_push_to_buffers(), which is simpler but
 * costs an extra copy per sample. This is an acceptable trade-off for a
 * design-comparison driver; a production version could instead use
 * iio_push_to_buffers_with_ts() plus a real DMA-buffer-backed IIO
 * buffer if the copy overhead turns out to matter.
 *
 * TODO (same open question as dma-sampler.c / TODO-dmaengine-only-plan.txt):
 * the very first sample of a half may still reflect the *previous*
 * SPI_TX_WORD channel command (channel-select and data-read are
 * pipelined one SPI transaction apart), and this callback does not yet
 * discard it. Needs verification against the actual gateware behaviour.
 */
static void fpga_sampler_dmaengine_period_done(void *param)
{
	struct iio_dev *indio_dev = param;
	struct fpga_sampler_dmaengine_state *st = iio_priv(indio_dev);
	void *half_start;
	unsigned int i;

	half_start = st->dma_vaddr + st->half * SAMPLER_BUFFER_BYTE_COUNT_HALF;

	for (i = 0; i < SAMPLER_HALF_SAMPLE_COUNT; i++) {
		const void *sample = half_start + i * SAMPLER_SAMPLE_BYTES;

		iio_push_to_buffers(indio_dev, sample);
	}

	st->half = !st->half;
}

/**
 * fpga_sampler_dmaengine_postenable() - iio_buffer_setup_ops::postenable
 * @indio_dev: device whose buffer is being enabled
 *
 * Counterpart of dma_sampler_iio_buffer_enable() in dma-sampler.c, but
 * using dmaengine instead of hand-rolled register access:
 *   1. Program the SPI command word for the single enabled channel.
 *   2. Allocate a coherent ping-pong buffer sized to exactly 2 LSRAM
 *      halves (matching the geometry fpga_sampler_dma_prep_dma_cyclic()
 *      requires - see fpga-sampler-dma.c).
 *   3. Build ONE cyclic descriptor spanning the whole ping-pong buffer
 *      with dmaengine_prep_dma_cyclic(), wire up the per-period
 *      callback, submit it, and kick it off with
 *      dma_async_issue_pending(). The dmaengine provider then owns
 *      alternating the two halves in hardware and firing the callback
 *      after each one, for as long as the descriptor remains active.
 *   4. Only then start the FPGA capture (SAMPLER_CONTROL_REG), mirroring
 *      dma-sampler.c's ordering of "arm DMA before/around starting
 *      capture".
 *
 * Return: 0 on success, negative errno on failure (in which case IIO
 * core will not proceed to enable the buffer).
 */
static int fpga_sampler_dmaengine_postenable(struct iio_dev *indio_dev)
{
	struct fpga_sampler_dmaengine_state *st = iio_priv(indio_dev);
	struct dma_async_tx_descriptor *desc;
	struct device *dev = indio_dev->dev.parent;
	unsigned int chan;
	dma_cookie_t cookie;

	chan = find_first_bit(indio_dev->active_scan_mask,
			      indio_dev->masklength);

	dma_sampler_meas_ctrl(st, false);
	dma_sampler_fpga_write(st, SAMPLER_SPI_TX_WORD_REG,
			       dma_sampler_channel_cmd(chan), false);

	st->half = 0;
	st->dma_buf_len = SAMPLER_BUFFER_BYTE_COUNT;
	st->dma_vaddr = dma_alloc_coherent(dev, st->dma_buf_len,
					   &st->dma_paddr, GFP_KERNEL);
	if (!st->dma_vaddr)
		return -ENOMEM;

	desc = dmaengine_prep_dma_cyclic(st->dma_chan, st->dma_paddr,
					 st->dma_buf_len,
					 SAMPLER_BUFFER_BYTE_COUNT_HALF,
					 DMA_DEV_TO_MEM, 0);
	if (!desc) {
		dma_free_coherent(dev, st->dma_buf_len, st->dma_vaddr,
				  st->dma_paddr);
		st->dma_vaddr = NULL;
		return -EIO;
	}

	desc->callback = fpga_sampler_dmaengine_period_done;
	desc->callback_param = indio_dev;

	cookie = dmaengine_submit(desc);
	if (dma_submit_error(cookie)) {
		dma_free_coherent(dev, st->dma_buf_len, st->dma_vaddr,
				  st->dma_paddr);
		st->dma_vaddr = NULL;
		return cookie;
	}
	st->dma_cookie = cookie;

	dma_async_issue_pending(st->dma_chan);

	dma_sampler_meas_ctrl(st, true);

	return 0;
}

/**
 * fpga_sampler_dmaengine_predisable() - iio_buffer_setup_ops::predisable
 * @indio_dev: device whose buffer is being disabled
 *
 * Counterpart of postenable(): stop the FPGA capture first, then
 * terminate the cyclic descriptor (dmaengine_terminate_sync() blocks
 * until the provider has genuinely stopped and any in-flight callback
 * has finished, so it is safe to free the coherent buffer immediately
 * afterwards).
 */
static int fpga_sampler_dmaengine_predisable(struct iio_dev *indio_dev)
{
	struct fpga_sampler_dmaengine_state *st = iio_priv(indio_dev);
	struct device *dev = indio_dev->dev.parent;

	dma_sampler_meas_ctrl(st, false);

	dmaengine_terminate_sync(st->dma_chan);

	if (st->dma_vaddr) {
		dma_free_coherent(dev, st->dma_buf_len, st->dma_vaddr,
				  st->dma_paddr);
		st->dma_vaddr = NULL;
	}

	return 0;
}

static const struct iio_buffer_setup_ops fpga_sampler_dmaengine_buffer_setup_ops = {
	.postenable = fpga_sampler_dmaengine_postenable,
	.predisable = fpga_sampler_dmaengine_predisable,
};

static int fpga_sampler_dmaengine_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct iio_dev *indio_dev;
	struct fpga_sampler_dmaengine_state *st;
	u32 spi_rate_sel;
	int ret;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*st));
	if (!indio_dev)
		return -ENOMEM;

	st = iio_priv(indio_dev);

	indio_dev->name = "sampler-dmaengine";
	indio_dev->modes = INDIO_BUFFER_SOFTWARE;
	indio_dev->info = &dma_sampler_info;
	indio_dev->channels = dma_sampler_channels;
	indio_dev->num_channels = ARRAY_SIZE(dma_sampler_channels);
	indio_dev->available_scan_masks = dma_sampler_available_scan_masks;

	st->sampler_regs = devm_ioremap(dev, SAMPLER_ADDRESS, SAMPLER_SIZE);
	if (!st->sampler_regs)
		return dev_err_probe(dev, -EINVAL,
				     "failed to map sampler registers\n");

	/*
	 * Requests the "rx" dma channel named in this node's "dmas"/
	 * "dma-names" device-tree properties (see mva-ext-dtso/
	 * iio-mem-access-overlay.dtso's `fpgasampler` node), which resolves
	 * to the fpga-sampler-dma provider via of_dma_simple_xlate().
	 */
	st->dma_chan = dma_request_chan(dev, "rx");
	if (IS_ERR(st->dma_chan))
		return dev_err_probe(dev, PTR_ERR(st->dma_chan),
				     "failed to request dma channel\n");

	/* Ensure sampler is not running before we start configuring it. */
	dma_sampler_meas_ctrl(st, false);

	ret = devm_iio_kfifo_buffer_setup(dev, indio_dev,
					  &fpga_sampler_dmaengine_buffer_setup_ops);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to setup kfifo buffer\n");

	/* set sample count */
	dma_sampler_fpga_write(st, SAMPLER_CAPTURE_COUNT_REG,
			       SAMPLER_LSRAM_SAMPLE_COUNT, false);

	/* pick a sane default sample rate; userspace can change it later
	 * via the sampling_frequency sysfs attribute (buffer must be
	 * disabled at that time)
	 */
	st->sample_rate_sps = 100000;
	ret = dma_sampler_rate_to_selector(st->sample_rate_sps, &spi_rate_sel);
	if (ret)
		return dev_err_probe(dev, ret,
				     "invalid default sample rate\n");

	dma_sampler_fpga_write(st, SAMPLER_SPI_RATE_SEL_REG, spi_rate_sel,
			       false);

	return devm_iio_device_register(dev, indio_dev);
}

static const struct of_device_id dma_sampler_dmaengine_of_match[] = {
	{ .compatible = "rohm,sampler-dmaengine" },
	{ }
};
MODULE_DEVICE_TABLE(of, dma_sampler_dmaengine_of_match);

static const struct platform_device_id dma_sampler_dmaengine_devices_ids[] = {
	{ .name = "sampler-dmaengine" },
	{ }
};
MODULE_DEVICE_TABLE(platform, dma_sampler_dmaengine_devices_ids);

static struct platform_driver dma_sampler_dmaengine_driver = {
	.driver = {
		.name = "sampler-dmaengine",
		.of_match_table = dma_sampler_dmaengine_of_match,
	},
	.probe = fpga_sampler_dmaengine_probe,
	.id_table = dma_sampler_dmaengine_devices_ids,
};
module_platform_driver(dma_sampler_dmaengine_driver);

MODULE_LICENSE("GPL");
