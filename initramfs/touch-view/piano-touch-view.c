/*
 * piano-touch-view — inspect the NT36532 THP frame stream on the device.
 *
 * Written from scratch for the linux-xiaomi-piano bring-up initramfs.
 * The kernel driver (linux-piano drivers/input/touchscreen/nt36532e)
 * publishes /proc/nvt_thp_stream: records of a 32-byte little-endian
 * header (magic "NTP1", header_len, frame_len, sequence, timestamp_ns,
 * header_crc, flags, firmware_magic) followed by frame_len bytes. A frame
 * is the 1-byte SPI command slot, the 256-byte event buffer and the
 * Xiaomi host touch computing payload. Payload layout, as parsed by the
 * stock p81 driver (refer/MiCode_piano/.../p81/nt36532/nt36xxx.c,
 * nvt_ts_prase_data_func):
 *
 *   +4 u16 checksum      +8 s32 crc_len (dwords from +20)
 *   +12 u16 ~checksum    +16 s32 ~crc_len
 *   +28 u16 frame_no     +48 u8 columns  +49 u8 rows
 *   +56 u8 data type     +64 s16 matrix[rows][columns] (mutual frames)
 *
 * checksum == -(sum of the crc_len*2 u16 words from +20).
 *
 * This is a bring-up tool, not a tuned touch service: the reference is the
 * median of the first frames (keep fingers off while it starts) and then
 * follows slow drift while nothing touches; contacts are plain local maxima
 * with a 3x3 centroid, tracked frame to frame by nearest distance.
 *
 * Screen orientation (landscape 3200x2136 console, measured on piano):
 * sensor columns run along screen x, sensor rows along screen y with y
 * reversed. The --swap-xy/--flip-x/--flip-y options toggle from there.
 *
 * Usage: piano-touch-view [MODE] [options]
 *   MODE: stats (default) | map | points | paint | dump | input
 *         paint draws finger traces on the framebuffer and logs touch
 *         down/move/up on the console; three fingers or "c" + Enter clear
 *         the canvas, "q" + Enter stops, and the console picture is put
 *         back when it ends
 *         input creates a multitouch evdev device through /dev/uinput
 *   --seconds N      stop after N seconds (default 30, 0 = forever)
 *   --type N         matrix frame type (default: most common in the
 *                    first frames, pen types 6/7/9/0x1d excluded)
 *   --threshold T    delta counted as touch (default 200)
 *   --reference N    reference frames (default 32)
 *   --invert         touches lower the value (default: raise)
 *   --swap-xy --flip-x --flip-y   toggle the orientation
 *
 * PIANO_THP_STREAM=<file> replays a captured stream instead of the proc
 * interface (capture control is left alone then).
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <linux/fb.h>
#include <linux/uinput.h>

#define STREAM_PATH "/proc/nvt_thp_stream"
#define CONTROL_PATH "/proc/nvt_thp_raw"
#define STREAM_MAGIC 0x3150544eu
#define TRANSPORT_LEN 257
#define PAYLOAD_MIN 64
#define MAX_NODES 4096
#define MAX_TYPES 256
#define MAX_REF 128
#define MAX_POINTS 10
#define TYPE_PROBE_FRAMES 16
#define SCREEN_W 3200
#define SCREEN_H 2136
/* a contact moving further than this between frames is a new finger */
#define TRACK_MAX_DIST 400

enum mode { MODE_STATS, MODE_MAP, MODE_POINTS, MODE_PAINT, MODE_DUMP,
	    MODE_INPUT };

static volatile sig_atomic_t running = 1;

static struct {
	enum mode mode;
	int seconds;
	int threshold;
	int ref_frames;
	int invert;
	int swap_xy, flip_x, flip_y;
	int type;
} opt = { MODE_STATS, 30, 200, 32, 0, 0, 0, 1, -1 };

static struct {
	unsigned long records, bad_magic, flag_valid, csum_ok, csum_bad;
	unsigned long short_payload, types[MAX_TYPES];
	int rows, cols;
	unsigned int last_frame_no;
	int max_delta;
	unsigned long ui_write_errors;
} st;

static int16_t ref_samples[MAX_REF][MAX_NODES];
static int ref_count, ref_type = -1;
static unsigned long probe_types[MAX_TYPES], probe_count;
static int reference[MAX_NODES];
static int have_reference;
static int delta[MAX_NODES];

