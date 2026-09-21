/* SPDX-License-Identifier: GPL-2.0
 *
 * adc_direct_capture - like adc_capture.c, but with NO libIIO dependency:
 * this tool talks directly to the Linux IIO subsystem's sysfs ABI
 * (/sys/bus/iio/devices/iio:deviceN/...) and reads raw samples straight
 * from the IIO buffer character device (/dev/iio:deviceN), instead of
 * linking against libiio.
 *
 * This is only possible/sensible because this tool is meant to always
 * run locally on the BeagleV-Fire target itself (unlike libiio, which
 * also supports capturing over the network from a remote host) - see
 * adc_capture.c's header comment for the libIIO-based equivalent.
 *
 * Usage:
 *   adc_direct_capture -c <channel> -f <sampling_frequency_hz>
 *                       -n <num_buffers> -o <output_file>
 *                       [-d <iio_device_name>]
 *
 * The capture file produced is BYTE-FOR-BYTE compatible with the one
 * produced by adc_capture.c: same struct capture_file_header (see
 * capture_format.h) followed by raw samples, so adc_capture_view.c works
 * unmodified on files produced by either tool.
 *
 * ---------------------------------------------------------------------
 * IIO sysfs ABI used (see Linux kernel Documentation/ABI/testing/sysfs-bus-iio*
 * and drivers/iio/industrialio-{core,buffer}.c in this tree for the
 * authoritative naming rules this tool relies on):
 *
 *   /sys/bus/iio/devices/iio:deviceN/name
 *       device name, used to find "dma-sampler"/"dma-sampler-dmaengine"
 *       (or a user-specified -d name) among all iio:deviceN entries.
 *
 *   /sys/bus/iio/devices/iio:deviceN/in_voltage_sampling_frequency
 *       IIO_CHAN_INFO_SAMP_FREQ, IIO_SHARED_BY_TYPE attribute (shared by
 *       all "voltage" type channels on this device, hence no channel
 *       index in the filename) - see BD79104_VOLTAGE_CHANNEL() in
 *       dma-sampler.c/dma-sampler-dmaengine.c.
 *
 *   /sys/bus/iio/devices/iio:deviceN/scan_elements/in_voltage<C>_en
 *       Write '1'/'0' to enable/disable channel <C> for buffered
 *       capture. Only one channel may be enabled at a time for this
 *       hardware (see dma_sampler_available_scan_masks[]).
 *
 *   /sys/bus/iio/devices/iio:deviceN/scan_elements/in_voltage<C>_index
 *       The channel's scan_index (informational only here).
 *
 *   /sys/bus/iio/devices/iio:deviceN/scan_elements/in_voltage<C>_type
 *       Sample storage format string, e.g. "be:u12/16>>4" meaning
 *       big-endian, unsigned, 12 significant bits, 16 storage bits,
 *       right-shift 4. Parsed by parse_scan_type() below to learn the
 *       storage width/signedness/endianness/shift needed to both size
 *       our raw reads correctly and to fill in the capture file header.
 *
 *   /sys/bus/iio/devices/iio:deviceN/buffer/length
 *       Number of scan elements (samples) the kernel-side IIO buffer
 *       should hold; used here as a stand-in for "how many samples fit
 *       in one of our ping-pong halves" - see SAMPLES_PER_BLOCK.
 *
 *   /sys/bus/iio/devices/iio:deviceN/buffer/enable
 *       Write '1' to start buffered capture (after configuring
 *       sampling_frequency/scan_elements/length above), '0' to stop.
 *
 *   /dev/iio:deviceN
 *       Character device; read() returns raw, back-to-back scan
 *       elements in the enabled channels' native storage format - for
 *       our single-channel-only capture this is simply a stream of
 *       storage_bits-wide raw samples, which is exactly what this tool
 *       writes (unmodified) to the capture file.
 *
 * "num_buffers" (the -n option) does not have a direct sysfs equivalent
 * the way libiio's iio_buffer_create_stream() nb_blocks parameter does;
 * here it is used purely to size our userspace read buffer (num_buffers
 * x SAMPLES_PER_BLOCK samples per read() call) and is recorded in the
 * capture file header for consistency with adc_capture.c's output, but
 * does not otherwise affect kernel-side buffering (only "buffer/length"
 * does that).
 */

