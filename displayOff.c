/*
 * displayOff - switches the monitors off after a while without input, and shows
 * a rolling starfield on request. For mwc (https://github.com/reygnn/mwc) and any
 * other Wayland compositor with the same protocols.
 *
 *   displayOff [-t SECONDS]   stay running: off after that long without input
 *                             (default 10 minutes), on again at the next input
 *   displayOff --now          off now, and at the next input on again and end
 *   displayOff --stars        the starfield on all monitors, until a key, a
 *                             button, the wheel or a clear move of the mouse
 *
 * libwayland only, one file, no toolkit. The two are independent. While the stars
 * are up nobody is typing, so the monitors still go off after the timeout, and
 * nothing is drawn then: the compositor sends no more frames.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "ext-idle-notify-v1-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "wlr-output-power-management-unstable-v1-client-protocol.h"

#define DEFAULT_TIMEOUT 600     /* seconds */
#define NOW_GRACE_MS 1000       /* --now: the hand that chose it is still moving */
#define MOVE_PX 8               /* a hand on the desk moves the mouse less */

/* The starfield, from StarsDemo: 256 lines as on a PAL Amiga, as wide as the
 * monitor's shape asks, and blown up without smoothing. */
#define FB_H 256
#define MAXSTARS 2000
#define ZFAR 1024.0f
#define ZNEAR 1.0f
#define FOV 128.0f
#define SPEED 6.0f
#define FRAME_MS 20             /* 50 Hz, also on a faster monitor */

struct star { float x, y, z, pz; };

/* One wl_shm buffer. */
struct buf {
	struct wl_buffer *wl;
	uint32_t *data;
	size_t size;
	int w, h;
	int busy;       /* the compositor still reads it */
	int stale;      /* wrong size: free it as soon as it is released */
};

/* One monitor. */
struct head {
	struct wl_list link;
	uint32_t name;          /* in the registry */
	struct wl_output *output;
	/* switching it off */
	struct zwlr_output_power_v1 *power;
	/* the stars on it */
	struct wl_surface *surface;
	struct zwlr_layer_surface_v1 *layer;
	struct wl_callback *frame;
	struct buf *bufs[2];
	int w, h;               /* on the screen */
	int fb_w;
	uint32_t *fb;
	int *xmap;              /* the column of fb for each column on the screen */
	struct star *stars;
	int nstars;
	float angle, t;
	uint64_t last_ms;
};

static struct wl_display *dpy;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct zwlr_output_power_manager_v1 *power_mgr;
static struct ext_idle_notifier_v1 *idle_notifier;
static struct wl_list heads;
static int stars_mode;
static int now_mode;
static uint32_t idle_notifier_version;
static int started;             /* the heads known now are set up */
static int is_off;
static volatile sig_atomic_t quit;

static struct wl_pointer *pointer;
static struct wl_keyboard *keyboard;
static int ptr_known, ptr_x0, ptr_y0;

static uint64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- switching off ------------------------------------------------------ */

static void power_mode(void *data, struct zwlr_output_power_v1 *p, uint32_t mode)
{
}

/* It is gone, or the compositor can't do it for this one. */
static void power_failed(void *data, struct zwlr_output_power_v1 *p)
{
	struct head *h = data;
	fprintf(stderr, "displayOff: a monitor can't be switched\n");
	zwlr_output_power_v1_destroy(h->power);
	h->power = NULL;
}

static const struct zwlr_output_power_v1_listener power_listener = {
	.mode = power_mode,
	.failed = power_failed,
};

static void set_all(enum zwlr_output_power_v1_mode mode)
{
	struct head *h;
	wl_list_for_each(h, &heads, link)
		if (h->power)
			zwlr_output_power_v1_set_mode(h->power, mode);
}

static void idled(void *data, struct ext_idle_notification_v1 *n)
{
	if (now_mode)
		return;         /* off already; from here on, input is meant */
	is_off = 1;
	set_all(ZWLR_OUTPUT_POWER_V1_MODE_OFF);
}

static void resumed(void *data, struct ext_idle_notification_v1 *n)
{
	if (now_mode) {
		quit = 1;       /* the end of main switches them on */
		return;
	}
	is_off = 0;
	set_all(ZWLR_OUTPUT_POWER_V1_MODE_ON);
}

