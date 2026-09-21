/* SPDX-License-Identifier: GPL-2.0
 *
 * adc_capture_view - terminal (ncurses) based viewer/analyser for the raw
 * ADC sample capture files produced by adc_capture.c (see
 * capture_format.h for the shared on-disk file format).
 *
 * Layout:
 *
 *   +--------------------------------------------------------------+
 *   | status line: header info (rate, channel, sample count, ...)  |
 *   +--------------------------------------------------------------+
 *   |                                                                |
 *   |  value area: one sample value per line (index: value), the    |
 *   |  currently selected/matched sample is highlighted, scrollable |
 *   |                                                                |
 *   +--------------------------------------------------------------+
 *   | command/result line: ':'- and '/'-style command input, or the |
 *   | result of the last command (cleared when a new command starts |
 *   | being typed)                                                   |
 *   +--------------------------------------------------------------+
 *
 * Supported commands (typed in the bottom line):
 *   /<value>          search for a sample equal to <value> (decimal or
 *                      0x-prefixed hex), starting at the first sample
 *                      currently visible in the value area, wrapping
 *                      around the end of the file if needed. Found
 *                      sample is scrolled into view and highlighted.
 *   :help             show a help screen listing available commands.
 *   :expect N=N+<k>    analyse the whole capture, checking that each
 *                      sample equals the previous sample plus <k>,
 *                      wrapping around at the maximum value
 *                      representable by the capture's sample width.
 *                      Reports "analysis Ok" if the whole capture
 *                      matches, otherwise scrolls/highlights the first
 *                      mismatching sample and reports "analysis Not Ok".
 *   :q, :quit          quit the program.
 *
 * Plain "j"/"k"/arrow keys/PageUp/PageDown/Home/End scroll the value
 * area without entering command mode, for quick browsing.
 *
 * UNTESTED: like the rest of this session's driver/tooling work, this
 * program has not been build-tested in this environment (no libiio/
 * ncursesw target toolchain available here) - please build & sanity
 * check it on the actual build host.
 */

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <locale.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ncurses.h>

#include "capture_format.h"

/*
 * ---------------------------------------------------------------------
 * Sample storage / access helpers
 * ---------------------------------------------------------------------
 */

/* Practical cap on storage_bits we support decoding; the capture format
 * itself does not limit this, but no sampler driver in this tree
 * currently produces more than 16-bit samples (see dma-sampler.c /
 * dma-sampler-dmaengine.c scan_type.storagebits = 16), and a 64-bit
 * ceiling keeps the value arithmetic (min/max, wraparound) simple using
 * plain uint64_t/int64_t.
 */
#define MAX_SUPPORTED_STORAGE_BITS	64

struct capture_file {
	FILE *fp;
	struct capture_file_header hdr;
	long data_offset;	/* byte offset of first sample in the file */
	size_t sample_bytes;	/* hdr.storage_bits / 8 */
	uint64_t sample_count;	/* hdr.sample_count, cached */
};

/* Decoded, sign/zero-extended sample value, widened to int64_t so both
 * signed and unsigned <=64-bit storage widths fit. */
typedef int64_t sample_value_t;

