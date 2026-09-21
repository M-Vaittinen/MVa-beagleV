/* SPDX-License-Identifier: GPL-2.0
 *
 * adc_capture - simple userspace test/demo tool that captures ADC samples
 * from the BeagleV-Fire FPGA sampler via libIIO and stores them, in raw
 * binary form, into a capture file prefixed by a small fixed-size header
 * describing how the capture was configured.
 *
 * This is a standalone command line tool, independent of libiio_wrapper.c
 * (which is used by the Python/Qt UI) - it talks to libIIO directly so it
 * can be run headless for quick captures/regression testing while
 * developing/comparing the "dma-sampler" and "dma-sampler-dmaengine" IIO
 * drivers.
 *
 * Usage:
 *   adc_capture -c <channel> -f <sampling_frequency_hz> -n <num_buffers>
 *               -o <output_file> [-u <iio_uri>] [-d <iio_device_name>]
 *
 * The IIO device is expected to expose:
 *   - a set of indexed voltage input channels (voltage0, voltage1, ...),
 *     of which exactly one is enabled for this single-channel capture
 *     (matching the "one channel at a time" restriction shared by both
 *     dma-sampler.c and dma-sampler-dmaengine.c: the FPGA sampler only has
 *     a single SPI_TX_WORD register to select the sampled ADC channel),
 *   - a shared-by-type IIO_CHAN_INFO_SAMP_FREQ attribute
 *     ("sampling_frequency"), settable while the buffer is disabled.
 *
 * The capture file format is:
 *
 *   [ struct capture_file_header ]  (fixed size, see below)
 *   [ raw samples, native channel storage format/endianness, back-to-back ]
 *
 * The header records enough information (format version, sample rate,
 * channel index/id, sample storage width, total sample count, ping-pong
 * buffer geometry used for the capture) that a separate analysis tool can
 * parse the raw sample stream unambiguously later, and so the header
 * format itself can be evolved (see CAPTURE_HEADER_VERSION) without
 * breaking tools that only understand older versions - readers should
 * always check capture_file_header.version before interpreting any
 * fields beyond magic/version/header_size.
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <iio/iio.h>

#include "capture_format.h"

/*
 * ---------------------------------------------------------------------
 * Command line handling
 * ---------------------------------------------------------------------
 */
struct capture_config {
	const char *uri;
	const char *device_name;
	const char *output_path;
	unsigned int channel_index;
	unsigned int sample_rate_sps;
	unsigned int num_buffers;
	uint64_t sample_count;	/* 0 means "run until interrupted" */
};

static void print_usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s -c <channel> -f <sampling_frequency_hz>\n"
		"          -n <num_ping_pong_buffers> -o <output_file>\n"
		"          [-s <sample_count>] [-u <iio_uri>]\n"
		"          [-d <iio_device_name>]\n"
		"       %s -h | --help\n"
		"\n"
		"  -c <channel>    IIO voltage channel index to enable (0-based)\n"
		"  -f <freq_hz>    ADC sampling frequency in samples per second;\n"
		"                  must match one of the values advertised by\n"
		"                  the device's sampling_frequency_available\n"
		"                  attribute\n"
		"  -n <count>      number of ping-pong (iio_block) buffers to\n"
		"                  request from libIIO for the capture stream\n"
		"  -o <file>       output capture file path\n"
		"  -s <count>      total number of samples to capture before\n"
		"                  stopping (default/0: run until Ctrl-C)\n"
		"  -u <uri>        libIIO context URI (default: \"local:\")\n"
		"  -d <name>       IIO device name/id to use (default: first\n"
		"                  device found in the context)\n"
		"  -h, --help      print this help text and exit\n"
		"\n"
		"Example:\n"
		"  %s -c 0 -f 100000 -n 4 -o capture0.bin\n"
		"      Capture channel 0 at 100000 samples/second using 4\n"
		"      ping-pong buffers, writing the result to capture0.bin\n"
		"      (Ctrl-C to stop, since -s was not given).\n",
		prog, prog, prog);
}

