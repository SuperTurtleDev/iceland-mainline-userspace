/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Exercise the real daemon callbacks without sending BLE or touch commands. */
#define main pencild_main
#include "../iceland-pencild.c"
#undef main
#include <assert.h>

int main(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "--panel")) {
		printf("panel_refresh=%u\n", panel_refresh());
		return 0;
	}
	struct ble_request busy = {0};
	struct app app = {
		.subscribed = TRUE, .status_subscribed = TRUE,
		.status = "/test/status", .pen_enabled = -1, .inflight = &busy,
	};
	const uint8_t on[] = {0x2c,0x90,1,1,1};
	const uint8_t off[] = {0x2c,0x90,1,1,0};
	status_notification(&app, on, sizeof(on));
	assert(app.pen_enabled == 1 && app.status_source);
	guint timer = app.status_source;
	status_notification(&app, on, sizeof(on));
	assert(app.status_source == timer && g_queue_is_empty(&app.writes));
	status_notification(&app, off, sizeof(off));
	assert(app.pen_enabled == 0 && !app.status_source);
	assert(!g_main_context_find_source_by_id(NULL, timer));
	assert(g_queue_get_length(&app.writes) == 1);
	struct ble_request *reply = g_queue_pop_head(&app.writes);
	gsize len;
	const void *bytes = g_bytes_get_data(reply->bytes, &len);
	assert(!strcmp(reply->path, app.status) && !reply->pdu);
	assert(len == sizeof(off) && !memcmp(bytes, off, len));
	write_free(reply);
	status_notification(&app, (uint8_t[]){0x2c,0x94,2,0,3}, 5);
	assert(app.pen_enabled == 0 && g_queue_is_empty(&app.writes));
	status_notification(&app, on, sizeof(on));
	timer = app.status_source;
	reset_status(&app);
	assert(!app.status_source && !app.status_subscribed && app.pen_enabled == -1);
	assert(!g_main_context_find_source_by_id(NULL, timer));
	assert(acknowledge_enabled(&app) == G_SOURCE_REMOVE);
	assert(g_queue_is_empty(&app.writes));
	puts("status callbacks passed: coalescing, disable ACK channel, cancellation, disconnect");
	return 0;
}