static struct {
	uint8_t *mem, *saved;
	unsigned int width, height, stride, bpp;
	size_t size;
} fb;

/* contacts tracked across frames, in SCREEN_W x SCREEN_H coordinates */
static struct {
	int next_id;
	struct {
		int active, id, x, y, px, py, fresh, ended;
	} slot[MAX_POINTS];
} trk;

static void on_signal(int sig)
{
	(void)sig;
	running = 0;
}

static uint16_t le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	       (uint32_t)p[3] << 24;
}

static double now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int write_control(int value)
{
	int fd = open(CONTROL_PATH, O_WRONLY | O_CLOEXEC);
	int ret;

	if (fd < 0)
		return -errno;
	ret = write(fd, value ? "1\n" : "0\n", 2) == 2 ? 0 : -errno;
	close(fd);
	return ret;
}

/* 1 = checksum matches, 0 = mismatch, -1 = header fields inconsistent */
static int payload_checksum(const uint8_t *p, size_t len)
{
	uint16_t csum = le16(p + 4), csum_inv = le16(p + 12);
	int32_t crc_len = (int32_t)le32(p + 8), crc_len_inv = (int32_t)le32(p + 16);
	uint32_t sum = 0;
	size_t words, i;

	if (csum_inv != (uint16_t)~csum || crc_len_inv != ~crc_len)
		return -1;
	if (crc_len <= 0 || 20 + (size_t)crc_len * 4 > len)
		return -1;
	words = (size_t)crc_len * 2;
	for (i = 0; i < words; i++)
		sum += le16(p + 20 + i * 2);
	return (uint16_t)(~sum + 1) == csum;
}

static int cmp_int16(const void *a, const void *b)
{
	return *(const int16_t *)a - *(const int16_t *)b;
}

static void build_reference(int nodes)
{
	int16_t column[MAX_REF];
	int n, k;

	for (n = 0; n < nodes; n++) {
		for (k = 0; k < ref_count; k++)
			column[k] = ref_samples[k][n];
		qsort(column, ref_count, sizeof(column[0]), cmp_int16);
		reference[n] = column[ref_count / 2];
	}
	have_reference = 1;
	fprintf(stderr, "reference ready (%d frames of type %d, %dx%d)\n",
		ref_count, ref_type, st.rows, st.cols);
}

static void print_map(void)
{
	static const char ramp[] = " .:-=+*#%@";
	int r, c, level;

	printf("\033[H\033[2J");
	for (r = 0; r < st.rows; r++) {
		for (c = 0; c < st.cols; c++) {
			int d = delta[r * st.cols + c];

			level = d <= 0 ? 0 : d * 9 / (opt.threshold * 3);
			if (d > 0 && level == 0 && d >= opt.threshold / 2)
				level = 1;
			putchar(ramp[level > 9 ? 9 : level]);
		}
		putchar('\n');
	}
	printf("max delta %d (threshold %d)\n", st.max_delta, opt.threshold);
	fflush(stdout);
}

struct point {
	double x, y;	/* column, row in sensor pitch units */
	int peak;
};

static int find_points(struct point *pts)
{
	int r, c, dr, dc, n = 0;

	for (r = 0; r < st.rows; r++) {
		for (c = 0; c < st.cols; c++) {
			int v = delta[r * st.cols + c], is_peak = 1;
			double sw = 0, sx = 0, sy = 0;

			if (v < opt.threshold)
				continue;
			for (dr = -1; dr <= 1 && is_peak; dr++)
				for (dc = -1; dc <= 1; dc++) {
					int rr = r + dr, cc = c + dc;

					if ((!dr && !dc) || rr < 0 || cc < 0 ||
					    rr >= st.rows || cc >= st.cols)
						continue;
					/* ties resolve to the first node */
					if (delta[rr * st.cols + cc] > v ||
					    (delta[rr * st.cols + cc] == v &&
					     (dr < 0 || (dr == 0 && dc < 0)))) {
						is_peak = 0;
						break;
					}
				}
			if (!is_peak)
				continue;
			for (dr = -1; dr <= 1; dr++)
				for (dc = -1; dc <= 1; dc++) {
					int rr = r + dr, cc = c + dc, w;

					if (rr < 0 || cc < 0 || rr >= st.rows ||
					    cc >= st.cols)
						continue;
					w = delta[rr * st.cols + cc];
					if (w <= 0)
						continue;
					sw += w;
					sx += w * (cc + 0.5);
					sy += w * (rr + 0.5);
				}
			if (n < MAX_POINTS && sw > 0) {
				pts[n].x = sx / sw;
				pts[n].y = sy / sw;
				pts[n].peak = v;
				n++;
			}
		}
	}
	return n;
}