static const struct ext_idle_notification_v1_listener idle_listener = {
	.idled = idled,
	.resumed = resumed,
};

/* ---- the stars ---------------------------------------------------------- */

static float frand(float lo, float hi)
{
	return lo + (hi - lo) * (float)rand() / (float)RAND_MAX;
}

static void spawn(const struct head *h, struct star *s, int anywhere)
{
	s->x = frand(-h->fb_w * 4, h->fb_w * 4);
	s->y = frand(-FB_H * 4, FB_H * 4);
	s->z = anywhere ? frand(ZNEAR, ZFAR) : ZFAR;
	s->pz = s->z;
}

static void plot(struct head *h, int x, int y, uint32_t c)
{
	if ((unsigned)x < (unsigned)h->fb_w && (unsigned)y < FB_H)
		h->fb[y * h->fb_w + x] = c;
}

static void line(struct head *h, int x0, int y0, int x1, int y1, uint32_t c)
{
	int dx = x1 - x0, dy = y1 - y0;
	int n = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
	if (n == 0) {
		plot(h, x0, y0, c);
		return;
	}
	if (n > 64)
		n = 64;
	for (int i = 0; i <= n; i++)
		plot(h, x0 + dx * i / n, y0 + dy * i / n, c);
}

static int project(const struct head *h, const struct star *s, float z, float ca,
	float sa, int *sx, int *sy)
{
	float rx = s->x * ca - s->y * sa;
	float ry = s->x * sa + s->y * ca;
	*sx = (int)(rx * FOV / z) + h->fb_w / 2;
	*sy = (int)(ry * FOV / z) + FB_H / 2;
	return (unsigned)*sx < (unsigned)h->fb_w && (unsigned)*sy < FB_H;
}

/* One step, with dt in frames of 50 Hz. */
static void stars_step(struct head *h, float dt)
{
	h->t += dt;
	h->angle += 0.012f * sinf(h->t * 0.004f) * dt + 0.004f * dt;
	float ca = cosf(h->angle), sa = sinf(h->angle);

	/* Fade to 75 %, which leaves trails. */
	for (int i = 0; i < h->fb_w * FB_H; i++) {
		uint32_t c = h->fb[i];
		h->fb[i] = ((c >> 1) & 0x7f7f7f) + ((c >> 2) & 0x3f3f3f);
	}
	for (int i = 0; i < h->nstars; i++) {
		struct star *s = &h->stars[i];
		int x0, y0, x1, y1;
		s->pz = s->z;
		s->z -= SPEED * dt;
		if (s->z < ZNEAR) {
			spawn(h, s, 0);
			continue;
		}
		if (!project(h, s, s->z, ca, sa, &x1, &y1)) {
			spawn(h, s, 0);
			continue;
		}
		project(h, s, s->pz, ca, sa, &x0, &y0);
		/* Closer is brighter, with a faint blue in the distance. */
		float b = 1.0f - s->z / ZFAR;
		int v = (int)(b * b * 255.0f);
		uint32_t c = (uint32_t)(v << 16 | v << 8 | (v + 60 > 255 ? 255 : v + 60));
		line(h, x0, y0, x1, y1, c);
		if (s->z < ZFAR * 0.25f) {
			plot(h, x1 + 1, y1, c);
			plot(h, x1, y1 + 1, c);
		}
	}
}

static void buf_free(struct buf *b)
{
	wl_buffer_destroy(b->wl);
	munmap(b->data, b->size);
	free(b);
}

static void buf_release(void *data, struct wl_buffer *wl)
{
	struct buf *b = data;
	b->busy = 0;
	if (b->stale)
		buf_free(b);
}

static const struct wl_buffer_listener buf_listener = { buf_release };

static struct buf *buf_new(int w, int h)
{
	size_t size = (size_t)w * h * 4;
	struct buf *b = calloc(1, sizeof *b);
	int fd = memfd_create("displayOff", MFD_CLOEXEC);
	void *data = MAP_FAILED;
	if (b && fd >= 0 && ftruncate(fd, size) == 0)
		data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		fprintf(stderr, "displayOff: no memory for a %dx%d buffer: %s\n", w, h,
			strerror(errno));
		exit(1);
	}
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	b->wl = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4,
		WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	wl_buffer_add_listener(b->wl, &buf_listener, b);
	b->data = data;
	b->size = size;
	b->w = w;
	b->h = h;
	return b;
}