static int capture_file_open(struct capture_file *cf, const char *path)
{
	memset(cf, 0, sizeof(*cf));

	cf->fp = fopen(path, "rb");
	if (!cf->fp) {
		fprintf(stderr, "error: failed to open '%s': %s\n", path,
			strerror(errno));
		return -errno;
	}

	if (fread(&cf->hdr, sizeof(cf->hdr), 1, cf->fp) != 1) {
		fprintf(stderr, "error: failed to read header from '%s'\n",
			path);
		fclose(cf->fp);
		return -EIO;
	}

	if (cf->hdr.magic != CAPTURE_HEADER_MAGIC) {
		fprintf(stderr,
			"error: '%s' is not a capture file (bad magic 0x%08x)\n",
			path, cf->hdr.magic);
		fclose(cf->fp);
		return -EINVAL;
	}

	if (cf->hdr.version != CAPTURE_HEADER_VERSION) {
		/*
		 * Only version 1 exists today. When new versions are
		 * introduced, add per-version field decoding here instead
		 * of rejecting outright - see capture_format.h.
		 */
		fprintf(stderr,
			"error: '%s' has unsupported header version %u (only %u supported)\n",
			path, cf->hdr.version, CAPTURE_HEADER_VERSION);
		fclose(cf->fp);
		return -EINVAL;
	}

	if (cf->hdr.storage_bits == 0 ||
	    cf->hdr.storage_bits > MAX_SUPPORTED_STORAGE_BITS ||
	    cf->hdr.storage_bits % 8 != 0) {
		fprintf(stderr,
			"error: '%s' has unsupported storage_bits=%u\n",
			path, cf->hdr.storage_bits);
		fclose(cf->fp);
		return -EINVAL;
	}

	cf->data_offset = (long)cf->hdr.header_size;
	cf->sample_bytes = cf->hdr.storage_bits / 8;
	cf->sample_count = cf->hdr.sample_count;

	/* Defensive: if the file is shorter than the header claims,
	 * clamp sample_count so we never seek/read past EOF. */
	if (fseek(cf->fp, 0, SEEK_END) == 0) {
		long file_size = ftell(cf->fp);

		if (file_size > cf->data_offset) {
			uint64_t bytes_available = (uint64_t)(file_size - cf->data_offset);
			uint64_t samples_available = bytes_available / cf->sample_bytes;

			if (samples_available < cf->sample_count)
				cf->sample_count = samples_available;
		} else {
			cf->sample_count = 0;
		}
	}

	return 0;
}

static void capture_file_close(struct capture_file *cf)
{
	if (cf->fp)
		fclose(cf->fp);
	cf->fp = NULL;
}

/* Read the raw bytes of sample at index 'idx' and decode it into a
 * signed/unsigned value according to the header's is_signed/is_be
 * fields. Returns 0 on success, negative errno on failure. */
static int capture_file_read_sample(struct capture_file *cf, uint64_t idx,
				    sample_value_t *out)
{
	unsigned char buf[MAX_SUPPORTED_STORAGE_BITS / 8];
	uint64_t raw = 0;
	long offset;

	if (idx >= cf->sample_count)
		return -ERANGE;

	offset = cf->data_offset + (long)(idx * cf->sample_bytes);
	if (fseek(cf->fp, offset, SEEK_SET) != 0)
		return -errno;

	if (fread(buf, cf->sample_bytes, 1, cf->fp) != 1)
		return -EIO;

	if (cf->hdr.is_big_endian) {
		for (size_t i = 0; i < cf->sample_bytes; i++)
			raw = (raw << 8) | buf[i];
	} else {
		for (size_t i = cf->sample_bytes; i-- > 0; )
			raw = (raw << 8) | buf[i];
	}

	if (cf->hdr.is_signed && cf->hdr.storage_bits < 64) {
		uint64_t sign_bit = (uint64_t)1 << (cf->hdr.storage_bits - 1);

		if (raw & sign_bit)
			raw |= ~(((uint64_t)1 << cf->hdr.storage_bits) - 1);
	}

	*out = cf->hdr.is_signed ? (sample_value_t)(int64_t)raw
				: (sample_value_t)raw;
	return 0;
}

/* Maximum value + 1 representable by the capture's storage width,
 * treated as unsigned for wraparound arithmetic purposes (matches how
 * "N=N+k" wraparound is defined in the task: wrap when the maximum
 * *sample* value is exceeded). */
static uint64_t capture_file_value_space(struct capture_file *cf)
{
	if (cf->hdr.storage_bits >= 64)
		return 0; /* 2^64, represented as 0 after wraparound math */

	return (uint64_t)1 << cf->hdr.storage_bits;
}

/*
 * ---------------------------------------------------------------------
 * UI state
 * ---------------------------------------------------------------------
 */
enum ui_mode {
	MODE_NORMAL,
	MODE_COMMAND,	/* typing after ':' */
	MODE_SEARCH,	/* typing after '/' */
};

#define CMD_BUF_SIZE	256
#define RESULT_BUF_SIZE	256

struct ui_state {
	struct capture_file cf;

	int rows, cols;
	WINDOW *status_win;
	WINDOW *value_win;
	WINDOW *cmd_win;

	uint64_t top_index;	/* index of first sample shown in value_win */
	uint64_t highlight_index;	/* index currently highlighted, or
					 * UINT64_MAX for "none" */
	bool has_highlight;

