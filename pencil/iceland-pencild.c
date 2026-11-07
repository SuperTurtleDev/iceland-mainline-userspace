/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "nvt-netlink.h"
#include "pen-protocol.h"
#include "nt36536.h"

#include <errno.h>
#include <gio/gio.h>
#include <glib-unix.h>
#include <linux/netlink.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define BLUEZ "org.bluez"
#define DEVICE "org.bluez.Device1"
#define GATT "org.bluez.GattCharacteristic1"
#define UUID_CONTROL "0783b03e-8535-b5a0-7140-a304d2495cbb"
#define UUID_PDU "0783b03e-8535-b5a0-7140-a304d2495cbc"
#define UUID_BUTTON "180938ac-2a69-11ec-8d3d-0242ac130003"

struct ble_request;

struct app {
	GMainLoop *loop;
	GDBusConnection *bus;
	GDBusObjectManager *objects;
	struct nvt_link commands, events;
	char *address, *device, *control, *pdu, *button;
	guint configure_source;
	int uevent_fd;
	gboolean connecting, subscribing, subscribed, initialized, pressure_known;
	gboolean button_subscribed;
	gboolean pressure;
	unsigned int pressure_packets;
	unsigned long uplinks, downlinks, unknown;
	GQueue writes;
	struct ble_request *inflight;
};

struct ble_request {
	struct app *app;
	GBytes *bytes;
	char *path;
	gboolean pdu;
};

static void configure(struct app *app);
static void write_next(struct app *app);

static void dump(const char *direction, const uint8_t *data, size_t len)
{
	GString *s = g_string_sized_new(len * 2);
	for (size_t i = 0; i < len; i++)
		g_string_append_printf(s, "%02x", data[i]);
	g_debug("%s %s", direction, s->str);
	g_string_free(s, TRUE);
}

static gboolean property_bool(GDBusProxy *proxy, const char *name)
{
	GVariant *v = g_dbus_proxy_get_cached_property(proxy, name);
	gboolean value = v && g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN) && g_variant_get_boolean(v);
	if (v)
		g_variant_unref(v);
	return value;
}

static char *property_string(GDBusProxy *proxy, const char *name)
{
	GVariant *v = g_dbus_proxy_get_cached_property(proxy, name);
	char *value = v ? g_variant_dup_string(v, NULL) : NULL;
	if (v)
		g_variant_unref(v);
	return value;
}

static int touch_write(struct app *app, const uint8_t *data, size_t len)
{
	if (nvt_send(&app->commands, data, len) < 0) {
		if (errno == EHOSTDOWN)
			g_debug("touch firmware asleep/not ready; command not applied");
		else
			g_warning("touch command: %s", strerror(errno));
		return -1;
	}
	dump("touch-write", data, len);
	return 0;
}

static void write_free(gpointer data)
{
	struct ble_request *request = data;
	g_bytes_unref(request->bytes);
	g_free(request->path);
	g_free(request);
}

static void write_done(GObject *object, GAsyncResult *result, gpointer user_data)
{
	struct ble_request *request = user_data;
	struct app *app = request->app;
	GError *error = NULL;
	GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(object), result, &error);
	if (!reply) {
		g_warning("BLE WriteValue: %s", error->message);
		g_error_free(error);
	} else {
		g_variant_unref(reply);
	}
	app->inflight = NULL;
	write_free(request);
	write_next(app);
}

static void write_next(struct app *app)
{
	if (app->inflight || !app->subscribed)
		return;
	struct ble_request *request = g_queue_pop_head(&app->writes);
	if (!request)
		return;
	app->inflight = request;
	gsize len;
	const uint8_t *data = g_bytes_get_data(request->bytes, &len);
	GVariantBuilder options;
	g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&options, "{sv}", "type", g_variant_new_string(request->pdu ? "command" : "request"));
	GVariant *bytes = g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, data, len, 1);
	g_dbus_connection_call(app->bus, BLUEZ, request->path, GATT, "WriteValue",
		g_variant_new("(@aya{sv})", bytes, &options), NULL,
		G_DBUS_CALL_FLAGS_NONE, 5000, NULL, write_done, request);
	dump(request->pdu ? "ble-pdu-write" : "ble-write", data, len);
}