static int fb_open(void)
{
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
	int fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);

	if (fd < 0)
		return -errno;
	if (ioctl(fd, FBIOGET_VSCREENINFO, &var) || ioctl(fd, FBIOGET_FSCREENINFO, &fix)) {
		close(fd);
		return -errno;
	}
	fb.width = var.xres;
	fb.height = var.yres;
	fb.bpp = var.bits_per_pixel;
	fb.stride = fix.line_length;
	fb.size = (size_t)fix.line_length * var.yres;
	if (fb.bpp != 32) {
		close(fd);
		return -ENOTSUP;
	}
	fb.mem = mmap(NULL, fb.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (fb.mem == MAP_FAILED) {
		fb.mem = NULL;
		return -errno;
	}
	/* keep the console picture to put it back at the end */
	fb.saved = malloc(fb.size);
	if (fb.saved)
		memcpy(fb.saved, fb.mem, fb.size);
	memset(fb.mem, 0, fb.size);
	return 0;
}

static void fb_clear(void)
{
	if (fb.mem)
		memset(fb.mem, 0, fb.size);
}

static void fb_close(void)
{
	if (!fb.mem)
		return;
	if (fb.saved)
		memcpy(fb.mem, fb.saved, fb.size);
	else
		memset(fb.mem, 0, fb.size);
	munmap(fb.mem, fb.size);
	fb.mem = NULL;
	free(fb.saved);
	fb.saved = NULL;
}

static void fb_dot(int x, int y, int radius, uint32_t color)
{
	int dx, dy;

	for (dy = -radius; dy <= radius; dy++)
		for (dx = -radius; dx <= radius; dx++) {
			int px = x + dx, py = y + dy;

			if (dx * dx + dy * dy > radius * radius ||
			    px < 0 || py < 0 || px >= (int)fb.width ||
			    py >= (int)fb.height)
				continue;
			memcpy(fb.mem + (size_t)py * fb.stride + px * 4, &color, 4);
		}
}

/* sensor centroid -> screen pixel */
static void to_screen(const struct point *pt, int width, int height,
		      int *x, int *y)
{
	double u = pt->x / st.cols, v = pt->y / st.rows, t;

	if (opt.swap_xy) {
		t = u;
		u = v;
		v = t;
	}
	if (opt.flip_x)
		u = 1.0 - u;
	if (opt.flip_y)
		v = 1.0 - v;
	*x = (int)(u * width);
	*y = (int)(v * height);
	if (*x < 0)
		*x = 0;
	if (*x >= width)
		*x = width - 1;
	if (*y < 0)
		*y = 0;
	if (*y >= height)
		*y = height - 1;
}

/* match this frame's contacts to the slots of the previous one */
static int track(const struct point *pts, int n)
{
	int px[MAX_POINTS], py[MAX_POINTS], taken[MAX_POINTS] = { 0 };
	int i, s, active = 0;

	for (i = 0; i < n; i++)
		to_screen(&pts[i], SCREEN_W, SCREEN_H, &px[i], &py[i]);

	for (s = 0; s < MAX_POINTS; s++) {
		int best = -1, best_d = TRACK_MAX_DIST * TRACK_MAX_DIST;

		trk.slot[s].fresh = 0;
		trk.slot[s].ended = 0;
		if (!trk.slot[s].active)
			continue;
		for (i = 0; i < n; i++) {
			int dx = px[i] - trk.slot[s].x, dy = py[i] - trk.slot[s].y;

			if (!taken[i] && dx * dx + dy * dy < best_d) {
				best = i;
				best_d = dx * dx + dy * dy;
			}
		}
		if (best < 0) {
			trk.slot[s].active = 0;
			trk.slot[s].ended = 1;
			continue;
		}
		taken[best] = 1;
		trk.slot[s].px = trk.slot[s].x;
		trk.slot[s].py = trk.slot[s].y;
		trk.slot[s].x = px[best];
		trk.slot[s].y = py[best];
	}
	for (i = 0; i < n; i++) {
		if (taken[i])
			continue;
		for (s = 0; s < MAX_POINTS && (trk.slot[s].active || trk.slot[s].ended); s++)
			;
		if (s == MAX_POINTS)
			break;
		trk.slot[s].active = 1;
		trk.slot[s].fresh = 1;
		trk.slot[s].id = trk.next_id++ & 0xffff;
		trk.slot[s].x = trk.slot[s].px = px[i];
		trk.slot[s].y = trk.slot[s].py = py[i];
	}
	for (s = 0; s < MAX_POINTS; s++)
		active += trk.slot[s].active;
	return active;
}