#include <ctype.h>
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

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "capture_format.h"

#define IIO_SYSFS_ROOT		"/sys/bus/iio/devices"
#define IIO_DEV_NODE_DIR	"/dev"

#define SAMPLES_PER_BLOCK	36864	/* matches adc_capture.c's choice */

struct capture_config {
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
		"          [-s <sample_count>] [-d <iio_device_name>]\n"
		"       %s -h | --help\n"
		"\n"
		"  -c <channel>    IIO voltage channel index to enable (0-based)\n"
		"  -f <freq_hz>    ADC sampling frequency in samples per second;\n"
		"                  must match one of the values advertised by\n"
		"                  the device's sampling_frequency_available\n"
		"                  attribute\n"
		"  -n <count>      number of ping-pong buffers worth of samples\n"
		"                  to read from the IIO char device per read()\n"
		"                  call (sizes our userspace read buffer; the\n"
		"                  kernel-side buffer/length is set separately,\n"
		"                  see SAMPLES_PER_BLOCK in the source)\n"
		"  -o <file>       output capture file path\n"
		"  -s <count>      total number of samples to capture before\n"
		"                  stopping (default/0: run until Ctrl-C)\n"
		"  -d <name>       IIO device name to use (default: first\n"
		"                  \"dma-sampler\"-like device found under\n"
		"                  " IIO_SYSFS_ROOT ")\n"
		"  -h, --help      print this help text and exit\n"
		"\n"
		"NOTE: unlike adc_capture (which uses libIIO and can talk to a\n"
		"remote target over the network), this tool only works when run\n"
		"directly on the BeagleV-Fire target, since it accesses\n"
		"" IIO_SYSFS_ROOT " and " IIO_DEV_NODE_DIR "/iio:deviceN directly.\n"
		"\n"
		"Example:\n"
		"  %s -c 0 -f 100000 -n 4 -o capture0.bin\n"
		"      Capture channel 0 at 100000 samples/second using a\n"
		"      4-block-sized read buffer, writing the result to\n"
		"      capture0.bin (Ctrl-C to stop, since -s was not given).\n",
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
	cfg->device_name = NULL;
	cfg->num_buffers = 4;

