/* SPDX-License-Identifier: GPL-2.0
 *
 * capture_format.h - on-disk layout shared by adc_capture (writer) and
 * adc_capture_view (reader) for the raw ADC sample capture files
 * produced by adc_capture.c.
 *
 * See adc_capture.c's top-of-file comment for the full rationale. Keep
 * this header as the single source of truth for the format so the two
 * tools cannot drift apart.
 */

#ifndef CAPTURE_FORMAT_H
#define CAPTURE_FORMAT_H

#include <stdint.h>

/*
 * ---------------------------------------------------------------------
 * Capture file header
 * ---------------------------------------------------------------------
 *
 * CAPTURE_HEADER_VERSION must be bumped whenever the on-disk layout of
 * struct capture_file_header changes in a way that is not purely
 * additive-at-the-end (e.g. changing a field's type/meaning, or
 * reordering/removing fields). Purely additive changes (new fields
 * appended after channel_id, with header_size growing accordingly) may
 * keep the same version as long as readers are expected to use
 * header_size (not sizeof(struct capture_file_header)) to know where the
 * sample data actually starts - both tools always do that.
 *
 * All multi-byte header fields are stored in the CPU's native byte
 * order. Since these tools are only intended to run in-place on the
 * BeagleV-Fire target (little-endian RISC-V) and be analysed on typical
 * (little-endian x86/ARM) development hosts, no explicit endianness
 * conversion is performed. If cross-endian analysis is ever needed, add
 * an explicit byte-order marker field and convert accordingly - this is
 * intentionally NOT done here to keep the format simple.
 */
#define CAPTURE_HEADER_MAGIC	0x41444331u	/* "ADC1" */
#define CAPTURE_HEADER_VERSION	1u

struct capture_file_header {
	/* Fixed fields: MUST remain first/stable across all versions so
	 * that any reader can at least identify the file and its version
	 * before deciding how to interpret the rest of the header.
	 */
	uint32_t magic;		/* CAPTURE_HEADER_MAGIC */
	uint32_t version;	/* CAPTURE_HEADER_VERSION at capture time */
	uint32_t header_size;	/* sizeof(struct capture_file_header) at
				 * capture time; readers should skip this
				 * many bytes to reach the first sample,
				 * regardless of their own compiled-in
				 * sizeof(struct capture_file_header). */

	/* Version-1 fields */
	uint32_t sample_rate_sps;	/* configured ADC sampling frequency,
					 * in samples per second */
	uint32_t channel_index;	/* IIO channel index enabled for this
					 * capture (e.g. 0 for "voltage0") */
	char channel_id[32];	/* IIO channel id string, e.g. "voltage0",
				 * NUL-terminated, truncated if longer */
	uint32_t sample_bits;	/* number of significant bits per sample,
				 * as reported by libIIO's data format
				 * (iio_data_format.bits) */
	uint32_t storage_bits;	/* on-disk/on-wire size of one raw sample,
				 * in bits, as reported by libIIO
				 * (iio_data_format.length); this is the
				 * unit used by sample_count below */
	uint32_t is_signed;	/* non-zero if samples are signed */
	uint32_t is_big_endian;	/* non-zero if raw samples are stored in
				 * big-endian order (matches
				 * iio_data_format.is_be) */
	uint32_t num_ping_pong_buffers;	/* number of libIIO iio_block
					 * buffers requested for the capture
					 * stream (the "ping-pong buffer
					 * count" input parameter) */
	uint32_t samples_per_buffer;	/* number of samples requested per
					 * libIIO iio_block */
	uint64_t sample_count;	/* total number of raw samples following
				 * this header in the file */
} __attribute__((packed));

#endif /* CAPTURE_FORMAT_H */
