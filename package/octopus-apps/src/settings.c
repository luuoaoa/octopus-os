// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * octopus-settings: the system's settings, by category - down the left, a
 * category's options to the right of it.
 *
 *   Appearance   night mode: auto (by the policy's hours), on, off
 *   Screen saver which (none, or one in /usr/lib/octopus/savers), after how
 *                long without input; a preview
 *   Time         the time zone
 *   Network      the SSH server (off by default; the first time on, a
 *                random root password, shown here)
 *   Sound        the bass enhancer's level and an EQ (a preset, or
 *                custom from eq.txt) - where the system has them (the
 *                RasPad's PipeWire chain: octopus-sound)
 *
 * Adding a setting: a builder for its category (or a new category in
 * categories[]: a name, an icon, when it applies, its builder). What the
 * apps' user may set goes in liboctopus's settings file
 * (octopus_setting_set(): night mode, the sound); what only root may (the
 * SSH server, the time zone) goes through the shell, which runs
 * /usr/libexec/octopus-setting for its "setting <name> <value>" command.
 */

#include <arpa/inet.h>
#include <dirent.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <signal.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <octopus.h>

#define APP_ID		"Settings"	/* = Id in settings.app */
#define SOUND_TOOL	"/usr/bin/octopus-sound"
#define SSH_ON_FILE	"/run/octopus-config/ssh-on"
#define SSH_PW_FILE	"/var/lib/octopus-root/ssh-password"
#define TZ_FILE		"/etc/timezone"
/* Sizes are 320x240's, scaled for the screen (octopus_px()). */
#define CAT_W		octopus_px(118)	/* the category list */
#define CAT_H		octopus_px(36)	/* five fit: 5 x (36 + 2) + 8 <= 208 */
#define PAD		OCTOPUS_MARGIN	/* the panel's content: the apps' margins */
#define PANEL_W		(OCTOPUS_APP_W - CAT_W)

struct category {
	const char *name;
	const char *icon;		/* LV_SYMBOL_* or OCTOPUS_ICON_* */
	bool (*applies)(void);		/* NULL: always */
	void (*build)(lv_obj_t *panel);
};

static void appearance_build(lv_obj_t *panel);
static void saver_build(lv_obj_t *panel);
static void time_build(lv_obj_t *panel);
static void network_build(lv_obj_t *panel);
static void sound_build(lv_obj_t *panel);
static bool sound_applies(void);

static const struct category categories[] = {
	{ "Appearance", OCTOPUS_ICON_SUN, NULL, appearance_build },
	{ "Saver", LV_SYMBOL_IMAGE, NULL, saver_build },
	{ "Time", OCTOPUS_ICON_CLOCK, NULL, time_build },
	{ "Network", LV_SYMBOL_WIFI, NULL, network_build },
	{ "Sound", LV_SYMBOL_AUDIO, sound_applies, sound_build },
};
#define N_CATEGORIES	(int)(sizeof(categories) / sizeof(categories[0]))

static lv_obj_t *scr, *panel;
static int cur_cat;
/* Shown and kept up to date (refresh_cb) by the category on screen. */
static lv_obj_t *ssh_switch, *ssh_info, *tz_label;

/* ------------------------------------------------------------------ */
/* Building blocks                                                    */

static lv_obj_t *heading(lv_obj_t *parent, const char *text)
{
	lv_obj_t *l = lv_label_create(parent);

	lv_obj_set_style_text_font(l, octopus_font(OCTOPUS_FONT_LARGE), 0);
	lv_obj_set_style_text_color(l, octopus_color(OCTOPUS_COLOR_TEXT), 0);
	lv_label_set_text(l, text);
	return l;
}

static lv_obj_t *note(lv_obj_t *parent, const char *text)
{
	lv_obj_t *l = lv_label_create(parent);

	lv_obj_set_style_text_font(l, octopus_font(OCTOPUS_FONT_CAPTION), 0);
	lv_obj_set_style_text_color(l, octopus_color(OCTOPUS_COLOR_TEXT_DIM), 0);
	lv_obj_set_width(l, LV_PCT(100));
	lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
	lv_label_set_text(l, text);
	return l;
}

/* A row: a label at the left, a control at the right. */
static lv_obj_t *row(lv_obj_t *parent, const char *text)
{
	lv_obj_t *r = lv_obj_create(parent), *l;

	lv_obj_remove_style_all(r);
	lv_obj_set_size(r, LV_PCT(100), octopus_px(36));
	lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	l = lv_label_create(r);
	lv_obj_set_style_text_font(l, octopus_font(OCTOPUS_FONT_BODY), 0);
	lv_obj_set_style_text_color(l, octopus_color(OCTOPUS_COLOR_TEXT), 0);
	lv_label_set_text(l, text);
	return r;
}

static lv_obj_t *switch_in(lv_obj_t *r, bool on, lv_event_cb_t cb)
{
	lv_obj_t *sw = lv_switch_create(r);

	lv_obj_set_size(sw, octopus_px(52), octopus_px(28));
	lv_obj_set_style_bg_color(sw, octopus_color(OCTOPUS_COLOR_ACCENT),
				  LV_PART_INDICATOR | LV_STATE_CHECKED);
	if (on)
		lv_obj_add_state(sw, LV_STATE_CHECKED);
	lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
	return sw;
}

/* A choice of a few: buttons side by side, the chosen one filled. The
 * event's user data is the choice's index; its button's text, its name. */
static void choice_buttons(lv_obj_t *parent, const char *const *names, int n, int chosen,
			   lv_event_cb_t cb)
{
	lv_obj_t *r = lv_obj_create(parent);
	int i;

	lv_obj_remove_style_all(r);
	lv_obj_set_size(r, LV_PCT(100), octopus_px(38));
	lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_column(r, octopus_px(4), 0);
	for (i = 0; i < n; i++) {
		lv_obj_t *b = lv_button_create(r), *l;

		lv_obj_set_height(b, octopus_px(36));
		lv_obj_set_flex_grow(b, 1);
		lv_obj_set_style_radius(b, octopus_px(8), 0);
		lv_obj_set_style_shadow_width(b, 0, 0);
		if (i == chosen)
			octopus_glossy(b, octopus_color(OCTOPUS_COLOR_ACCENT), 0);
		else
			lv_obj_set_style_bg_color(b, octopus_color(OCTOPUS_COLOR_SURFACE), 0);
		l = lv_label_create(b);
		lv_obj_set_style_text_font(l, octopus_font(OCTOPUS_FONT_BODY), 0);
		lv_obj_set_style_text_color(l, i == chosen ? octopus_on_accent() :
					    octopus_color(OCTOPUS_COLOR_TEXT), 0);
		lv_label_set_text(l, names[i]);
		lv_obj_center(l);
		lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
	}
}

static void panel_show(int cat);

/* ------------------------------------------------------------------ */
/* Appearance                                                         */

static const char *const mode_names[] = { "Auto", "On", "Off" };
static const char *const mode_values[] = { "auto", "night", "day" };

/* After the tap's event: the theme follows the setting (on_mode rebuilds
 * the screen, the button tapped with it - not while it's handling it). */
static void mode_apply(void *choice)
{
	int i = (int)(intptr_t)choice;
	octopus_mode_t before = octopus_mode();

	octopus_setting_set("mode", mode_values[i]);
	if (octopus_mode() == before)
		panel_show(cur_cat);	/* (no switch, no rebuild: the buttons again) */
}

static void mode_cb(lv_event_t *e)
{
	lv_async_call(mode_apply, lv_event_get_user_data(e));
}

static void appearance_build(lv_obj_t *p)
{
	char val[16] = "";
	int chosen = 0, i;

	if (!octopus_setting_get("mode", val, sizeof(val)))
		snprintf(val, sizeof(val), "%s", octopus_policy()->force == OCTOPUS_MODE_NIGHT ?
			 "night" : octopus_policy()->force == OCTOPUS_MODE_DAY ? "day" : "auto");
	for (i = 0; i < 3; i++)
		if (!strcmp(val, mode_values[i]))
			chosen = i;
	heading(p, "Night mode");
	choice_buttons(p, mode_names, 3, chosen, mode_cb);
}

/* ------------------------------------------------------------------ */
/* Screen saver                                                       */

#define SAVERS		"/usr/lib/octopus/savers"
#define MAX_SAVERS	8

static char savers[MAX_SAVERS + 1][32];	/* [0]: "none" */
static int n_savers;
static const int minutes[] = { 1, 2, 5, 10, 15, 30, 60 };
#define N_MINUTES	(int)(sizeof(minutes) / sizeof(minutes[0]))

static void saver_cb(lv_event_t *e)
{
	int i = lv_dropdown_get_selected(lv_event_get_target(e));

	if (i < n_savers)
		octopus_setting_set("saver", savers[i]);
}

static void minutes_cb(lv_event_t *e)
{
	char v[8];

	snprintf(v, sizeof(v), "%d", minutes[lv_dropdown_get_selected(lv_event_get_target(e))]);
	octopus_setting_set("saver_minutes", v);
}

static void preview_cb(lv_event_t *e)
{
	(void)e;
	octopus_shell_command("saver");	/* the shell starts it; a tap ends it */
}

static lv_obj_t *dropdown_in(lv_obj_t *r, const char *opts, int sel, int w, lv_event_cb_t cb)
{
	lv_obj_t *dd = lv_dropdown_create(r);

	lv_dropdown_set_options(dd, opts);
	lv_dropdown_set_selected(dd, sel);
	lv_obj_set_width(dd, w);
	lv_obj_set_style_text_font(dd, octopus_font(OCTOPUS_FONT_BODY), 0);
	lv_obj_add_event_cb(dd, cb, LV_EVENT_VALUE_CHANGED, NULL);
	return dd;
}

static void saver_build(lv_obj_t *p)
{
	const struct octopus_policy *pol = octopus_policy();
	char opts[(MAX_SAVERS + 1) * 34] = "None", mopts[N_MINUTES * 12] = "";
	struct dirent *e;
	DIR *d;
	lv_obj_t *r, *b, *l;
	int i, sel = 0, msel = 2;

	/* Every program in SAVERS: its name, capitalised ("toasters": Toasters). */
	n_savers = 1;
	snprintf(savers[0], sizeof(savers[0]), "none");
	if ((d = opendir(SAVERS))) {
		while ((e = readdir(d)) && n_savers <= MAX_SAVERS) {
			char path[300], name[32];

			snprintf(path, sizeof(path), SAVERS "/%s", e->d_name);
			if (e->d_name[0] == '.' || access(path, X_OK) != 0 || strlen(e->d_name) > 30)
				continue;
			snprintf(savers[n_savers], sizeof(savers[0]), "%s", e->d_name);
			snprintf(name, sizeof(name), "%s", e->d_name);
			name[0] = toupper((unsigned char)name[0]);
			strcat(opts, "\n");
			strcat(opts, name);
			if (!strcmp(pol->saver, e->d_name))
				sel = n_savers;
			n_savers++;
		}
		closedir(d);
	}
	for (i = 0; i < N_MINUTES; i++) {
		char m[12];

		snprintf(m, sizeof(m), "%s%d min", i ? "\n" : "", minutes[i]);
		strcat(mopts, m);
		if (minutes[i] == pol->saver_minutes)
			msel = i;
	}

	heading(p, "Screen saver");
	r = row(p, "Saver");
	dropdown_in(r, opts, sel, 116, saver_cb);
	r = row(p, "After");
	dropdown_in(r, mopts, msel, 116, minutes_cb);
	b = lv_button_create(p);
	lv_obj_set_size(b, octopus_px(96), octopus_px(36));
	lv_obj_set_style_radius(b, octopus_px(8), 0);
	lv_obj_set_style_shadow_width(b, 0, 0);
	octopus_glossy(b, octopus_color(OCTOPUS_COLOR_ACCENT), 0);
	l = lv_label_create(b);
	lv_obj_set_style_text_color(l, octopus_on_accent(), 0);
	lv_label_set_text(l, "Preview");
	lv_obj_center(l);
	lv_obj_add_event_cb(b, preview_cb, LV_EVENT_CLICKED, NULL);
}

/* ------------------------------------------------------------------ */
/* Time                                                               */

/* A choice of zones (the tz database has hundreds): a city each. */
static const char *const zones[] = {
	"Pacific/Honolulu", "America/Anchorage", "America/Los_Angeles", "America/Denver",
	"America/Phoenix", "America/Chicago", "America/New_York", "America/Halifax",
	"America/Mexico_City", "America/Bogota", "America/Lima", "America/Santiago",
	"America/Sao_Paulo", "America/Argentina/Buenos_Aires", "Atlantic/Reykjavik",
	"Europe/London", "Europe/Dublin", "Europe/Lisbon", "Europe/Paris", "Europe/Madrid",
	"Europe/Berlin", "Europe/Amsterdam", "Europe/Rome", "Europe/Stockholm",
	"Europe/Warsaw", "Europe/Athens", "Europe/Helsinki", "Europe/Istanbul",
	"Europe/Moscow", "Africa/Lagos", "Africa/Cairo", "Africa/Johannesburg",
	"Africa/Nairobi", "Asia/Dubai", "Asia/Karachi", "Asia/Kolkata", "Asia/Dhaka",
	"Asia/Bangkok", "Asia/Ho_Chi_Minh", "Asia/Jakarta", "Asia/Shanghai",
	"Asia/Hong_Kong", "Asia/Singapore", "Asia/Taipei", "Asia/Manila", "Asia/Seoul",
	"Asia/Tokyo", "Australia/Perth", "Australia/Adelaide", "Australia/Brisbane",
	"Australia/Sydney", "Pacific/Auckland", "UTC",
};
#define N_ZONES		(int)(sizeof(zones) / sizeof(zones[0]))

static lv_obj_t *tz_roller;

static void tz_current(char *out, size_t size)
{
	FILE *f = fopen(TZ_FILE, "r");

	out[0] = '\0';
	if (f) {
		if (fgets(out, size, f))
			out[strcspn(out, "\n")] = '\0';
		fclose(f);
	}
	if (!out[0])
		snprintf(out, size, "UTC");
}

static void tz_label_update(void)
{
	char tz[64], text[96];

	tz_current(tz, sizeof(tz));
	snprintf(text, sizeof(text), "Now: %s", tz);
	if (strcmp(lv_label_get_text(tz_label), text))
		lv_label_set_text(tz_label, text);
}

static void tz_set_cb(lv_event_t *e)
{
	char cmd[96];

	(void)e;
	snprintf(cmd, sizeof(cmd), "setting timezone %s", zones[lv_roller_get_selected(tz_roller)]);
	octopus_shell_command(cmd);	/* the shell runs octopus-setting (root's) */
}

static void time_build(lv_obj_t *p)
{
	char tz[64], *opts, *o;
	lv_obj_t *r, *b, *l;
	int i, len = 0, sel = 0;

	heading(p, "Time zone");
	tz_label = note(p, "");
	tz_label_update();
	tz_current(tz, sizeof(tz));
	for (i = 0; i < N_ZONES; i++)
		len += strlen(zones[i]) + 1;
	opts = o = malloc(len + 1);
	for (i = 0; i < N_ZONES; i++) {	/* "America/Los_Angeles" shown as "Los Angeles" */
		const char *city = strrchr(zones[i], '/');
		int j;

		city = city ? city + 1 : zones[i];
		for (j = 0; city[j]; j++)
			*o++ = city[j] == '_' ? ' ' : city[j];
		*o++ = i < N_ZONES - 1 ? '\n' : '\0';
		if (!strcmp(zones[i], tz))
			sel = i;
	}
	r = lv_obj_create(p);
	lv_obj_remove_style_all(r);
	lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	tz_roller = lv_roller_create(r);
	lv_roller_set_options(tz_roller, opts, LV_ROLLER_MODE_NORMAL);
	free(opts);
	lv_roller_set_visible_row_count(tz_roller, 3);
	lv_roller_set_selected(tz_roller, sel, LV_ANIM_OFF);
	lv_obj_set_width(tz_roller, octopus_px(116));	/* "Johannesburg" */
	lv_obj_set_style_text_font(tz_roller, octopus_font(OCTOPUS_FONT_BODY), 0);
	lv_obj_set_style_bg_color(tz_roller, octopus_color(OCTOPUS_COLOR_ACCENT), LV_PART_SELECTED);
	lv_obj_set_style_text_color(tz_roller, octopus_on_accent(), LV_PART_SELECTED);
	b = lv_button_create(r);
	lv_obj_set_size(b, octopus_px(56), octopus_px(40));
	lv_obj_set_style_radius(b, octopus_px(8), 0);
	lv_obj_set_style_shadow_width(b, 0, 0);
	octopus_glossy(b, octopus_color(OCTOPUS_COLOR_ACCENT), 0);
	l = lv_label_create(b);
	lv_obj_set_style_text_color(l, octopus_on_accent(), 0);
	lv_label_set_text(l, "Set");
	lv_obj_center(l);
	lv_obj_add_event_cb(b, tz_set_cb, LV_EVENT_CLICKED, NULL);
}

/* ------------------------------------------------------------------ */
/* Network                                                            */

static bool ssh_on(void)
{
	return access(SSH_ON_FILE, F_OK) == 0;
}

/* The Chumby's address, for "ssh root@...": its first IPv4 one. */
static void my_address(char *out, size_t size)
{
	struct ifaddrs *ifs, *i;

	snprintf(out, size, "(no network)");
	if (getifaddrs(&ifs) < 0)
		return;
	for (i = ifs; i; i = i->ifa_next)
		if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET &&
		    !(i->ifa_flags & IFF_LOOPBACK)) {
			inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, out, size);
			break;
		}
	freeifaddrs(ifs);
}