static void fb_line(int x0, int y0, int x1, int y1, int radius, uint32_t color)
{
	int dx = x1 - x0, dy = y1 - y0;
	int steps = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
	int k;

	steps = steps / (radius > 1 ? radius / 2 : 1) + 1;
	for (k = 0; k <= steps; k++)
		fb_dot(x0 + dx * k / steps, y0 + dy * k / steps, radius, color);
}

static void paint(int active)
{
	/* a8b8g8r8 in memory order R, G, B, A */
	static const uint32_t colors[] = {
		0xff0000ff, 0xff00ff00, 0xffff0000, 0xff00ffff, 0xffff00ff,
		0xffffff00, 0xffffffff, 0xff0080ff, 0xff8000ff, 0xff80ff00,
	};
	static int prev_active;
	static double last_move;
	int s, moved = 0;

	/* three fingers wipe the canvas */
	if (active >= 3 && prev_active < 3) {
		fb_clear();
		printf("three fingers: canvas cleared\n");
	}
	prev_active = active;

	for (s = 0; s < MAX_POINTS; s++) {
		uint32_t color = colors[trk.slot[s].id % 10];
		int x = trk.slot[s].x * (int)fb.width / SCREEN_W;
		int y = trk.slot[s].y * (int)fb.height / SCREEN_H;

		if (trk.slot[s].ended) {
			printf("up   #%d at %4d,%4d\n", trk.slot[s].id,
			       trk.slot[s].x, trk.slot[s].y);
			continue;
		}
		if (!trk.slot[s].active)
			continue;
		if (trk.slot[s].fresh) {
			printf("down #%d at %4d,%4d (%d finger%s)\n",
			       trk.slot[s].id, trk.slot[s].x, trk.slot[s].y,
			       active, active > 1 ? "s" : "");
		} else {
			moved = 1;
		}
		if (!fb.mem || active >= 3)
			continue;
		if (trk.slot[s].fresh)
			fb_dot(x, y, 14, color);
		else
			fb_line(trk.slot[s].px * (int)fb.width / SCREEN_W,
				trk.slot[s].py * (int)fb.height / SCREEN_H,
				x, y, 6, color);
	}
	if (moved && now_s() - last_move > 0.25) {
		printf("move");
		for (s = 0; s < MAX_POINTS; s++)
			if (trk.slot[s].active)
				printf("  #%d %4d,%4d", trk.slot[s].id,
				       trk.slot[s].x, trk.slot[s].y);
		putchar('\n');
		last_move = now_s();
	}
	fflush(stdout);
}

static struct {
	int fd;
	int reported_touch;
} ui = { .fd = -1 };

static void ui_abs(int code, int min, int max)
{
	struct uinput_abs_setup a = { .code = code };

	a.absinfo.minimum = min;
	a.absinfo.maximum = max;
	if (ioctl(ui.fd, UI_ABS_SETUP, &a))
		perror("UI_ABS_SETUP");
}