	enum ui_mode mode;
	char cmd_buf[CMD_BUF_SIZE];
	size_t cmd_len;

	char result_buf[RESULT_BUF_SIZE];

	bool quit;
};

static int value_area_rows(struct ui_state *ui)
{
	int h = ui->rows - 2; /* minus status line and command line */

	return h > 0 ? h : 0;
}

static void clamp_top_index(struct ui_state *ui)
{
	uint64_t count = ui->cf.sample_count;
	int visible = value_area_rows(ui);

	if (count == 0) {
		ui->top_index = 0;
		return;
	}

	if (visible <= 0)
		return;

	if ((uint64_t)visible >= count) {
		ui->top_index = 0;
		return;
	}

	if (ui->top_index > count - (uint64_t)visible)
		ui->top_index = count - (uint64_t)visible;
}

static void ensure_visible(struct ui_state *ui, uint64_t idx)
{
	int visible = value_area_rows(ui);

	if (visible <= 0)
		return;

	if (idx < ui->top_index)
		ui->top_index = idx;
	else if (idx >= ui->top_index + (uint64_t)visible)
		ui->top_index = idx - (uint64_t)visible + 1;

	clamp_top_index(ui);
}

/*
 * ---------------------------------------------------------------------
 * Rendering
 * ---------------------------------------------------------------------
 */
static void render_status(struct ui_state *ui)
{
	struct capture_file_header *h = &ui->cf.hdr;
	char line[512];

	werase(ui->status_win);
	wattron(ui->status_win, A_REVERSE);

	snprintf(line, sizeof(line),
		" chan=%s(#%u)  rate=%u sps  bits=%u/%u %s%s  "
		"buffers=%u x %u  samples=%" PRIu64 "  pos=%" PRIu64,
		h->channel_id, h->channel_index, h->sample_rate_sps,
		h->sample_bits, h->storage_bits,
		h->is_signed ? "signed" : "unsigned",
		h->is_big_endian ? " BE" : " LE",
		h->num_ping_pong_buffers, h->samples_per_buffer,
		ui->cf.sample_count, ui->top_index);

	mvwprintw(ui->status_win, 0, 0, "%-*s", ui->cols, line);
	wattroff(ui->status_win, A_REVERSE);
	wnoutrefresh(ui->status_win);
}

static void render_values(struct ui_state *ui)
{
	int visible = value_area_rows(ui);
	int row;

	werase(ui->value_win);

	for (row = 0; row < visible; row++) {
		uint64_t idx = ui->top_index + (uint64_t)row;
		sample_value_t val;

		if (idx >= ui->cf.sample_count)
			break;

		if (capture_file_read_sample(&ui->cf, idx, &val) != 0) {
			mvwprintw(ui->value_win, row, 0,
				 "%10" PRIu64 ": <read error>", idx);
			continue;
		}

		if (ui->has_highlight && idx == ui->highlight_index)
			wattron(ui->value_win, A_REVERSE | A_BOLD);

		mvwprintw(ui->value_win, row, 0, "%10" PRIu64 ": %10" PRId64
			 " (0x%" PRIx64 ")", idx, (int64_t)val,
			 (uint64_t)val & (ui->cf.hdr.storage_bits >= 64 ?
					  ~(uint64_t)0 :
					  (((uint64_t)1 << ui->cf.hdr.storage_bits) - 1)));

		if (ui->has_highlight && idx == ui->highlight_index)
			wattroff(ui->value_win, A_REVERSE | A_BOLD);
	}

	wnoutrefresh(ui->value_win);
}

static void render_cmd_line(struct ui_state *ui)
{
	werase(ui->cmd_win);

	if (ui->mode == MODE_COMMAND)
		mvwprintw(ui->cmd_win, 0, 0, ":%s", ui->cmd_buf);
	else if (ui->mode == MODE_SEARCH)
		mvwprintw(ui->cmd_win, 0, 0, "/%s", ui->cmd_buf);
	else
		mvwprintw(ui->cmd_win, 0, 0, "%s", ui->result_buf);

	wnoutrefresh(ui->cmd_win);
}

static void render_all(struct ui_state *ui)
{
	render_status(ui);
	render_values(ui);
	render_cmd_line(ui);
	doupdate();
}