/* A free buffer of the size on the screen, or NULL while the compositor has both. */
static struct buf *get_buffer(struct head *h)
{
	for (int i = 0; i < 2; i++) {
		struct buf *b = h->bufs[i];
		if (!b || (b->w == h->w && b->h == h->h))
			continue;
		if (b->busy)
			b->stale = 1;
		else
			buf_free(b);
		h->bufs[i] = NULL;
	}
	for (int i = 0; i < 2; i++)
		if (h->bufs[i] && !h->bufs[i]->busy)
			return h->bufs[i];
	for (int i = 0; i < 2; i++)
		if (!h->bufs[i])
			return h->bufs[i] = buf_new(h->w, h->h);
	return NULL;
}

static void blow_up(const struct head *h, struct buf *b)
{
	int last = -1;
	for (int y = 0; y < h->h; y++) {
		uint32_t *out = b->data + (size_t)y * h->w;
		int sy = (int)((int64_t)y * FB_H / h->h);
		if (sy == last) {
			memcpy(out, out - h->w, (size_t)h->w * 4);
			continue;
		}
		const uint32_t *in = h->fb + (size_t)sy * h->fb_w;
		for (int x = 0; x < h->w; x++)
			out[x] = in[h->xmap[x]];
		last = sy;
	}
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t time);
static const struct wl_callback_listener frame_listener = { frame_done };

/* Draws if it is time and a buffer is free, and asks for the next frame anyway. */
static void head_draw(struct head *h, int force)
{
	uint64_t now = now_ms();
	struct buf *b = NULL;
	if (force || now - h->last_ms >= FRAME_MS)
		b = get_buffer(h);
	if (b) {
		float dt = (float)(now - h->last_ms) / FRAME_MS;
		/* Back from a monitor that was off: go on, don't jump. */
		stars_step(h, dt > 5.0f ? 5.0f : dt);
		h->last_ms = now;
		blow_up(h, b);
		wl_surface_attach(h->surface, b->wl, 0, 0);
		wl_surface_damage_buffer(h->surface, 0, 0, b->w, b->h);
		b->busy = 1;
	}
	if (h->frame)
		wl_callback_destroy(h->frame);
	h->frame = wl_surface_frame(h->surface);
	wl_callback_add_listener(h->frame, &frame_listener, h);
	wl_surface_commit(h->surface);
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
	struct head *h = data;
	wl_callback_destroy(cb);
	h->frame = NULL;
	head_draw(h, 0);
}

static void head_resize(struct head *h, int w, int h_)
{
	h->w = w;
	h->h = h_;
	h->fb_w = (int)((float)FB_H * w / h_ + 0.5f);
	if (h->fb_w < 160)
		h->fb_w = 160;
	h->nstars = 600 * h->fb_w / 320;
	if (h->nstars > MAXSTARS)
		h->nstars = MAXSTARS;
	free(h->fb);
	free(h->xmap);
	free(h->stars);
	h->fb = calloc((size_t)h->fb_w * FB_H, sizeof *h->fb);
	h->xmap = malloc((size_t)w * sizeof *h->xmap);
	h->stars = malloc((size_t)h->nstars * sizeof *h->stars);
	if (!h->fb || !h->xmap || !h->stars) {
		fprintf(stderr, "displayOff: out of memory\n");
		exit(1);
	}
	for (int x = 0; x < w; x++)
		h->xmap[x] = (int)((int64_t)x * h->fb_w / w);
	for (int i = 0; i < h->nstars; i++)
		spawn(h, &h->stars[i], 1);
	h->last_ms = now_ms();
}

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *l,
	uint32_t serial, uint32_t w, uint32_t hh)
{
	struct head *h = data;
	zwlr_layer_surface_v1_ack_configure(l, serial);
	if (w == 0 || hh == 0) {
		fprintf(stderr, "displayOff: the compositor gave no size\n");
		exit(1);
	}
	if ((int)w != h->w || (int)hh != h->h)
		head_resize(h, w, hh);
	head_draw(h, 1);        /* a configure wants its commit, frames or not */
}