	while ((opt = getopt_long(argc, argv, "c:f:n:o:s:d:h",
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
 * sysfs helpers
 * ---------------------------------------------------------------------
 */

/* Read a whole sysfs attribute file into 'buf' (size 'buf_size'),
 * stripping a single trailing newline if present. Returns 0 on success,
 * negative errno on failure. */
static int sysfs_read_string(const char *path, char *buf, size_t buf_size)
{
	FILE *f;
	size_t len;

	f = fopen(path, "r");
	if (!f)
		return -errno;

	if (!fgets(buf, (int)buf_size, f)) {
		int err = ferror(f) ? -EIO : -ENODATA;

		fclose(f);
		return err;
	}
	fclose(f);

	len = strlen(buf);
	if (len > 0 && buf[len - 1] == '\n')
		buf[len - 1] = '\0';

	return 0;
}

static int sysfs_write_string(const char *path, const char *value)
{
	FILE *f;

	f = fopen(path, "w");
	if (!f)
		return -errno;

	if (fprintf(f, "%s", value) < 0) {
		fclose(f);
		return -EIO;
	}

	if (fclose(f) != 0)
		return -errno;

	return 0;
}

static int sysfs_write_uint(const char *path, unsigned long long value)
{
	char str[32];

	snprintf(str, sizeof(str), "%llu", value);
	return sysfs_write_string(path, str);
}

/*
 * ---------------------------------------------------------------------
 * IIO device discovery
 * ---------------------------------------------------------------------
 *
 * Scans /sys/bus/iio/devices/iio:device* for a device whose "name" file
 * matches cfg->device_name (or, if not given, whose name starts with
 * "dma-sampler" - matching both dma-sampler.c's "dma-sampler" and
 * dma-sampler-dmaengine.c's "sampler-dmaengine" driver names; see those
 * files' indio_dev->name assignments). Returns the device's numeric
 * index (N in iio:deviceN) via *out_index, or a negative errno.
 */
static int find_iio_device(const char *want_name, int *out_index)
{
	DIR *dir;
	struct dirent *ent;
	int ret = -ENODEV;

	dir = opendir(IIO_SYSFS_ROOT);
	if (!dir)
		return -errno;

	while ((ent = readdir(dir)) != NULL) {
		char name_path[PATH_MAX];
		char name[128];
		int index;

		if (sscanf(ent->d_name, "iio:device%d", &index) != 1)
			continue;

		snprintf(name_path, sizeof(name_path), "%s/%s/name",
			IIO_SYSFS_ROOT, ent->d_name);
		if (sysfs_read_string(name_path, name, sizeof(name)) != 0)
			continue;

		if (want_name) {
			if (strcmp(name, want_name) != 0)
				continue;
		} else {
			if (strncmp(name, "dma-sampler", 11) != 0 &&
			    strncmp(name, "sampler-dmaengine", 17) != 0)
				continue;
		}

		*out_index = index;
		ret = 0;
		break;
	}

	closedir(dir);
	return ret;
}

/*
 * ---------------------------------------------------------------------
 * scan_elements/in_voltage<C>_type parsing
 * ---------------------------------------------------------------------
 *
 * Parses strings of the form "[be|le]:[s|u]<realbits>/<storagebits>>><shift>"
 * (optionally with a repeat count "X<repeat>" before the shift, which
 * this tool does not support and rejects - see iio_show_fixed_type() in
 * drivers/iio/industrialio-buffer.c for the authoritative format).
 */
struct scan_type {
	bool is_be;
	bool is_signed;
	unsigned int realbits;
	unsigned int storagebits;
	unsigned int shift;
};

static int parse_scan_type(const char *str, struct scan_type *st)
{
	char endian[3];
	char sign;
	unsigned int realbits, storagebits, shift;
	int n;

	memset(st, 0, sizeof(*st));

	n = sscanf(str, "%2[a-z]:%c%u/%u>>%u", endian, &sign, &realbits,
		  &storagebits, &shift);
	if (n != 5) {
		fprintf(stderr,
			"error: unsupported scan_elements type string '%s'\n",
			str);
		return -EINVAL;
	}

	if (strcmp(endian, "be") == 0)
		st->is_be = true;
	else if (strcmp(endian, "le") == 0)
		st->is_be = false;
	else
		return -EINVAL;

	if (sign == 's')
		st->is_signed = true;
	else if (sign == 'u')
		st->is_signed = false;
	else
		return -EINVAL;

	st->realbits = realbits;
	st->storagebits = storagebits;
	st->shift = shift;

	return 0;
}

/*
 * ---------------------------------------------------------------------
 * Capture state / signal handling
 * ---------------------------------------------------------------------
 */
struct capture_state {
	char dev_dir[PATH_MAX];	/* e.g. /sys/bus/iio/devices/iio:device0 */
	int chardev_fd;
	FILE *out;
	bool buffer_enabled;
};

static volatile sig_atomic_t stop_requested;

static void on_sigint(int signum)
{
	(void)signum;
	stop_requested = 1;
}

static void capture_state_cleanup(struct capture_state *st)
{
	if (st->buffer_enabled) {
		char path[PATH_MAX];

		snprintf(path, sizeof(path), "%s/buffer/enable", st->dev_dir);
		sysfs_write_string(path, "0");
	}

	if (st->chardev_fd >= 0)
		close(st->chardev_fd);
	if (st->out)
		fclose(st->out);
}

static int write_header_placeholder(FILE *out)
{
	struct capture_file_header zero = { 0 };

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
	struct capture_state st = { .chardev_fd = -1 };
	struct capture_file_header hdr = { 0 };
	struct scan_type scan_type;
	char path[PATH_MAX];
	char type_str[64];
	char chardev_path[PATH_MAX];
	int dev_index = -1;
	unsigned char *read_buf = NULL;
	size_t read_buf_len;
	uint64_t total_samples = 0;
	int ret;

	ret = parse_args(argc, argv, &cfg);
	if (ret)
		return EXIT_FAILURE;

	signal(SIGINT, on_sigint);
	signal(SIGTERM, on_sigint);

	ret = find_iio_device(cfg.device_name, &dev_index);
	if (ret) {
		fprintf(stderr,
			"error: no matching IIO device found under %s (%s)\n",
			IIO_SYSFS_ROOT, strerror(-ret));
		return EXIT_FAILURE;
	}

	snprintf(st.dev_dir, sizeof(st.dev_dir), "%s/iio:device%d",
		IIO_SYSFS_ROOT, dev_index);
	fprintf(stderr, "info: using %s\n", st.dev_dir);

	/*
	 * Set sampling_frequency (must happen before the buffer is
	 * enabled - the kernel driver enforces this via
	 * iio_device_claim_direct_mode(), returning -EBUSY otherwise).
	 */
	snprintf(path, sizeof(path), "%s/in_voltage_sampling_frequency",
		st.dev_dir);
	ret = sysfs_write_uint(path, cfg.sample_rate_sps);
	if (ret) {
		fprintf(stderr, "error: failed to set %s to %u: %s\n", path,
			cfg.sample_rate_sps, strerror(-ret));
		goto err;
	}

	/*
	 * Disable all channels first (defensive: in case a previous run
	 * left a different channel enabled), then enable only the
	 * requested one - matches the "only one channel at a time"
	 * hardware restriction.
	 */
	{
		DIR *dir;
		struct dirent *ent;
		char scan_dir[PATH_MAX];

		snprintf(scan_dir, sizeof(scan_dir), "%s/scan_elements",
			st.dev_dir);
		dir = opendir(scan_dir);
		if (dir) {
			while ((ent = readdir(dir)) != NULL) {
				char en_path[PATH_MAX];
				size_t name_len = strlen(ent->d_name);

				if (name_len < 3 ||
				    strcmp(ent->d_name + name_len - 3, "_en") != 0)
					continue;

				snprintf(en_path, sizeof(en_path), "%s/%s",
					scan_dir, ent->d_name);
				sysfs_write_string(en_path, "0");
			}
			closedir(dir);
		}
	}

	snprintf(path, sizeof(path),
		"%s/scan_elements/in_voltage%u_en", st.dev_dir,
		cfg.channel_index);
	ret = sysfs_write_string(path, "1");
	if (ret) {
		fprintf(stderr, "error: failed to enable channel %u (%s): %s\n",
			cfg.channel_index, path, strerror(-ret));
		goto err;
	}

	/*
	 * Read back the enabled channel's storage format so we know how
	 * many bytes make up one raw sample, and can fill in the capture
	 * file header accurately.
	 */
	snprintf(path, sizeof(path),
		"%s/scan_elements/in_voltage%u_type", st.dev_dir,
		cfg.channel_index);
	ret = sysfs_read_string(path, type_str, sizeof(type_str));
	if (ret) {
		fprintf(stderr, "error: failed to read %s: %s\n", path,
			strerror(-ret));
		goto err;
	}

	ret = parse_scan_type(type_str, &scan_type);
	if (ret)
		goto err;

	if (scan_type.storagebits == 0 || scan_type.storagebits % 8 != 0) {
		fprintf(stderr,
			"error: unsupported storagebits=%u in '%s'\n",
			scan_type.storagebits, type_str);
		ret = -EINVAL;
		goto err;
	}

	/*
	 * Size the kernel-side buffer (in samples) to one "block"; our
	 * userspace read buffer is sized to num_buffers x that, so a
	 * single read() call can pull several kernel buffer's worth of
	 * samples at once if desired.
	 */
	snprintf(path, sizeof(path), "%s/buffer/length", st.dev_dir);
	ret = sysfs_write_uint(path, SAMPLES_PER_BLOCK);
	if (ret) {
		fprintf(stderr, "error: failed to set %s to %u: %s\n", path,
			SAMPLES_PER_BLOCK, strerror(-ret));
		goto err;
	}

	read_buf_len = (size_t)cfg.num_buffers * SAMPLES_PER_BLOCK *
		      (scan_type.storagebits / 8);
	read_buf = malloc(read_buf_len);
	if (!read_buf) {
		fprintf(stderr, "error: failed to allocate %zu-byte read buffer\n",
			read_buf_len);
		ret = -ENOMEM;
		goto err;
	}

	/*
	 * Open the output file and reserve header space before starting
	 * the capture, mirroring adc_capture.c.
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
	 * Open the IIO buffer character device and start the capture.
	 */
	snprintf(chardev_path, sizeof(chardev_path), "%s/iio:device%d",
		IIO_DEV_NODE_DIR, dev_index);
	st.chardev_fd = open(chardev_path, O_RDONLY | O_NONBLOCK);
	if (st.chardev_fd < 0) {
		fprintf(stderr, "error: failed to open '%s': %s\n",
			chardev_path, strerror(errno));
		goto err;
	}

	/* Buffered reads on the IIO char device block by default; we
	 * opened O_NONBLOCK above only so that open() itself cannot hang
	 * if something else is odd, but we want blocking read() semantics
	 * for the capture loop, so clear the flag again here. */
	{
		int flags = fcntl(st.chardev_fd, F_GETFL, 0);

		if (flags >= 0)
			fcntl(st.chardev_fd, F_SETFL, flags & ~O_NONBLOCK);
	}

	snprintf(path, sizeof(path), "%s/buffer/enable", st.dev_dir);
	ret = sysfs_write_string(path, "1");
	if (ret) {
		fprintf(stderr, "error: failed to enable buffer (%s): %s\n",
			path, strerror(-ret));
		goto err;
	}
	st.buffer_enabled = true;

	fprintf(stderr,
		"info: capturing channel %u at %u sps into '%s'\n"
		"      (%u x %u-sample read buffer, %u-bit samples)\n",
		cfg.channel_index, cfg.sample_rate_sps, cfg.output_path,
		cfg.num_buffers, SAMPLES_PER_BLOCK, scan_type.storagebits);

	/*
	 * Capture loop: read raw sample bytes straight from the char
	 * device and append them to the output file, until either the
	 * requested sample_count is reached (if non-zero) or the user
	 * interrupts us (Ctrl-C / SIGTERM).
	 */
	while (!stop_requested) {
		ssize_t n;
		size_t count;
		size_t bytes;

		n = read(st.chardev_fd, read_buf, read_buf_len);
		if (n < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			fprintf(stderr, "error: read from '%s' failed: %s\n",
				chardev_path, strerror(errno));
			goto err;
		}
		if (n == 0)
			continue;

		bytes = (size_t)n;
		count = bytes / (scan_type.storagebits / 8);

		if (cfg.sample_count && total_samples + count > cfg.sample_count) {
			count = (size_t)(cfg.sample_count - total_samples);
			bytes = count * (scan_type.storagebits / 8);
		}

		if (bytes && fwrite(read_buf, 1, bytes, st.out) != bytes) {
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
	 * Stop the buffer before rewriting the header, so a subsequent
	 * run (or the -h help text example) starts from a clean state;
	 * capture_state_cleanup() would do this too, but doing it
	 * explicitly here keeps the "stop capture, then finalize file"
	 * ordering obvious.
	 */
	sysfs_write_string(path, "0");
	st.buffer_enabled = false;

	hdr.magic = CAPTURE_HEADER_MAGIC;
	hdr.version = CAPTURE_HEADER_VERSION;
	hdr.header_size = sizeof(hdr);
	hdr.sample_rate_sps = cfg.sample_rate_sps;
	hdr.channel_index = cfg.channel_index;
	snprintf(hdr.channel_id, sizeof(hdr.channel_id), "voltage%u",
		cfg.channel_index);
	hdr.sample_bits = scan_type.realbits;
	hdr.storage_bits = scan_type.storagebits;
	hdr.is_signed = scan_type.is_signed;
	hdr.is_big_endian = scan_type.is_be;
	hdr.num_ping_pong_buffers = cfg.num_buffers;
	hdr.samples_per_buffer = SAMPLES_PER_BLOCK;
	hdr.sample_count = total_samples;

	ret = rewrite_header(st.out, &hdr);
	if (ret) {
		fprintf(stderr, "error: failed to write final header: %s\n",
			strerror(-ret));
		goto err;
	}

	free(read_buf);
	capture_state_cleanup(&st);
	return EXIT_SUCCESS;

err:
	free(read_buf);
	capture_state_cleanup(&st);
	return EXIT_FAILURE;
}
