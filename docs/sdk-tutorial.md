# Tutorial: One Song, an Octopus app that plays a song

In this tutorial you build **One Song**: an app with one big button that
plays a song from the internet, or stops it. You'll try it in the
simulator on your PC, then install it on a device running Octopus
OS - a Raspberry Pi, or the Chumby One itself. It takes about 15 minutes,
and needs no device until the last step.

What you'll learn:
- the shape of an app (an LVGL screen, the main loop, day and night
  colours);
- playing sound with Octopus's stream player, which goes on while your
  app is in the background;
- bundles: building, packing, the simulator, installing.

You need the SDK for your board, built once with `sdk/make-sdk.sh`
([sdk-build.md](sdk-build.md)), ARM emulation, and `octopus-app` on your
PATH ([sdk.md](sdk.md), "Setting up"). `octopus-app` builds for a Raspberry
Pi unless `OCTOPUS_BOARD` says otherwise (`falconwing` for the
Chumby One).

## 1. Start from the template

```sh
octopus-app new OneSong
cd OneSong
```

That makes `onesong.c` (a label in the middle of the screen), `app.conf`
and a `Makefile`. Build it and try it now, before changing anything:

```sh
octopus-app build      # cross-compiles into bundle/
octopus-app sim        # Octopus OS in a window
```

In the window, open the Home menu (the house, top left) and pick "OneSong":
"Hello, Octopus" in the middle of the screen. Close the window (or Ctrl-C)
to stop the simulator.

## 2. Describe the app: app.conf

Open `app.conf` and make it:

```
Id = OneSong
Name = One Song
Short = Song
Version = 1.0
Exec = onesong
Icon = play
Color = #d35400
```

- **`Id`** is how the shell knows your app's window. The program must pass
  the same string to `octopus_app_init()`.
- **`Name`** is shown in the Home menu, and **`Short`** on the app's tab
  (tabs are narrow).
- **`Icon`** is one of the shell's icons; **`Color`** is your app's hue (its
  tab, and `OCTOPUS_COLOR_ACCENT` in your code).
- **`Exec`** is the program, inside the bundle.

## 3. The code

Replace `onesong.c` with the following, read top to bottom. (The finished
file is also in the SDK: `sdk/examples/onesong/onesong.c`.)

### The song, and the parts of the screen

```c
#include <stdio.h>
#include <string.h>
#include <octopus.h>

#define APP_ID	"OneSong"	/* = Id in app.conf */
#define SONG	"https://www.soundhelix.com/examples/mp3/SoundHelix-Song-1.mp3"
#define TITLE	"SoundHelix Song 1"

static lv_obj_t *button, *button_icon, *title, *status;
```

`octopus.h` is liboctopus: LVGL 9 (all of `lv_*`) plus Octopus's own
`octopus_*` functions. The song is a plain MP3 file on the web; a radio
stream's URL works too.

### Asking the player what plays

```c
/* Is our song the one playing (or connecting)? */
static bool ours(struct octopus_stream_status *st)
{
	return octopus_stream_status(st) && !strcmp(st->url, SONG);
}
```

The app doesn't play the song itself. Octopus OS has one **stream player**,
`octopus-stream`, a process of its own, so the song goes on while your app
is in the background, frozen (the shell freezes apps out of sight) or
closed. `octopus_stream_status()` reads what it's doing: false when nothing
plays; otherwise the URL, the state ("connecting", "playing", "error"), and
for a file, the seconds played.

Something else may be playing: the FM radio, AirPlay, another app's
stream. So the app checks that the stream is **ours**, by its URL.

### The button

```c
static void pressed(lv_event_t *e)
{
	struct octopus_stream_status st;

	(void)e;
	if (ours(&st))
		octopus_stream_stop();
	else
		octopus_stream_play(SONG, TITLE);	/* stops FM, AirPlay, another stream */
	lv_label_set_text(status, "...");	/* until the player says */
}
```

`octopus_stream_play()` starts the player in the background and returns at
once (never block the UI: a stuck main loop is a frozen screen, and on a
small board's CPU that's easy). Whatever else was playing stops: there is one
speaker, and the newest source takes it. The name (`TITLE`) is what the
volume control's stop button shows.

### Showing the state, twice a second

```c
static void poll(lv_timer_t *t)
{
	struct octopus_stream_status st;
	char text[64];

	(void)t;
	if (!ours(&st)) {
		lv_label_set_text(button_icon, LV_SYMBOL_PLAY);
		lv_label_set_text(status, "Tap to play");
		return;
	}
	lv_label_set_text(button_icon, LV_SYMBOL_STOP);
	if (!strcmp(st.state, "playing"))
		snprintf(text, sizeof(text), "Playing  %d:%02d", st.played_s / 60, st.played_s % 60);
	else if (!strcmp(st.state, "error"))
		snprintf(text, sizeof(text), "Can't play it (no network?)");
	else
		snprintf(text, sizeof(text), "Connecting...");
	lv_label_set_text(status, text);
}
```