static void set_result(struct ui_state *ui, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(ui->result_buf, sizeof(ui->result_buf), fmt, ap);
	va_end(ap);
}

/*
 * ---------------------------------------------------------------------
 * Search command: '/<value>'
 * ---------------------------------------------------------------------
 *
 * Starts scanning from the first sample currently visible in the value
 * area (ui->top_index), wrapping around the end of the file back to
 * index 0 if no match is found before reaching the end, and stopping
 * once the whole file has been scanned once (i.e. we returned to the
 * starting index without a match) or a match was found.
 */
static bool parse_integer_value(const char *str, sample_value_t *out)
{
	char *end;
	long long v;

	if (!*str)
		return false;

	errno = 0;
	v = strtoll(str, &end, 0);
	if (errno != 0 || *end != '\0')
		return false;

	*out = (sample_value_t)v;
	return true;
}

static void do_search(struct ui_state *ui, const char *value_str)
{
	sample_value_t target;
	uint64_t count = ui->cf.sample_count;
	uint64_t start, idx;
	uint64_t scanned;

	if (count == 0) {
		set_result(ui, "search: capture is empty");
		return;
	}

	if (!parse_integer_value(value_str, &target)) {
		set_result(ui, "search: invalid value '%s'", value_str);
		return;
	}

	start = ui->top_index % count;
	idx = start;

	for (scanned = 0; scanned < count; scanned++, idx = (idx + 1) % count) {
		sample_value_t val;

		if (capture_file_read_sample(&ui->cf, idx, &val) != 0)
			continue;

		if (val == target) {
			ui->has_highlight = true;
			ui->highlight_index = idx;
			ensure_visible(ui, idx);
			set_result(ui, "search: found value %" PRId64
				  " at sample index %" PRIu64, (int64_t)target,
				  idx);
			return;
		}
	}

	set_result(ui, "search: value %" PRId64 " not found (scanned whole file)",
		  (int64_t)target);
}

/*
 * ---------------------------------------------------------------------
 * ':expect N=N+<k>' analysis command
 * ---------------------------------------------------------------------
 *
 * Verifies that, for every pair of consecutive samples (prev, cur), cur
 * == (prev + k) mod value_space, where value_space is 2^storage_bits
 * (the wraparound point is the maximum value representable by the
 * capture's sample storage width, per the task description - not the
 * ADC's real bit-width, since a full LSRAM word/wire width is what
 * actually wraps in hardware). The very first sample has no predecessor
 * and is always accepted as the analysis' starting point.
 */
static bool parse_expect_arg(const char *arg, int64_t *step)
{
	/* Accept "N=N+<k>" (k possibly negative, e.g. "N=N+-1" is silly but
	 * harmless; more usefully supports "N=N+1", "N=N+2", ...). Any
	 * other syntax is rejected for now - see file header comment: only
	 * this specific expectation form is implemented today. */
	static const char prefix[] = "N=N+";
	const char *k_str;
	char *end;
	long long v;

	if (strncmp(arg, prefix, strlen(prefix)) != 0)
		return false;

	k_str = arg + strlen(prefix);
	if (!*k_str)
		return false;

	errno = 0;
	v = strtoll(k_str, &end, 0);
	if (errno != 0 || *end != '\0')
		return false;

	*step = (int64_t)v;
	return true;
}