/* Stops showing the stars on this monitor, and at the end stops altogether. */
static void head_stop_stars(struct head *h)
{
	if (h->frame)
		wl_callback_destroy(h->frame);
	h->frame = NULL;
	if (h->layer)
		zwlr_layer_surface_v1_destroy(h->layer);
	h->layer = NULL;
	if (h->surface)
		wl_surface_destroy(h->surface);
	h->surface = NULL;
	for (int i = 0; i < 2; i++) {
		if (!h->bufs[i])
			continue;
		if (h->bufs[i]->busy)
			h->bufs[i]->stale = 1;
		else
			buf_free(h->bufs[i]);
		h->bufs[i] = NULL;
	}
	free(h->fb);
	free(h->xmap);
	free(h->stars);
	h->fb = NULL;
	h->xmap = NULL;
	h->stars = NULL;
	h->w = h->h = 0;
}

static void end_if_no_stars(void)
{
	struct head *h;
	if (!stars_mode)
		return;
	wl_list_for_each(h, &heads, link)
		if (h->surface)
			return;
	quit = 1;
}

/* The compositor took it away, as with an unplugged monitor. */
static void layer_closed(void *data, struct zwlr_layer_surface_v1 *l)
{
	head_stop_stars(data);
	end_if_no_stars();
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
	.configure = layer_configure,
	.closed = layer_closed,
};

/* ---- monitors ----------------------------------------------------------- */

static void head_start(struct head *h)
{
	if (!stars_mode) {
		h->power = zwlr_output_power_manager_v1_get_output_power(power_mgr,
			h->output);
		zwlr_output_power_v1_add_listener(h->power, &power_listener, h);
		if (is_off)
			zwlr_output_power_v1_set_mode(h->power,
				ZWLR_OUTPUT_POWER_V1_MODE_OFF);
		return;
	}
	h->surface = wl_compositor_create_surface(compositor);
	h->layer = zwlr_layer_shell_v1_get_layer_surface(layer_shell, h->surface,
		h->output, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "displayOff");
	zwlr_layer_surface_v1_add_listener(h->layer, &layer_listener, h);
	zwlr_layer_surface_v1_set_anchor(h->layer,
		ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
	zwlr_layer_surface_v1_set_exclusive_zone(h->layer, -1); /* over panels too */
	zwlr_layer_surface_v1_set_keyboard_interactivity(h->layer,
		ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
	wl_surface_commit(h->surface);
}

static void head_remove(struct head *h)
{
	if (h->power)
		zwlr_output_power_v1_destroy(h->power);
	head_stop_stars(h);
	wl_output_destroy(h->output);
	wl_list_remove(&h->link);
	free(h);
}

/* ---- input: anything ends the stars ------------------------------------- */

static void ptr_enter(void *data, struct wl_pointer *p, uint32_t serial,
	struct wl_surface *s, wl_fixed_t x, wl_fixed_t y)
{
	wl_pointer_set_cursor(p, serial, NULL, 0, 0);
	if (!ptr_known) {
		ptr_known = 1;
		ptr_x0 = wl_fixed_to_int(x);
		ptr_y0 = wl_fixed_to_int(y);
	}
}

static void ptr_leave(void *data, struct wl_pointer *p, uint32_t serial,
	struct wl_surface *s)
{
	ptr_known = 0;  /* the next monitor has other coordinates */
}

static void ptr_motion(void *data, struct wl_pointer *p, uint32_t time, wl_fixed_t x,
	wl_fixed_t y)
{
	if (ptr_known && (abs(wl_fixed_to_int(x) - ptr_x0) > MOVE_PX ||
	    abs(wl_fixed_to_int(y) - ptr_y0) > MOVE_PX))
		quit = 1;
}

static void ptr_button(void *data, struct wl_pointer *p, uint32_t serial,
	uint32_t time, uint32_t button, uint32_t state)
{
	if (state == WL_POINTER_BUTTON_STATE_PRESSED)
		quit = 1;
}

static void ptr_axis(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis,
	wl_fixed_t value)
{
	quit = 1;
}

/* Seat version 1 sends no more than these. */
static const struct wl_pointer_listener pointer_listener = {
	.enter = ptr_enter,
	.leave = ptr_leave,
	.motion = ptr_motion,
	.button = ptr_button,
	.axis = ptr_axis,
};

static void kb_keymap(void *data, struct wl_keyboard *k, uint32_t format, int32_t fd,
	uint32_t size)
{
	close(fd);
}

static void kb_enter(void *data, struct wl_keyboard *k, uint32_t serial,
	struct wl_surface *s, struct wl_array *keys)
{
}

static void kb_leave(void *data, struct wl_keyboard *k, uint32_t serial,
	struct wl_surface *s)
{
}

/* Only a press: the key that started us is still going to be released. */
static void kb_key(void *data, struct wl_keyboard *k, uint32_t serial, uint32_t time,
	uint32_t key, uint32_t state)
{
	if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
		quit = 1;
}

static void kb_modifiers(void *data, struct wl_keyboard *k, uint32_t serial,
	uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group)
{
}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = kb_keymap,
	.enter = kb_enter,
	.leave = kb_leave,
	.key = kb_key,
	.modifiers = kb_modifiers,
};

static void seat_capabilities(void *data, struct wl_seat *s, uint32_t caps)
{
	if (!stars_mode)
		return;
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
		pointer = wl_seat_get_pointer(s);
		wl_pointer_add_listener(pointer, &pointer_listener, NULL);
	}
	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !keyboard) {
		keyboard = wl_seat_get_keyboard(s);
		wl_keyboard_add_listener(keyboard, &keyboard_listener, NULL);
	}
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
};