static int parse_args(int argc, char **argv, struct capture_config *cfg)
{
	static const struct option long_options[] = {
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	int opt;

	memset(cfg, 0, sizeof(*cfg));
	cfg->uri = "local:";
	cfg->device_name = NULL;
	cfg->num_buffers = 4;

	while ((opt = getopt_long(argc, argv, "c:f:n:o:s:u:d:h",
				  long_options, NULL)) != -1) {
		switch (opt) {
		case 'c':
			cfg->channel_index = (unsigned int)strtoul(optarg, NULL, 0);
			break;
		case 'f':
			cfg->sample_rate_sps = (unsigned int)strtoul(optarg, NULL, 0);
			break;
		case 'n':
			cfg->num_buffers = (unsigned int)strtoul(optarg, NULL, 0);
			break;
		case 'o':
			cfg->output_path = optarg;
			break;
		case 's':
			cfg->sample_count = strtoull(optarg, NULL, 0);
			break;
		case 'u':
			cfg->uri = optarg;
			break;
		case 'd':
			cfg->device_name = optarg;
			break;
		case 'h':
			print_usage(argv[0]);
			exit(EXIT_SUCCESS);
		default:
			print_usage(argv[0]);
			return -EINVAL;
		}
	}

	if (!cfg->output_path || !cfg->sample_rate_sps || !cfg->num_buffers) {
		fprintf(stderr,
			"error: -o, -f and -n are mandatory (and -n must be > 0)\n");
		print_usage(argv[0]);
		return -EINVAL;
	}

	return 0;
}

/*
 * ---------------------------------------------------------------------
 * Capture state
 * ---------------------------------------------------------------------
 */
#define SAMPLES_PER_BLOCK	36864	/* matches libiio_wrapper.c's choice */

struct capture_state {
	struct iio_context *ctx;
	struct iio_device *dev;
	struct iio_channel *chan;
	struct iio_channels_mask *mask;
	struct iio_buffer *buffer;
	struct iio_stream *stream;
	FILE *out;
};

static volatile sig_atomic_t stop_requested;

static void on_sigint(int signum)
{
	(void)signum;
	stop_requested = 1;
}

static void capture_state_cleanup(struct capture_state *st)
{
	if (st->stream)
		iio_stream_destroy(st->stream);
	if (st->chan && st->mask)
		iio_channel_disable(st->chan, st->mask);
	if (st->mask)
		iio_channels_mask_destroy(st->mask);
	if (st->ctx)
		iio_context_destroy(st->ctx);
	if (st->out)
		fclose(st->out);
}

static int set_sampling_frequency(struct iio_channel *chan,
				  unsigned int sample_rate_sps)
{
	const struct iio_attr *attr;
	ssize_t ret;

	attr = iio_channel_find_attr(chan, "sampling_frequency");
	if (!attr) {
		fprintf(stderr,
			"error: channel has no 'sampling_frequency' attribute\n");
		return -ENOENT;
	}

	ret = iio_attr_write(attr, (long long)sample_rate_sps);
	if (ret < 0) {
		fprintf(stderr,
			"error: failed to set sampling_frequency to %u: %s\n",
			sample_rate_sps, strerror((int)-ret));
		return (int)ret;
	}

	return 0;
}

static int write_header_placeholder(FILE *out)
{
	struct capture_file_header zero = { 0 };

	/* Reserve space for the header now; it is rewritten with the real
	 * values (including the final sample_count) once the capture is
	 * complete, so that the file remains parseable even if this
	 * placeholder is somehow left in place (e.g. the process is killed
	 * before the real header can be written back - readers will at
	 * least see magic==0, which is clearly not CAPTURE_HEADER_MAGIC,
	 * and can reject the file instead of misinterpreting it).
	 */
	if (fwrite(&zero, sizeof(zero), 1, out) != 1)
		return -EIO;

	return 0;
}

static int rewrite_header(FILE *out, const struct capture_file_header *hdr)
{
	if (fseek(out, 0, SEEK_SET) != 0)
		return -errno;

	if (fwrite(hdr, sizeof(*hdr), 1, out) != 1)
		return -EIO;

	if (fflush(out) != 0)
		return -errno;

	return 0;
}

int main(int argc, char **argv)
{
	struct capture_config cfg;
	struct capture_state st = { 0 };
	struct capture_file_header hdr = { 0 };
	const struct iio_data_format *format;
	unsigned int devices_count;
	unsigned int dev_index;
	uint64_t total_samples = 0;
	int ret;

	ret = parse_args(argc, argv, &cfg);
	if (ret)
		return EXIT_FAILURE;

	signal(SIGINT, on_sigint);
	signal(SIGTERM, on_sigint);

	/*
	 * connect
	 */
	st.ctx = iio_create_context(NULL, cfg.uri);
	if (iio_err(st.ctx)) {
		fprintf(stderr, "error: failed to create IIO context '%s': %s\n",
			cfg.uri, strerror(iio_err(st.ctx)));
		return EXIT_FAILURE;
	}

	/*
	 * find device
	 */
	if (cfg.device_name) {
		st.dev = iio_context_find_device(st.ctx, cfg.device_name);
		if (!st.dev) {
			fprintf(stderr, "error: device '%s' not found\n",
				cfg.device_name);
			goto err;
		}
	} else {
		devices_count = iio_context_get_devices_count(st.ctx);
		if (devices_count == 0) {
			fprintf(stderr, "error: no IIO devices found in context\n");
			goto err;
		}
		dev_index = 0;
		st.dev = iio_context_get_device(st.ctx, dev_index);
		if (!st.dev) {
			fprintf(stderr, "error: failed to get device %u\n",
				dev_index);
			goto err;
		}
		fprintf(stderr, "info: using device '%s'\n",
			iio_device_get_name(st.dev) ?
			iio_device_get_name(st.dev) : iio_device_get_id(st.dev));
	}

	/*
	 * get buffer (index 0, matches libiio_wrapper.c's usage)
	 */
	st.buffer = iio_device_get_buffer(st.dev, 0);
	if (!st.buffer) {
		fprintf(stderr, "error: device has no buffer\n");
		goto err;
	}

	/*
	 * create channels mask and enable the requested channel
	 */
	st.mask = iio_create_channels_mask(iio_device_get_channels_count(st.dev));
	if (!st.mask) {
		fprintf(stderr, "error: failed to allocate channels mask\n");
		goto err;
	}

	st.chan = iio_device_get_channel(st.dev, cfg.channel_index);
	if (!st.chan) {
		fprintf(stderr, "error: channel index %u not found\n",
			cfg.channel_index);
		goto err;
	}

	/* Sampling frequency must be set while the channel/buffer is not
	 * yet enabled - both dma-sampler.c and dma-sampler-dmaengine.c
	 * reject IIO_CHAN_INFO_SAMP_FREQ writes unless
	 * iio_device_claim_direct_mode() succeeds, i.e. while no buffer is
	 * currently enabled.
	 */
	ret = set_sampling_frequency(st.chan, cfg.sample_rate_sps);
	if (ret)
		goto err;

	iio_channel_enable(st.chan, st.mask);

	format = iio_channel_get_data_format(st.chan);
	if (!format) {
		fprintf(stderr, "error: failed to get channel data format\n");
		goto err;
	}

	/*
	 * open output file and reserve header space
	 */
	st.out = fopen(cfg.output_path, "wb");
	if (!st.out) {
		fprintf(stderr, "error: failed to open '%s' for writing: %s\n",
			cfg.output_path, strerror(errno));
		goto err;
	}

	ret = write_header_placeholder(st.out);
	if (ret) {
		fprintf(stderr, "error: failed to write header placeholder: %s\n",
			strerror(-ret));
		goto err;
	}

	/*
	 * create stream: cfg.num_buffers is the "amount of ping-pong
	 * buffers" input parameter.
	 */
	st.stream = iio_buffer_create_stream(st.buffer, cfg.num_buffers,
					     SAMPLES_PER_BLOCK, st.mask);
	if (iio_err(st.stream)) {
		fprintf(stderr, "error: failed to create IIO stream: %s\n",
			strerror(iio_err(st.stream)));
		st.stream = NULL;
		goto err;
	}

	fprintf(stderr,
		"info: capturing channel '%s' (index %u) at %u sps into '%s'\n"
		"      (%u ping-pong buffers x %u samples, %u-bit samples)\n",
		iio_channel_get_id(st.chan), cfg.channel_index,
		cfg.sample_rate_sps, cfg.output_path, cfg.num_buffers,
		SAMPLES_PER_BLOCK, format->length);

	/*
	 * capture loop: pull blocks from the stream and append the raw
	 * bytes belonging to our single enabled channel to the output
	 * file, until either the requested sample_count is reached (if
	 * non-zero) or the user interrupts us (Ctrl-C / SIGTERM).
	 */
	while (!stop_requested) {
		const struct iio_block *block;
		void *first, *end;
		size_t bytes, count;

		block = iio_stream_get_next_block(st.stream);
		if (iio_err(block)) {
			fprintf(stderr, "error: failed to get next block: %s\n",
				strerror(iio_err(block)));
			goto err;
		}

		first = iio_block_first(block, st.chan);
		end = iio_block_end(block);
		bytes = (size_t)((char *)end - (char *)first);
		count = bytes / (format->length / 8);

		if (cfg.sample_count && total_samples + count > cfg.sample_count) {
			/* Truncate the last block so the file contains
			 * exactly cfg.sample_count samples. */
			count = (size_t)(cfg.sample_count - total_samples);
			bytes = count * (format->length / 8);
		}

		if (bytes && fwrite(first, 1, bytes, st.out) != bytes) {
			fprintf(stderr, "error: short write to '%s': %s\n",
				cfg.output_path, strerror(errno));
			goto err;
		}

		total_samples += count;

		if (cfg.sample_count && total_samples >= cfg.sample_count)
			break;
	}

	fprintf(stderr, "info: capture stopped, %" PRIu64 " samples written\n",
		total_samples);

	/*
	 * fill in and (re)write the real header now that we know the
	 * final sample count.
	 */
	hdr.magic = CAPTURE_HEADER_MAGIC;
	hdr.version = CAPTURE_HEADER_VERSION;
	hdr.header_size = sizeof(hdr);
	hdr.sample_rate_sps = cfg.sample_rate_sps;
	hdr.channel_index = cfg.channel_index;
	strncpy(hdr.channel_id, iio_channel_get_id(st.chan),
		sizeof(hdr.channel_id) - 1);
	hdr.sample_bits = format->bits;
	hdr.storage_bits = format->length;
	hdr.is_signed = format->is_signed;
	hdr.is_big_endian = format->is_be;
	hdr.num_ping_pong_buffers = cfg.num_buffers;
	hdr.samples_per_buffer = SAMPLES_PER_BLOCK;
	hdr.sample_count = total_samples;

	ret = rewrite_header(st.out, &hdr);
	if (ret) {
		fprintf(stderr, "error: failed to write final header: %s\n",
			strerror(-ret));
		goto err;
	}

	capture_state_cleanup(&st);
	return EXIT_SUCCESS;

err:
	capture_state_cleanup(&st);
	return EXIT_FAILURE;
}