static int ui_open(void)
{
	struct uinput_setup us = { 0 };

	ui.fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (ui.fd < 0)
		return -errno;
	ioctl(ui.fd, UI_SET_EVBIT, EV_SYN);
	ioctl(ui.fd, UI_SET_EVBIT, EV_KEY);
	ioctl(ui.fd, UI_SET_EVBIT, EV_ABS);
	ioctl(ui.fd, UI_SET_KEYBIT, BTN_TOUCH);
	ioctl(ui.fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);
	ioctl(ui.fd, UI_SET_ABSBIT, ABS_X);
	ioctl(ui.fd, UI_SET_ABSBIT, ABS_Y);
	ioctl(ui.fd, UI_SET_ABSBIT, ABS_MT_SLOT);
	ioctl(ui.fd, UI_SET_ABSBIT, ABS_MT_TRACKING_ID);
	ioctl(ui.fd, UI_SET_ABSBIT, ABS_MT_POSITION_X);
	ioctl(ui.fd, UI_SET_ABSBIT, ABS_MT_POSITION_Y);
	ui_abs(ABS_X, 0, SCREEN_W - 1);
	ui_abs(ABS_Y, 0, SCREEN_H - 1);
	ui_abs(ABS_MT_SLOT, 0, MAX_POINTS - 1);
	ui_abs(ABS_MT_TRACKING_ID, 0, 0xffff);
	ui_abs(ABS_MT_POSITION_X, 0, SCREEN_W - 1);
	ui_abs(ABS_MT_POSITION_Y, 0, SCREEN_H - 1);
	us.id.bustype = BUS_SPI;
	us.id.vendor = 0x0603;	/* Novatek */
	us.id.product = 0x6532;
	us.id.version = 1;
	snprintf(us.name, sizeof(us.name), "piano NT36532 THP touchscreen");
	if (ioctl(ui.fd, UI_DEV_SETUP, &us) || ioctl(ui.fd, UI_DEV_CREATE)) {
		int ret = -errno;

		close(ui.fd);
		ui.fd = -1;
		return ret;
	}
	return 0;
}

static void ui_emit(int type, int code, int value)
{
	struct input_event ev = { .type = type, .code = code, .value = value };

	if (write(ui.fd, &ev, sizeof(ev)) != sizeof(ev))
		st.ui_write_errors++;
}

static void ui_report(int active)
{
	int s, first = -1;

	for (s = 0; s < MAX_POINTS; s++) {
		if (!trk.slot[s].active && !trk.slot[s].ended)
			continue;
		ui_emit(EV_ABS, ABS_MT_SLOT, s);
		if (trk.slot[s].ended) {
			ui_emit(EV_ABS, ABS_MT_TRACKING_ID, -1);
			continue;
		}
		if (trk.slot[s].fresh)
			ui_emit(EV_ABS, ABS_MT_TRACKING_ID, trk.slot[s].id);
		ui_emit(EV_ABS, ABS_MT_POSITION_X, trk.slot[s].x);
		ui_emit(EV_ABS, ABS_MT_POSITION_Y, trk.slot[s].y);
		if (first < 0)
			first = s;
	}
	if (first >= 0) {
		ui_emit(EV_ABS, ABS_X, trk.slot[first].x);
		ui_emit(EV_ABS, ABS_Y, trk.slot[first].y);
	}
	if (!!active != ui.reported_touch) {
		ui_emit(EV_KEY, BTN_TOUCH, !!active);
		ui.reported_touch = !!active;
	}
	ui_emit(EV_SYN, SYN_REPORT, 0);
}