static void ble_write(struct app *app, const uint8_t *data, size_t len, gboolean pdu)
{
	const char *path = pdu ? app->pdu : app->control;
	if (!app->subscribed || !path)
		return;
	/* ATT allows one outstanding request; advance only on WriteValue completion. */
	if (g_queue_get_length(&app->writes) >= 64) {
		g_warning("BLE write queue full; dropping packet");
		return;
	}
	struct ble_request *request = g_new0(struct ble_request, 1);
	request->app = app;
	request->bytes = g_bytes_new(data, len);
	request->path = g_strdup(path);
	request->pdu = pdu;
	g_queue_push_tail(&app->writes, request);
	write_next(app);
}

static void initialize(struct app *app)
{
	const uint8_t enable[] = {0x7f,0x10,1};
	const uint8_t connected[] = {0x7f,0x40,0x20};
	const uint8_t size_query[] = {0xaa,1,2,0};
	const uint8_t sources[] = {'N','V','T',0,NVT_PEN_LOCAL_SOURCES,
		NVT_PEN_SOURCE_PRESSURE |
		(app->button_subscribed ? NVT_PEN_SOURCE_STYLUS : 0)};
	const uint8_t pressure_zero[] = {'N','V','T',0,NVT_PEN_LOCAL_PRESSURE,0,0};
	const uint8_t buttons_zero[] = {'N','V','T',0,NVT_PEN_LOCAL_BUTTONS,0};
	if (!app->subscribed || app->initialized)
		return;
	if (touch_write(app, enable, sizeof(enable)) < 0 ||
	    touch_write(app, connected, sizeof(connected)) < 0 ||
	    touch_write(app, sources, sizeof(sources)) < 0 ||
	    touch_write(app, pressure_zero, sizeof(pressure_zero)) < 0 ||
	    (app->button_subscribed && touch_write(app, buttons_zero, sizeof(buttons_zero)) < 0))
		return;
	app->initialized = TRUE;
	app->pressure_known = FALSE;
	app->pressure_packets = 0;
	ble_write(app, size_query, sizeof(size_query), FALSE);
	g_message("touch bridge initialized for %s; awaiting real timing/ACKs", app->address);
}

static void release_pressure(struct app *app)
{
	const uint8_t zero[] = {'N','V','T',0,NVT_PEN_LOCAL_PRESSURE,0,0};
	const uint8_t buttons_zero[] = {'N','V','T',0,NVT_PEN_LOCAL_BUTTONS,0};

	if (app->initialized) {
		touch_write(app, zero, sizeof(zero));
		if (app->button_subscribed)
			touch_write(app, buttons_zero, sizeof(buttons_zero));
	}
}

static void notification(struct app *app, const uint8_t *data, size_t len)
{
	struct pen_command command;
	dump("ble-notify", data, len);
	int ret = pen_downlink(data, len, &command);
	if (ret <= 0) {
		app->unknown++;
		g_debug("BLE message %s (len=%zu)", ret < 0 ? "malformed" : "unmapped", len);
		return;
	}
	app->downlinks++;
	if (command.kind == PEN_PRESSURE) {
		/* OEM libbluetooth_jni sends one empty PDU per six pressure reports. */
		if (++app->pressure_packets == 6) {
			const uint8_t empty[] = {0xaa,2,0,0};
			ble_write(app, empty, sizeof(empty), TRUE);
			app->pressure_packets = 0;
		}
		if (!app->initialized)
			return;
		/* Inject every value; the IC only receives the OEM presence transition. */
		if (touch_write(app, command.data, command.len) < 0)
			return;
		gboolean pressed = command.data[5] || command.data[6];
		if (app->pressure_known && app->pressure == pressed)
			return;
		const uint8_t presence[] = {0x7f,0x40,pressed ? 0x27 : 0x07};
		if (!touch_write(app, presence, sizeof(presence))) {
			app->pressure_known = TRUE;
			app->pressure = pressed;
		}
	} else {
		if (!touch_write(app, command.data, command.len) && data[1] == 2)
			g_message("%s response=%u delivered to touch IC",
				  data[2] == 1 ? "timing" : "hop", data[4]);
	}
}