static void ssh_info_update(void)
{
	char text[160], pw[32] = "", addr[48];
	FILE *f;

	if (!ssh_info)
		return;
	if (!ssh_on()) {
		snprintf(text, sizeof(text), "Off: nothing can log in over the network.");
	} else {
		if ((f = fopen(SSH_PW_FILE, "r"))) {
			if (fgets(pw, sizeof(pw), f))
				pw[strcspn(pw, "\n")] = '\0';
			fclose(f);
		}
		my_address(addr, sizeof(addr));
		snprintf(text, sizeof(text), "ssh root@%s\nPassword: %s", addr,
			 pw[0] ? pw : "(as set on the card)");
	}
	if (strcmp(lv_label_get_text(ssh_info), text))
		lv_label_set_text(ssh_info, text);
	if (ssh_on() != lv_obj_has_state(ssh_switch, LV_STATE_CHECKED)) {
		if (ssh_on())
			lv_obj_add_state(ssh_switch, LV_STATE_CHECKED);
		else
			lv_obj_remove_state(ssh_switch, LV_STATE_CHECKED);
	}
}

static void ssh_cb(lv_event_t *e)
{
	bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);

	octopus_shell_command(on ? "setting ssh on" : "setting ssh off");
}

static void network_build(lv_obj_t *p)
{
	lv_obj_t *r;

	heading(p, "SSH server");
	r = row(p, "Allow logins");
	ssh_switch = switch_in(r, ssh_on(), ssh_cb);
	ssh_info = note(p, "");
	ssh_info_update();
}