static void do_expect(struct ui_state *ui, const char *arg)
{
	int64_t step;
	uint64_t count = ui->cf.sample_count;
	uint64_t value_space; /* 0 means "2^64", i.e. no masking needed */
	sample_value_t prev, cur;
	uint64_t idx;

	if (!parse_expect_arg(arg, &step)) {
		set_result(ui, "expect: unsupported expression '%s' "
			  "(only \"N=N+<k>\" is supported)", arg);
		return;
	}

	if (count < 2) {
		set_result(ui, "analysis Ok (fewer than 2 samples, nothing to check)");
		return;
	}

	value_space = capture_file_value_space(&ui->cf);

	if (capture_file_read_sample(&ui->cf, 0, &prev) != 0) {
		set_result(ui, "expect: failed to read sample 0");
		return;
	}

	for (idx = 1; idx < count; idx++) {
		int64_t expected;

		if (capture_file_read_sample(&ui->cf, idx, &cur) != 0) {
			set_result(ui, "expect: failed to read sample %" PRIu64, idx);
			return;
		}

		expected = (int64_t)prev + step;
		if (value_space != 0) {
			/* wrap into [0, value_space) using unsigned/masked
			 * arithmetic so both signed and unsigned storage
			 * widths wrap the same way at the storage width's
			 * value space boundary. */
			uint64_t mask = value_space - 1;

			expected = (int64_t)(((uint64_t)expected) & mask);
			if (ui->cf.hdr.is_signed) {
				uint64_t sign_bit = value_space >> 1;

				if ((uint64_t)expected & sign_bit)
					expected = (int64_t)(((uint64_t)expected) | ~mask);
			}
		}

		if (cur != (sample_value_t)expected) {
			ui->has_highlight = true;
			ui->highlight_index = idx;
			ensure_visible(ui, idx);
			set_result(ui, "analysis Not Ok: sample %" PRIu64
				  " = %" PRId64 ", expected %" PRId64
				  " (previous sample %" PRIu64 " = %" PRId64 ")",
				  idx, (int64_t)cur, expected, idx - 1,
				  (int64_t)prev);
			return;
		}

		prev = cur;
	}

	set_result(ui, "analysis Ok");
}

/*
 * ---------------------------------------------------------------------
 * ':help' command
 * ---------------------------------------------------------------------
 */
static void show_help(struct ui_state *ui)
{
	static const char *const help_lines[] = {
		"adc_capture_view - available commands",
		"",
		"  /<value>          Search for a sample equal to <value>",
		"                    (decimal, or 0x-prefixed hex). Search",
		"                    starts at the top of the currently",
		"                    visible area and wraps around the end",
		"                    of the file.",
		"  :expect N=N+<k>   Verify each sample equals the previous",
		"                    sample plus <k>, wrapping at the value",
		"                    space of the capture's sample width.",
		"                    Reports 'analysis Ok' or scrolls to and",
		"                    highlights the first mismatch and",
		"                    reports 'analysis Not Ok'.",
		"  :help             Show this help screen.",
		"  :q, :quit         Quit the program.",
		"",
		"Navigation (normal mode, no leading ':' or '/'):",
		"  j / Down          scroll down one sample",
		"  k / Up            scroll up one sample",
		"  PageDown / space   scroll down one page",
		"  PageUp            scroll up one page",
		"  g / Home          jump to first sample",
		"  G / End           jump to last sample",
		"  Esc               cancel command/search input",
		"  q                 quit",
		"",
		"Press any key to return...",
	};
	size_t n = sizeof(help_lines) / sizeof(help_lines[0]);
	size_t i;

	werase(ui->value_win);
	for (i = 0; i < n && (int)i < value_area_rows(ui); i++)
		mvwprintw(ui->value_win, (int)i, 0, "%s", help_lines[i]);
	wnoutrefresh(ui->value_win);
	doupdate();

	wgetch(ui->value_win);
}

/*
 * ---------------------------------------------------------------------
 * Command dispatch
 * ---------------------------------------------------------------------
 */
static void run_command(struct ui_state *ui, const char *cmd)
{
	if (strcmp(cmd, "help") == 0) {
		show_help(ui);
		ui->result_buf[0] = '\0';
		return;
	}

	if (strcmp(cmd, "q") == 0 || strcmp(cmd, "quit") == 0) {
		ui->quit = true;
		return;
	}

	if (strncmp(cmd, "expect ", 7) == 0) {
		do_expect(ui, cmd + 7);
		return;
	}

	set_result(ui, "unknown command ':%s' (try ':help')", cmd);
}

/*
 * ---------------------------------------------------------------------
 * Input handling
 * ---------------------------------------------------------------------
 */