static gboolean raw_ready(gint fd, GIOCondition condition, gpointer user_data)
{
	struct app *app = user_data;
	(void)fd;
	if (condition & (G_IO_HUP | G_IO_ERR)) {
		g_warning("touch event socket failed");
		g_main_loop_quit(app->loop);
		return G_SOURCE_REMOVE;
	}
	for (;;) {
		uint8_t frame[300], packet[80];
		int n = nvt_receive(&app->events, frame, sizeof(frame));
		if (n < 0) {
			if (errno != EAGAIN && errno != EWOULDBLOCK)
				g_warning("touch receive: %s", strerror(errno));
			break;
		}
		int len = pen_uplink(frame, n, packet, sizeof(packet));
		if (len > 0) {
			app->uplinks++;
			ble_write(app, packet, len, FALSE);
		} else {
			app->unknown++;
			dump("unmapped-touch-frame", frame, n);
		}
	}
	return G_SOURCE_CONTINUE;
}

static gboolean configure_idle(gpointer user_data)
{
	struct app *app = user_data;
	app->configure_source = 0;
	configure(app);
	return G_SOURCE_REMOVE;
}

static void schedule_configure(struct app *app)
{
	if (!app->configure_source)
		app->configure_source = g_idle_add(configure_idle, app);
}

static void button_notify_done(GObject *object, GAsyncResult *result, gpointer user_data)
{
	struct app *app = user_data;
	GError *error = NULL;
	GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(object), result, &error);
	app->subscribing = FALSE;
	if (!reply) {
		g_warning("BLE button StartNotify: %s", error->message);
		g_error_free(error);
	} else {
		g_variant_unref(reply);
		app->button_subscribed = TRUE;
		/* Re-select sources if the control channel was initialized earlier. */
		app->initialized = FALSE;
		g_message("BLE side-button notification subscription ready");
	}
	initialize(app);
}

static void notify_done(GObject *object, GAsyncResult *result, gpointer user_data)
{
	struct app *app = user_data;
	GError *error = NULL;
	GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(object), result, &error);
	app->subscribing = FALSE;
	if (!reply) {
		g_warning("BLE StartNotify: %s", error->message);
		g_error_free(error);
		return;
	}
	g_variant_unref(reply);
	app->subscribed = TRUE;
	g_message("BLE control notification subscription ready");
	schedule_configure(app);
}

static void connect_done(GObject *object, GAsyncResult *result, gpointer user_data)
{
	struct app *app = user_data;
	GError *error = NULL;
	GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(object), result, &error);
	app->connecting = FALSE;
	if (reply) {
		g_variant_unref(reply);
		schedule_configure(app);
	} else {
		g_warning("BLE Connect: %s", error->message);
		g_error_free(error);
	}
}