/* ------------------------------------------------------------------ */
/* Sound (where the system has processing to set: octopus-sound)        */

#define MAX_PRESETS	8

static char preset_ids[MAX_PRESETS][16], preset_names[MAX_PRESETS][32];
static int n_presets;

static bool sound_applies(void)
{
	return access(SOUND_TOOL, X_OK) == 0;
}

static void sound_apply(void)
{
	char *argv[] = { "octopus-sound", "apply", NULL };

	octopus_spawn(SOUND_TOOL, argv);	/* (reaped: SIGCHLD ignored) */
}

static const char *const bass_ids[] = { "off", "low", "medium", "high" };
static lv_obj_t *custom_note, *custom_info;

static void bass_cb(lv_event_t *e)
{
	unsigned i = lv_dropdown_get_selected(lv_event_get_target(e));

	if (i < sizeof(bass_ids) / sizeof(bass_ids[0])) {
		octopus_setting_set("bass", bass_ids[i]);
		sound_apply();
	}
}

/* Custom: where its file goes and what it gives; shown only for Custom. */
static void custom_update(const char *eq)
{
	char line[128] = "";
	FILE *f;
	bool custom = !strcmp(eq, "custom");

	if (custom && (f = popen(SOUND_TOOL " custom-info", "r"))) {
		if (fgets(line, sizeof(line), f))
			line[strcspn(line, "\n")] = '\0';
		pclose(f);
		lv_label_set_text(custom_info, line);
	}
	if (custom) {
		lv_obj_remove_flag(custom_note, LV_OBJ_FLAG_HIDDEN);
		lv_obj_remove_flag(custom_info, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_add_flag(custom_note, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(custom_info, LV_OBJ_FLAG_HIDDEN);
	}
}

static void eq_cb(lv_event_t *e)
{
	int i = lv_dropdown_get_selected(lv_event_get_target(e));

	if (i < n_presets) {
		octopus_setting_set("eq", preset_ids[i]);
		custom_update(preset_ids[i]);
		sound_apply();
	}
}

static void sound_build(lv_obj_t *p)
{
	char val[16] = "", line[64], opts[MAX_PRESETS * 32] = "";
	lv_obj_t *r, *dd;
	FILE *f;
	int sel = 0;

	heading(p, "Speakers");
	octopus_setting_get("bass", val, sizeof(val));
	if (!strcmp(val, "on"))		/* (the old switch) */
		strcpy(val, "medium");
	for (unsigned i = 0; i < sizeof(bass_ids) / sizeof(bass_ids[0]); i++)
		if (!strcmp(val, bass_ids[i]))
			sel = i;
	r = row(p, "Bass");
	dd = lv_dropdown_create(r);
	lv_dropdown_set_options(dd, "Off\nLow\nMedium\nHigh");
	lv_dropdown_set_selected(dd, sel);
	lv_obj_set_width(dd, octopus_px(110));
	lv_obj_set_style_text_font(dd, octopus_font(OCTOPUS_FONT_BODY), 0);
	lv_obj_add_event_cb(dd, bass_cb, LV_EVENT_VALUE_CHANGED, NULL);

	n_presets = 0;
	sel = 0;
	if ((f = popen(SOUND_TOOL " presets", "r"))) {
		while (n_presets < MAX_PRESETS && fgets(line, sizeof(line), f) &&
		       sscanf(line, "%15[^\t]\t%31[^\n]", preset_ids[n_presets],
			      preset_names[n_presets]) == 2)
			n_presets++;
		pclose(f);
	}
	val[0] = '\0';
	octopus_setting_get("eq", val, sizeof(val));
	for (int i = 0; i < n_presets; i++) {
		strcat(opts, preset_names[i]);
		if (i < n_presets - 1)
			strcat(opts, "\n");
		if (!strcmp(val, preset_ids[i]))
			sel = i;
	}
	r = row(p, "EQ");
	dd = lv_dropdown_create(r);
	lv_dropdown_set_options(dd, opts);
	lv_dropdown_set_selected(dd, sel);
	lv_obj_set_width(dd, octopus_px(110));
	lv_obj_set_style_text_font(dd, octopus_font(OCTOPUS_FONT_BODY), 0);
	lv_obj_add_event_cb(dd, eq_cb, LV_EVENT_VALUE_CHANGED, NULL);
	custom_note = note(p, "Custom: put eq.txt on the card's config partition "
			   "(next to user-data) and restart. Format: Room EQ Wizard's "
			   "File > Export > Filter settings as text (or Equalizer "
			   "APO's), lines like\n"
			   "Preamp: -4 dB\n"
			   "Filter 1: ON PK Fc 200 Hz Gain -3 dB Q 1.5\n"
			   "Up to 10 PK filters, one LS and one HS.");
	custom_info = note(p, "");
	custom_update(n_presets ? preset_ids[sel] : "");
	note(p, "Bass enhancer: adds the harmonics of bass too low for the small "
	     "speakers, so the ear hears it; higher is stronger.");
}

/* ------------------------------------------------------------------ */
/* The screen: categories, a category's panel                          */

static void panel_show(int cat)
{
	cur_cat = cat;
	ssh_info = ssh_switch = tz_label = NULL;
	lv_obj_clean(panel);
	categories[cat].build(panel);
}

static lv_obj_t *cat_list;

static void cat_cb(lv_event_t *e)
{
	int i = (int)(intptr_t)lv_event_get_user_data(e);
	uint32_t c;

	for (c = 0; c < lv_obj_get_child_count(cat_list); c++) {
		lv_obj_t *b = lv_obj_get_child(cat_list, c);

		if (b == lv_event_get_target(e))
			lv_obj_add_state(b, LV_STATE_CHECKED);
		else
			lv_obj_remove_state(b, LV_STATE_CHECKED);
	}
	panel_show(i);
}

/* Everything, in the mode's colours (again at a switch: night mode is one
 * of ours). */
static void build(void)
{
	int i;

	lv_obj_clean(scr);
	lv_obj_set_style_bg_color(scr, octopus_color(OCTOPUS_COLOR_BG), 0);

	cat_list = lv_obj_create(scr);
	lv_obj_remove_style_all(cat_list);
	lv_obj_set_size(cat_list, CAT_W, OCTOPUS_APP_H);
	lv_obj_set_style_bg_opa(cat_list, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(cat_list, octopus_color(OCTOPUS_COLOR_SURFACE), 0);
	lv_obj_set_style_pad_all(cat_list, octopus_px(4), 0);
	lv_obj_set_style_pad_row(cat_list, octopus_px(2), 0);
	lv_obj_set_flex_flow(cat_list, LV_FLEX_FLOW_COLUMN);
	for (i = 0; i < N_CATEGORIES; i++) {
		lv_obj_t *b, *l;

		if (categories[i].applies && !categories[i].applies())
			continue;
		b = lv_button_create(cat_list);
		lv_obj_set_size(b, LV_PCT(100), CAT_H);
		lv_obj_set_style_radius(b, octopus_px(8), 0);
		lv_obj_set_style_shadow_width(b, 0, 0);
		lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
		lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_CHECKED);
		lv_obj_set_style_bg_color(b, octopus_color(OCTOPUS_COLOR_ACCENT), LV_STATE_CHECKED);
		lv_obj_set_style_pad_left(b, octopus_px(4), 0);	/* room for "Appearance" */
		lv_obj_set_style_pad_right(b, octopus_px(2), 0);
		lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(b, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
				      LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_pad_column(b, octopus_px(4), 0);
		l = lv_label_create(b);
		lv_obj_set_style_text_font(l, octopus_icon_font(20), 0);
		lv_label_set_text(l, categories[i].icon);
		l = lv_label_create(b);
		lv_obj_set_style_text_font(l, octopus_font(OCTOPUS_FONT_BODY), 0);
		lv_label_set_text(l, categories[i].name);
		lv_obj_set_style_text_color(b, octopus_color(OCTOPUS_COLOR_TEXT), 0);
		lv_obj_set_style_text_color(b, octopus_on_accent(), LV_STATE_CHECKED);
		if (i == cur_cat)
			lv_obj_add_state(b, LV_STATE_CHECKED);
		lv_obj_add_event_cb(b, cat_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
	}

	panel = lv_obj_create(scr);
	lv_obj_remove_style_all(panel);
	lv_obj_set_size(panel, PANEL_W, OCTOPUS_APP_H);
	lv_obj_set_pos(panel, CAT_W, 0);
	lv_obj_set_style_pad_all(panel, PAD, 0);
	lv_obj_set_style_pad_row(panel, octopus_px(6), 0);
	lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_scroll_dir(panel, LV_DIR_VER);
	panel_show(cur_cat);
}

static void on_mode(octopus_mode_t mode, void *user)
{
	(void)mode; (void)user;
	build();
}

/* What changes by itself (the SSH server turned on through the shell, its
 * password made; the time zone set): kept up to date. */
static void refresh_cb(lv_timer_t *t)
{
	(void)t;
	ssh_info_update();
	if (tz_label)
		tz_label_update();
}

/* octopus-settings [CATEGORY]: open at that category (its name, any case). */
int main(int argc, char **argv)
{
	int i;

	for (i = 0; argc > 1 && i < N_CATEGORIES; i++)
		if (!strcasecmp(argv[1], categories[i].name) &&
		    (!categories[i].applies || categories[i].applies()))
			cur_cat = i;
	signal(SIGCHLD, SIG_IGN);	/* octopus-sound: nothing to wait for */
	scr = octopus_app_init(APP_ID);
	lv_obj_set_scrollable(scr, false);
	octopus_on_mode_change(on_mode, NULL);	/* builds it */
	lv_timer_create(refresh_cb, 1000, NULL);
	return octopus_run();
}