static void handle_frame(const uint8_t *frame, size_t len)
{
	static double last_print;
	const uint8_t *p;
	size_t plen, nodes;
	int type, rows, cols, n, csum;

	if (len < TRANSPORT_LEN + PAYLOAD_MIN) {
		st.short_payload++;
		return;
	}
	p = frame + TRANSPORT_LEN;
	plen = len - TRANSPORT_LEN;
	type = p[56];
	st.types[type]++;
	st.last_frame_no = le16(p + 28);
	csum = payload_checksum(p, plen);
	if (csum > 0)
		st.csum_ok++;
	else
		st.csum_bad++;

	if (opt.mode == MODE_DUMP) {
		printf("type=%d frame_no=%u cols=%u rows=%u csum=%s crc_len=%d ev=%02x %02x %02x %02x\n",
		       type, le16(p + 28), p[48], p[49],
		       csum > 0 ? "ok" : csum == 0 ? "BAD" : "inval",
		       (int32_t)le32(p + 8), frame[1], frame[2], frame[3], frame[4]);
		return;
	}

	cols = p[48];
	rows = p[49];
	nodes = (size_t)rows * cols;
	if (csum <= 0 || !nodes || nodes > MAX_NODES || 64 + nodes * 2 > plen)
		return;
	if (ref_type < 0) {
		/* lock onto the dominant non-pen matrix type */
		if (type == 6 || type == 7 || type == 9 || type == 0x1d)
			return;
		probe_types[type]++;
		if (++probe_count < TYPE_PROBE_FRAMES)
			return;
		for (n = 0; n < MAX_TYPES; n++)
			if (ref_type < 0 || probe_types[n] > probe_types[ref_type])
				ref_type = n;
		return;
	}
	if (type != ref_type)
		return;	/* other frame types (stylus, ...) */

	if (!have_reference) {
		if (!ref_count) {
			st.rows = rows;
			st.cols = cols;
		}
		if (rows != st.rows || cols != st.cols)
			return;
		for (n = 0; n < (int)nodes; n++)
			ref_samples[ref_count][n] = (int16_t)le16(p + 64 + n * 2);
		if (++ref_count >= opt.ref_frames)
			build_reference(nodes);
		return;
	}
	if (rows != st.rows || cols != st.cols)
		return;

	st.max_delta = 0;
	for (n = 0; n < (int)nodes; n++) {
		int d = (int16_t)le16(p + 64 + n * 2) - reference[n];

		delta[n] = opt.invert ? -d : d;
		if (delta[n] > st.max_delta)
			st.max_delta = delta[n];
	}
	/* follow slow baseline drift while nothing is near the threshold */
	if (st.max_delta < opt.threshold / 2)
		for (n = 0; n < (int)nodes; n++)
			reference[n] += ((int16_t)le16(p + 64 + n * 2) -
					 reference[n]) / 8;

	if (opt.mode == MODE_MAP) {
		if (now_s() - last_print > 0.2) {
			print_map();
			last_print = now_s();
		}
	} else if (opt.mode == MODE_POINTS || opt.mode == MODE_PAINT ||
		   opt.mode == MODE_INPUT) {
		struct point pts[MAX_POINTS];
		int i, count = find_points(pts);

		if (opt.mode == MODE_PAINT) {
			paint(track(pts, count));
			return;
		}
		if (opt.mode == MODE_INPUT) {
			ui_report(track(pts, count));
			return;
		}
		if (count && now_s() - last_print > 0.1) {
			printf("frame %5u:", st.last_frame_no);
			for (i = 0; i < count; i++)
				printf("  [%d] col %.2f row %.2f peak %d", i,
				       pts[i].x, pts[i].y, pts[i].peak);
			putchar('\n');
			fflush(stdout);
			last_print = now_s();
		}
	}
}

static void print_stats(double elapsed)
{
	int t;

	printf("%6.1fs records %lu (bad magic %lu) valid-flag %lu checksum ok %lu bad %lu short %lu types:",
	       elapsed, st.records, st.bad_magic, st.flag_valid, st.csum_ok,
	       st.csum_bad, st.short_payload);
	for (t = 0; t < MAX_TYPES; t++)
		if (st.types[t])
			printf(" %d:%lu", t, st.types[t]);
	printf(" frame_no %u max_delta %d\n", st.last_frame_no, st.max_delta);
	fflush(stdout);
}

static void usage(void)
{
	fputs("usage: piano-touch-view [stats|map|points|paint|dump|input] [--seconds N]\n"
	      "       [--type N] [--threshold T] [--reference N] [--invert]\n"
	      "       [--swap-xy] [--flip-x] [--flip-y]\n", stderr);
	exit(2);
}