static void read_address(struct app *app)
{
	if (app->address)
		return;
	char *text = NULL;
	if (g_file_get_contents("/sys/class/power_supply/oplus-pencil/ble_mac_addr", &text, NULL, NULL)) {
		g_strstrip(text);
		unsigned int b[6];
		if (strlen(text) == 17 && sscanf(text, "%2x:%2x:%2x:%2x:%2x:%2x",
			&b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6 &&
		    (b[0] | b[1] | b[2] | b[3] | b[4] | b[5]))
			app->address = g_ascii_strup(text, -1);
		g_free(text);
	}
}

static gboolean is_paired_pen(GDBusProxy *proxy)
{
	if (!property_bool(proxy, "Paired"))
		return FALSE;
	GVariant *uuids = g_dbus_proxy_get_cached_property(proxy, "UUIDs");
	if (!uuids)
		return FALSE;
	GVariantIter it;
	const char *uuid;
	gboolean found = FALSE;
	g_variant_iter_init(&it, uuids);
	while (g_variant_iter_next(&it, "&s", &uuid))
		if (!g_ascii_strcasecmp(uuid, "0783b03e-8535-b5a0-7140-a304d2495cb7"))
			found = TRUE;
	g_variant_unref(uuids);
	return found;
}

static void configure(struct app *app)
{
	read_address(app);
	GList *objects = g_dbus_object_manager_get_objects(app->objects);
	GDBusProxy *device = NULL;
	/* Detached pens clear the kernel MAC; recover one unique bonded pen. */
	if (!app->address) {
		char *candidate = NULL;
		unsigned int matches = 0;
		for (GList *l = objects; l; l = l->next) {
			GDBusInterface *iface = g_dbus_object_get_interface(l->data, DEVICE);
			if (!iface)
				continue;
			if (is_paired_pen(G_DBUS_PROXY(iface))) {
				matches++;
				g_free(candidate);
				candidate = property_string(G_DBUS_PROXY(iface), "Address");
			}
			g_object_unref(iface);
		}
		if (matches == 1) {
			app->address = candidate;
			g_message("recovered paired pencil %s from BlueZ", candidate);
		} else {
			g_free(candidate);
			goto out;
		}
	}
	for (GList *l = objects; l; l = l->next) {
		GDBusInterface *iface = g_dbus_object_get_interface(l->data, DEVICE);
		if (!iface)
			continue;
		char *address = property_string(G_DBUS_PROXY(iface), "Address");
		if (address && !g_ascii_strcasecmp(address, app->address)) {
			device = G_DBUS_PROXY(iface);
			g_free(address);
			break;
		}
		g_free(address);
		g_object_unref(iface);
	}
	if (!device)
		goto out;
	g_free(app->device);
	app->device = g_strdup(g_dbus_proxy_get_object_path(device));
	if (!property_bool(device, "Connected")) {
		release_pressure(app);
		app->initialized = app->subscribed = app->pressure_known = FALSE;
		app->button_subscribed = FALSE;
		g_queue_clear_full(&app->writes, write_free);
		if (!app->connecting && property_bool(device, "Paired")) {
			app->connecting = TRUE;
			g_message("connecting paired pencil %s", app->address);
			g_dbus_connection_call(app->bus, BLUEZ, app->device, DEVICE, "Connect", NULL,
				NULL, G_DBUS_CALL_FLAGS_NONE, 15000, NULL, connect_done, app);
		}
		goto out;
	}
	if (!property_bool(device, "ServicesResolved") || app->subscribing)
		goto out;
	g_clear_pointer(&app->control, g_free);
	g_clear_pointer(&app->pdu, g_free);
	g_clear_pointer(&app->button, g_free);
	for (GList *l = objects; l; l = l->next) {
		const char *path = g_dbus_object_get_object_path(l->data);
		if (!g_str_has_prefix(path, app->device))
			continue;
		GDBusInterface *iface = g_dbus_object_get_interface(l->data, GATT);
		if (!iface)
			continue;
		char *uuid = property_string(G_DBUS_PROXY(iface), "UUID");
		if (uuid && !g_ascii_strcasecmp(uuid, UUID_CONTROL))
			app->control = g_strdup(path);
		if (uuid && !g_ascii_strcasecmp(uuid, UUID_PDU))
			app->pdu = g_strdup(path);
		if (uuid && !g_ascii_strcasecmp(uuid, UUID_BUTTON))
			app->button = g_strdup(path);
		g_free(uuid);
		g_object_unref(iface);
	}
	if (app->control && app->pdu && !app->subscribed) {
		app->subscribing = TRUE;
		g_dbus_connection_call(app->bus, BLUEZ, app->control, GATT, "StartNotify", NULL,
			NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, notify_done, app);
	} else if (app->subscribed && app->button && !app->button_subscribed) {
		/* Serialize CCCD writes before starting the ordinary control writes. */
		app->subscribing = TRUE;
		g_dbus_connection_call(app->bus, BLUEZ, app->button, GATT, "StartNotify", NULL,
			NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, button_notify_done, app);
	} else {
		initialize(app);
	}
out:
	if (device)
		g_object_unref(device);
	g_list_free_full(objects, g_object_unref);
}

static void properties_changed(GDBusObjectManagerClient *manager, GDBusObjectProxy *object,
		GDBusProxy *proxy, GVariant *changed, const gchar *const *invalidated, gpointer user_data)
{
	struct app *app = user_data;
	(void)manager;
	(void)invalidated;
	const char *path = g_dbus_object_get_object_path(G_DBUS_OBJECT(object));
	const char *interface = g_dbus_proxy_get_interface_name(proxy);
	if (!strcmp(interface, DEVICE)) {
		if (app->device && !strcmp(path, app->device) && !property_bool(proxy, "Connected")) {
			const uint8_t disconnected[] = {0x7f,0x40,0};
			release_pressure(app);
			if (app->initialized)
				touch_write(app, disconnected, sizeof(disconnected));
			app->initialized = app->subscribed = app->pressure_known = FALSE;
			app->button_subscribed = FALSE;
			g_queue_clear_full(&app->writes, write_free);
		}
		schedule_configure(app);
	} else if (!strcmp(interface, GATT) &&
		   ((app->control && !strcmp(path, app->control)) ||
		    (app->button && !strcmp(path, app->button)))) {
		GVariant *value = g_variant_lookup_value(changed, "Value", G_VARIANT_TYPE_BYTESTRING);
		if (value) {
			gsize len;
			const uint8_t *data = g_variant_get_fixed_array(value, &len, 1);
			if (app->button && !strcmp(path, app->button)) {
				struct pen_command command;
				dump("ble-button", data, len);
				if (app->initialized && app->button_subscribed &&
				    pen_button(data, len, &command) > 0)
					touch_write(app, command.data, command.len);
			} else {
				notification(app, data, len);
			}
			g_variant_unref(value);
		}
	}
}

static void object_changed(GDBusObjectManager *manager, GDBusObject *object, gpointer user_data)
{
	(void)manager;
	(void)object;
	schedule_configure(user_data);
}

static void object_removed(GDBusObjectManager *manager, GDBusObject *object, gpointer user_data)
{
	struct app *app = user_data;
	const char *path = g_dbus_object_get_object_path(object);
	(void)manager;
	if ((app->device && !strcmp(path, app->device)) ||
	    (app->control && !strcmp(path, app->control))) {
		release_pressure(app);
		app->initialized = app->subscribed = app->pressure_known = FALSE;
		app->button_subscribed = FALSE;
		g_queue_clear_full(&app->writes, write_free);
	}
	schedule_configure(app);
}

static gboolean uevent_ready(gint fd, GIOCondition condition, gpointer user_data)
{
	struct app *app = user_data;
	struct sockaddr_nl sender = {0};
	socklen_t sender_len = sizeof(sender);
	gboolean pencil = FALSE, ready = FALSE, spi = FALSE, change = FALSE;
	(void)condition;
	char buf[8192];
	ssize_t n = recvfrom(fd, buf, sizeof(buf) - 1, MSG_DONTWAIT,
		(void *)&sender, &sender_len);
	if (n <= 0 || sender.nl_pid != 0)
		return G_SOURCE_CONTINUE;
	buf[n] = 0;
	for (char *field = buf; field < buf + n; field += strlen(field) + 1) {
		pencil |= !strcmp(field, "POWER_SUPPLY_NAME=oplus-pencil");
		ready |= !strcmp(field, "NVT_FIRMWARE=ready");
		spi |= !strcmp(field, "SUBSYSTEM=spi");
		change |= !strcmp(field, "ACTION=change");
	}
	if (pencil)
		schedule_configure(app);
	if (ready && spi && change) {
		/* Panel modesets and firmware recovery both erase the IC's pen state. */
		app->initialized = app->pressure_known = FALSE;
		g_queue_clear_full(&app->writes, write_free);
		g_message("touch firmware ready; reconfiguring pencil");
		initialize(app);
	}
	return G_SOURCE_CONTINUE;
}

static gboolean quit(gpointer user_data)
{
	struct app *app = user_data;
	g_main_loop_quit(app->loop);
	return G_SOURCE_REMOVE;
}

int main(int argc, char **argv)
{
	struct app app = { .commands.fd = -1, .events.fd = -1, .uevent_fd = -1 };
	GError *error = NULL;
	gint duration = 0;
	gboolean debug = FALSE;
	GOptionEntry options[] = {
		{"address", 0, 0, G_OPTION_ARG_STRING, &app.address, "Pencil MAC (default: MT5806 sysfs)", "MAC"},
		{"duration", 0, 0, G_OPTION_ARG_INT, &duration, "Exit after this many seconds (test)", "SECONDS"},
		{"debug", 0, 0, G_OPTION_ARG_NONE, &debug, "Log protocol payloads", NULL},
		{NULL, 0, 0, 0, NULL, NULL, NULL}
	};
	GOptionContext *context = g_option_context_new("- OPPO Pencil BLE/touch bridge");
	g_option_context_add_main_entries(context, options, NULL);
	if (!g_option_context_parse(context, &argc, &argv, &error)) {
		g_printerr("%s\n", error->message);
		return 2;
	}
	if (debug)
		g_setenv("G_MESSAGES_DEBUG", "all", TRUE);
	if (nvt_open(&app.commands, 0) < 0 || nvt_open(&app.events, 1) < 0) {
		g_printerr("touch netlink: %s\n", strerror(errno));
		return 1;
	}
	app.bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
	if (!app.bus) {
		g_printerr("system bus: %s\n", error->message);
		return 1;
	}
	app.objects = g_dbus_object_manager_client_new_sync(app.bus, G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE,
		BLUEZ, "/", NULL, NULL, NULL, NULL, &error);
	if (!app.objects) {
		g_printerr("BlueZ: %s\n", error->message);
		return 1;
	}
	app.loop = g_main_loop_new(NULL, FALSE);
	g_signal_connect(app.objects, "interface-proxy-properties-changed", G_CALLBACK(properties_changed), &app);
	g_signal_connect(app.objects, "object-added", G_CALLBACK(object_changed), &app);
	g_signal_connect(app.objects, "object-removed", G_CALLBACK(object_removed), &app);
	g_unix_fd_add(app.events.fd, G_IO_IN | G_IO_ERR | G_IO_HUP, raw_ready, &app);
	app.uevent_fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_KOBJECT_UEVENT);
	struct sockaddr_nl addr = {.nl_family = AF_NETLINK, .nl_groups = 1};
	if (app.uevent_fd < 0 || bind(app.uevent_fd, (void *)&addr, sizeof(addr)) < 0) {
		g_printerr("power supply uevent: %s\n", strerror(errno));
		return 1;
	}
	g_unix_fd_add(app.uevent_fd, G_IO_IN, uevent_ready, &app);
	g_unix_signal_add(SIGINT, quit, &app);
	g_unix_signal_add(SIGTERM, quit, &app);
	if (duration > 0)
		g_timeout_add_seconds(duration, quit, &app);
	configure(&app);
	g_main_loop_run(app.loop);
	g_message("bridge stopped: uplinks=%lu downlinks=%lu unmapped=%lu", app.uplinks, app.downlinks, app.unknown);
	if (app.initialized) {
		const uint8_t disconnected[] = {0x7f,0x40,0};
		release_pressure(&app);
		touch_write(&app, disconnected, sizeof(disconnected));
	}
	/* No module unload or Bluetooth disconnect: the standard HID connection remains. */
	nvt_close(&app.commands);
	nvt_close(&app.events);
	close(app.uevent_fd);
	return 0;
}