/* ---- setup -------------------------------------------------------------- */

static void registry_global(void *data, struct wl_registry *r, uint32_t name,
	const char *iface, uint32_t ver)
{
	if (!strcmp(iface, wl_compositor_interface.name)) {
		/* 4 for wl_surface.damage_buffer */
		compositor = wl_registry_bind(r, name, &wl_compositor_interface,
			ver < 4 ? ver : 4);
	} else if (!strcmp(iface, wl_shm_interface.name)) {
		shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	} else if (!strcmp(iface, wl_seat_interface.name) && !seat) {
		seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
		wl_seat_add_listener(seat, &seat_listener, NULL);
	} else if (!strcmp(iface, zwlr_layer_shell_v1_interface.name)) {
		layer_shell = wl_registry_bind(r, name, &zwlr_layer_shell_v1_interface,
			ver < 4 ? ver : 4);
	} else if (!strcmp(iface, zwlr_output_power_manager_v1_interface.name)) {
		power_mgr = wl_registry_bind(r, name,
			&zwlr_output_power_manager_v1_interface, 1);
	} else if (!strcmp(iface, ext_idle_notifier_v1_interface.name)) {
		idle_notifier_version = ver < 2 ? ver : 2;
		idle_notifier = wl_registry_bind(r, name,
			&ext_idle_notifier_v1_interface, idle_notifier_version);
	} else if (!strcmp(iface, wl_output_interface.name)) {
		struct head *h = calloc(1, sizeof *h);
		if (!h) {
			fprintf(stderr, "displayOff: out of memory\n");
			exit(1);
		}
		h->name = name;
		h->output = wl_registry_bind(r, name, &wl_output_interface, 1);
		wl_list_insert(&heads, &h->link);
		if (started)
			head_start(h);
	}
}

static void registry_remove(void *data, struct wl_registry *r, uint32_t name)
{
	struct head *h;
	wl_list_for_each(h, &heads, link) {
		if (h->name == name) {
			head_remove(h);
			end_if_no_stars();
			return;
		}
	}
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_remove,
};

static void on_signal(int sig)
{
	quit = 1;
}

static void usage(FILE *f)
{
	fprintf(f, "usage: displayOff [-t SECONDS]   monitors off after that long "
		"without input\n"
		"                                 (default %d)\n"
		"       displayOff --now         off now, until the next input\n"
		"       displayOff --stars       a starfield until the next input\n",
		DEFAULT_TIMEOUT);
}

static void lost_connection(void)
{
	fprintf(stderr, "displayOff: lost the connection to the compositor: %s\n",
		strerror(wl_display_get_error(dpy)));
	exit(1);
}