int main(int argc, char **argv)
{
	static uint8_t buf[1 << 20];
	size_t have = 0;
	double start, last_stats;
	const char *replay;
	int fd, i, ret, interactive;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "stats"))
			opt.mode = MODE_STATS;
		else if (!strcmp(argv[i], "map"))
			opt.mode = MODE_MAP;
		else if (!strcmp(argv[i], "points"))
			opt.mode = MODE_POINTS;
		else if (!strcmp(argv[i], "paint"))
			opt.mode = MODE_PAINT;
		else if (!strcmp(argv[i], "dump"))
			opt.mode = MODE_DUMP;
		else if (!strcmp(argv[i], "input"))
			opt.mode = MODE_INPUT;
		else if (!strcmp(argv[i], "--seconds") && i + 1 < argc)
			opt.seconds = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--threshold") && i + 1 < argc)
			opt.threshold = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--reference") && i + 1 < argc)
			opt.ref_frames = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--type") && i + 1 < argc)
			opt.type = strtol(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--invert"))
			opt.invert = 1;
		else if (!strcmp(argv[i], "--swap-xy"))
			opt.swap_xy = !opt.swap_xy;
		else if (!strcmp(argv[i], "--flip-x"))
			opt.flip_x = !opt.flip_x;
		else if (!strcmp(argv[i], "--flip-y"))
			opt.flip_y = !opt.flip_y;
		else
			usage();
	}
	if (opt.threshold <= 0 || opt.ref_frames < 1 || opt.ref_frames > MAX_REF ||
	    opt.type >= MAX_TYPES)
		usage();
	ref_type = opt.type;

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	if (opt.mode == MODE_PAINT) {
		ret = fb_open();
		if (ret)
			fprintf(stderr, "no framebuffer painting: %s\n", strerror(-ret));
	}
	if (opt.mode == MODE_INPUT) {
		ret = ui_open();
		if (ret) {
			fprintf(stderr, "/dev/uinput: %s (modprobe uinput)\n",
				strerror(-ret));
			return 1;
		}
		fprintf(stderr, "uinput touchscreen created (%dx%d)\n",
			SCREEN_W, SCREEN_H);
	}

	replay = getenv("PIANO_THP_STREAM");
	fd = open(replay ? replay : STREAM_PATH, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		perror(replay ? replay : STREAM_PATH);
		return 1;
	}
	ret = replay ? 0 : write_control(1);
	if (ret) {
		fprintf(stderr, "%s: %s\n", CONTROL_PATH, strerror(-ret));
		return 1;
	}
	fprintf(stderr, "capturing; keep fingers off the screen for the reference\n");
	if (opt.mode == MODE_PAINT)
		fprintf(stderr, "paint: draw on the screen for %ds; three fingers or "
			"\"c\" + Enter clear, \"q\" + Enter stops\n", opt.seconds);
	interactive = opt.mode == MODE_PAINT && isatty(STDIN_FILENO);

	start = last_stats = now_s();
	while (running && (!opt.seconds || now_s() - start < opt.seconds)) {
		struct pollfd pfd[2] = {
			{ .fd = fd, .events = POLLIN },
			{ .fd = interactive ? STDIN_FILENO : -1, .events = POLLIN },
		};
		ssize_t n;
		size_t off = 0;

		ret = poll(pfd, 2, 200);
		if (ret < 0 && errno != EINTR)
			break;
		if (ret > 0 && (pfd[1].revents & (POLLIN | POLLHUP))) {
			char line[64];

			n = read(STDIN_FILENO, line, sizeof(line));
			if (n <= 0)
				interactive = 0;
			else if (line[0] == 'q')
				running = 0;
			else if (line[0] == 'c') {
				fb_clear();
				printf("canvas cleared\n");
				fflush(stdout);
			}
		}
		if (ret > 0 && (pfd[0].revents & POLLIN)) {
			n = read(fd, buf + have, sizeof(buf) - have);
			if (n > 0)
				have += n;
			else if (!n && replay && have < 32)
				running = 0;	/* end of the replayed file */
		}
		while (have - off >= 32) {
			const uint8_t *h = buf + off;
			uint16_t hlen = le16(h + 4), flen = le16(h + 6);

			if (le32(h) != STREAM_MAGIC || hlen < 32) {
				st.bad_magic++;
				off++;
				continue;
			}
			if (have - off < (size_t)hlen + flen)
				break;
			st.records++;
			if (le16(h + 26) & 1)
				st.flag_valid++;
			handle_frame(h + hlen, flen);
			off += hlen + flen;
		}
		memmove(buf, buf + off, have - off);
		have -= off;

		if (opt.mode == MODE_STATS && now_s() - last_stats >= 1.0) {
			print_stats(now_s() - start);
			last_stats = now_s();
		}
	}

	if (!replay)
		write_control(0);
	close(fd);
	if (ui.fd >= 0) {
		ioctl(ui.fd, UI_DEV_DESTROY);
		close(ui.fd);
	}
	if (fb.mem) {
		fb_close();
		fprintf(stderr, "paint: screen restored\n");
	}
	print_stats(now_s() - start);
	return st.csum_ok ? 0 : 3;
}
