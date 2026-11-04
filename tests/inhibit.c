/* For the test: asks not to be idle until it is killed, as a video player does. */
#include <stdio.h>
#include <string.h>
#include <wayland-client.h>

#include "idle-inhibit-unstable-v1-client-protocol.h"

static struct wl_compositor *compositor;
static struct zwp_idle_inhibit_manager_v1 *manager;

static void global(void *data, struct wl_registry *r, uint32_t name, const char *iface,
	uint32_t ver)
{
	if (!strcmp(iface, wl_compositor_interface.name))
		compositor = wl_registry_bind(r, name, &wl_compositor_interface, 1);
	else if (!strcmp(iface, zwp_idle_inhibit_manager_v1_interface.name))
		manager = wl_registry_bind(r, name,
			&zwp_idle_inhibit_manager_v1_interface, 1);
}

static void global_remove(void *data, struct wl_registry *r, uint32_t name)
{
}

static const struct wl_registry_listener listener = { global, global_remove };

int main(void)
{
	struct wl_display *dpy = wl_display_connect(NULL);
	if (!dpy)
		return 1;
	wl_registry_add_listener(wl_display_get_registry(dpy), &listener, NULL);
	wl_display_roundtrip(dpy);
	if (!compositor || !manager) {
		fprintf(stderr, "inhibit: the compositor lacks idle-inhibit\n");
		return 1;
	}
	zwp_idle_inhibit_manager_v1_create_inhibitor(manager,
		wl_compositor_create_surface(compositor));
	while (wl_display_dispatch(dpy) >= 0)
		;
	return 0;
}