int main(int argc, char **argv)
{
	long timeout = DEFAULT_TIMEOUT;
	if (argc == 2 && !strcmp(argv[1], "--stars")) {
		stars_mode = 1;
	} else if (argc == 2 && !strcmp(argv[1], "--now")) {
		now_mode = 1;
	} else if (argc == 3 && !strcmp(argv[1], "-t")) {
		char *end;
		errno = 0;
		timeout = strtol(argv[2], &end, 10);
		/* In milliseconds it has to fit 32 bits. */
		if (errno || end == argv[2] || *end || timeout < 1 || timeout > 4000000) {
			fprintf(stderr, "displayOff: bad number of seconds: %s\n", argv[2]);
			return 1;
		}
	} else if (argc == 2 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
		usage(stdout);
		return 0;
	} else if (argc != 1) {
		usage(stderr);
		return 1;
	}

	/* Blocked except in the wait, so no signal comes between checking and waiting. */
	sigset_t block, wait_mask;
	sigemptyset(&block);
	sigaddset(&block, SIGTERM);
	sigaddset(&block, SIGINT);
	sigaddset(&block, SIGHUP);
	sigprocmask(SIG_BLOCK, &block, &wait_mask);
	struct sigaction sa = { .sa_handler = on_signal };
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);

	if (!(dpy = wl_display_connect(NULL))) {
		fprintf(stderr, "displayOff: cannot connect to a Wayland compositor\n");
		return 1;
	}
	wl_list_init(&heads);
	struct wl_registry *registry = wl_display_get_registry(dpy);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(dpy);
	if (stars_mode && (!compositor || !shm || !layer_shell)) {
		fprintf(stderr, "displayOff: the compositor lacks wl_compositor, wl_shm "
			"or wlr-layer-shell\n");
		return 1;
	}
	if (!stars_mode && (!seat || !power_mgr || !idle_notifier)) {
		fprintf(stderr, "displayOff: the compositor lacks a seat, "
			"wlr-output-power-management or ext-idle-notify\n");
		return 1;
	}
	/* Version 2 hears input also while a video player keeps everyone from being
	 * idle. Otherwise nothing would switch the monitors on again. */
	if (now_mode && idle_notifier_version < 2) {
		fprintf(stderr, "displayOff: --now needs ext-idle-notify version 2\n");
		return 1;
	}
	if (stars_mode && wl_list_empty(&heads)) {
		fprintf(stderr, "displayOff: no monitors\n");
		return 1;
	}

	srand((unsigned)time(NULL));
	struct head *h;
	is_off = now_mode;
	wl_list_for_each(h, &heads, link)
		head_start(h);
	started = 1;
	if (now_mode) {
		struct ext_idle_notification_v1 *n =
			ext_idle_notifier_v1_get_input_idle_notification(idle_notifier,
				NOW_GRACE_MS, seat);
		ext_idle_notification_v1_add_listener(n, &idle_listener, NULL);
	} else if (!stars_mode) {
		struct ext_idle_notification_v1 *n =
			ext_idle_notifier_v1_get_idle_notification(idle_notifier,
				(uint32_t)timeout * 1000, seat);
		ext_idle_notification_v1_add_listener(n, &idle_listener, NULL);
	}

	int fd = wl_display_get_fd(dpy);
	while (!quit) {
		if (wl_display_dispatch_pending(dpy) < 0)
			lost_connection();
		if (quit)
			break;
		if (wl_display_prepare_read(dpy) != 0)
			continue;       /* events arrived in the meantime */
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		if (wl_display_flush(dpy) < 0) {
			if (errno != EAGAIN) {
				wl_display_cancel_read(dpy);
				lost_connection();
			}
			pfd.events |= POLLOUT;
		}
		if (ppoll(&pfd, 1, NULL, &wait_mask) > 0 &&
		    (pfd.revents & (POLLIN | POLLERR | POLLHUP))) {
			if (wl_display_read_events(dpy) < 0)
				lost_connection();
		} else {
			wl_display_cancel_read(dpy);
		}
	}

	/* Nobody else would switch them on again. */
	if (is_off) {
		set_all(ZWLR_OUTPUT_POWER_V1_MODE_ON);
		wl_display_roundtrip(dpy);
	}
	return 0;
}