The app keeps no "am I playing?" variable of its own: it asks the player
every time. That keeps it right when something else stops the song (the
volume control's stop button, an alarm, the radio taking over) and when the
app comes back from the background.

### Day and night

```c
static void on_mode(octopus_mode_t mode, void *user)
{
	(void)mode; (void)user;
	octopus_glossy(button, octopus_color(OCTOPUS_COLOR_ACCENT), 0);
	lv_obj_set_style_text_color(button_icon, octopus_on_accent(), 0);
	lv_obj_set_style_text_color(title, octopus_color(OCTOPUS_COLOR_TEXT), 0);
	lv_obj_set_style_text_color(status, octopus_color(OCTOPUS_COLOR_TEXT_DIM), 0);
}
```

Octopus OS has a day theme and a darker, warmer night theme. Set colours
in a mode callback, never once at start: it's called once when you register
it, and again at every switch. `OCTOPUS_COLOR_ACCENT` is your app's `Color`
(dimmed at night); `octopus_on_accent()` is the text colour that reads on it.

### Putting it together

```c
int main(void)
{
	lv_obj_t *scr = octopus_app_init(APP_ID);

	lv_obj_set_scrollable(scr, false);

	/* The button: big and round (a finger on a resistive screen). */
	button = lv_button_create(scr);
	lv_obj_set_size(button, 96, 96);
	lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
	lv_obj_align(button, LV_ALIGN_CENTER, 0, -24);
	lv_obj_add_event_cb(button, pressed, LV_EVENT_CLICKED, NULL);
	button_icon = lv_label_create(button);
	lv_obj_set_style_text_font(button_icon, &lv_font_montserrat_48, 0);
	lv_obj_center(button_icon);

	title = lv_label_create(scr);
	lv_obj_set_style_text_font(title, octopus_font(OCTOPUS_FONT_LARGE), 0);
	lv_label_set_text(title, TITLE);
	lv_obj_align_to(title, button, LV_ALIGN_OUT_BOTTOM_MID, 0, 14);

	status = lv_label_create(scr);
	lv_obj_set_style_text_font(status, octopus_font(OCTOPUS_FONT_SMALL), 0);
	lv_obj_set_style_text_align(status, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_width(status, OCTOPUS_APP_W);	/* centred however long the text */
	lv_obj_align_to(status, title, LV_ALIGN_OUT_BOTTOM_MID, 0, 6);

	octopus_on_mode_change(on_mode, NULL);
	lv_timer_ready(lv_timer_create(poll, 500, NULL));
	return octopus_run();
}
```

- `octopus_app_init()` connects to the compositor and gives you your
  window's screen: 320x208 under the shell's tab bar.
- The fonts are Octopus's bitmap fonts (`octopus_font()`); the play and
  stop symbols come from LVGL's Montserrat, at 14, 20, 28 or 48 px.
- `lv_timer_ready()` runs the first poll at once, so the button is right
  from the start.
- `octopus_run()` is the main loop: LVGL's timers, input, the compositor. It
  returns when the window is closed (holding its tab, or the shell closing
  background apps when memory runs short). Your app can be ended any time:
  keep anything worth keeping in `octopus_app_data_dir()` as it changes.

## 4. Build and try it

```sh
octopus-app build
octopus-app sim
```

Home menu → One Song. Tap the button (click it): "Connecting...", then
"Playing 0:01, 0:02...", and you hear it through your PC's speakers (the
simulator's sound goes to your desktop's). "Can't play it" means the
song couldn't be fetched: the simulator's network is the PC's.

Things to try:
- Switch to another app from the Home menu: the song goes on. Come back,
  and the time has moved on.
- Open the volume control (the speaker icon): its stop button stops your
  song, and your app notices within half a second.

## 5. Pack and install it on your device

```sh
octopus-app pack                 # -> OneSong.octopusapp (a few KB)
```

Then either:
- **USB:** copy `OneSong.octopusapp` into a folder named `octopus-apps` on a
  FAT-formatted stick, and plug it into the device. One Song appears in the
  Home menu.
- **Network:** `octopus-app deploy <the device's address>` (SSH, as root:
  turn the SSH server on in Settings > Network first, which shows the
  address and the password).

Remove it with `octopus-install --remove OneSong` on the device. A bundle
runs only on the kind of board its SDK was built for: `octopus-install`
says so if it's another's.

## Where next

- [writing-octopus-apps.md](writing-octopus-apps.md): the rest of liboctopus,
  sound (the FM radio, playing your own sounds), input, and the LVGL traps
  we've hit.
- `octopus.h` in the SDK (`/opt/octopus-sdk/arm-buildroot-linux-gnueabi/sysroot/usr/include/octopus/`):
  every function, with comments.
- Screen savers are programs too, on liboctopus's saver kit
  (`octopus_saver.h`; Flying Toasters, `package/octopus-apps/src/toasters.c`,
  is the example). They're part of the image for now
  (`/usr/lib/octopus/savers`): bundles can't install one yet.