static void handle_normal_key(struct ui_state *ui, int ch)
{
	int visible = value_area_rows(ui);

	switch (ch) {
	case 'j':
	case KEY_DOWN:
		if (ui->top_index + 1 < ui->cf.sample_count ||
		    ui->cf.sample_count == 0)
			ui->top_index++;
		clamp_top_index(ui);
		break;
	case 'k':
	case KEY_UP:
		if (ui->top_index > 0)
			ui->top_index--;
		break;
	case KEY_NPAGE:
	case ' ':
		ui->top_index += (uint64_t)(visible > 0 ? visible : 1);
		clamp_top_index(ui);
		break;
	case KEY_PPAGE:
		if (ui->top_index > (uint64_t)visible)
			ui->top_index -= (uint64_t)visible;
		else
			ui->top_index = 0;
		break;
	case 'g':
	case KEY_HOME:
		ui->top_index = 0;
		break;
	case 'G':
	case KEY_END:
		if (ui->cf.sample_count > 0)
			ui->top_index = ui->cf.sample_count - 1;
		clamp_top_index(ui);
		break;
	case ':':
		ui->mode = MODE_COMMAND;
		ui->cmd_len = 0;
		ui->cmd_buf[0] = '\0';
		ui->result_buf[0] = '\0';
		break;
	case '/':
		ui->mode = MODE_SEARCH;
		ui->cmd_len = 0;
		ui->cmd_buf[0] = '\0';
		ui->result_buf[0] = '\0';
		break;
	case 'q':
		ui->quit = true;
		break;
	default:
		break;
	}
}

static void handle_input_key(struct ui_state *ui, int ch)
{
	if (ch == 27) { /* Esc: cancel */
		ui->mode = MODE_NORMAL;
		ui->cmd_len = 0;
		ui->cmd_buf[0] = '\0';
		return;
	}

	if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
		enum ui_mode mode = ui->mode;

		ui->mode = MODE_NORMAL;
		if (mode == MODE_SEARCH)
			do_search(ui, ui->cmd_buf);
		else
			run_command(ui, ui->cmd_buf);
		ui->cmd_len = 0;
		ui->cmd_buf[0] = '\0';
		return;
	}

	if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
		if (ui->cmd_len > 0)
			ui->cmd_buf[--ui->cmd_len] = '\0';
		return;
	}

	if (isprint(ch) && ui->cmd_len + 1 < sizeof(ui->cmd_buf)) {
		ui->cmd_buf[ui->cmd_len++] = (char)ch;
		ui->cmd_buf[ui->cmd_len] = '\0';
	}
}

/*
 * ---------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------
 */
static void print_usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s <capture_file>\n"
		"       %s -h | --help\n"
		"\n"
		"Terminal (ncurses) viewer/analyser for raw ADC capture\n"
		"files produced by adc_capture.\n"
		"\n"
		"Example:\n"
		"  %s capture0.bin\n",
		prog, prog, prog);
}

int main(int argc, char **argv)
{
	struct ui_state ui = { 0 };
	int ret;

	if (argc == 2 && (strcmp(argv[1], "-h") == 0 ||
			 strcmp(argv[1], "--help") == 0)) {
		print_usage(argv[0]);
		return EXIT_SUCCESS;
	}

	if (argc != 2) {
		print_usage(argv[0]);
		return EXIT_FAILURE;
	}

	ret = capture_file_open(&ui.cf, argv[1]);
	if (ret)
		return EXIT_FAILURE;

	ui.has_highlight = false;
	ui.mode = MODE_NORMAL;

	setlocale(LC_ALL, "");
	initscr();
	cbreak();
	noecho();
	keypad(stdscr, TRUE);
	curs_set(0);

	getmaxyx(stdscr, ui.rows, ui.cols);
	ui.status_win = newwin(1, ui.cols, 0, 0);
	ui.value_win = newwin(ui.rows - 2, ui.cols, 1, 0);
	ui.cmd_win = newwin(1, ui.cols, ui.rows - 1, 0);
	keypad(ui.value_win, TRUE);

	set_result(&ui, "loaded '%s': %" PRIu64 " samples "
		  "(':help' for commands, 'q' to quit)",
		  argv[1], ui.cf.sample_count);

	while (!ui.quit) {
		int ch;

		render_all(&ui);

		ch = wgetch(ui.mode == MODE_NORMAL ? stdscr : ui.value_win);
		if (ch == ERR)
			continue;

		if (ui.mode == MODE_NORMAL)
			handle_normal_key(&ui, ch);
		else
			handle_input_key(&ui, ch);
	}

	delwin(ui.status_win);
	delwin(ui.value_win);
	delwin(ui.cmd_win);
	endwin();

	capture_file_close(&ui.cf);
	return EXIT_SUCCESS;
}
