# Writing apps for the Chumby One (falconwing-mainline)

How the UI and sound fit together on the Chumby One image, what's already
there to build on, and the traps we fell into so you don't have to. Read
this before writing an app; the headers it points at have the details.

## The pieces

```
chumby-wm (wlroots compositor, package/chumby-wm)
 ├── chumby-shell  (package/chumby-shell)  tab bar, Home menu, volume /
 │                  brightness sliders, alarms, snooze button, screen power
 └── apps          (package/chumby-apps)  chumby-clock, chumby-fm,
                    chumby-netradio, ...: one Wayland client each
libchumby (package/libchumby)  LVGL 9.6 compiled in, plus the Chumby
                    framework every app and the shell link against
```

- **Screen**: 320x240. The shell's tab bar is the top `CHUMBY_BAR_H` (32)
  px; an app gets `CHUMBY_APP_W` x `CHUMBY_APP_H` (320x208) under it. The
  compositor places windows by their app_id (see the top of
  `chumby-wm.c`); an app doesn't position itself.
- **One app in front at a time.** The tab bar shows it (filled tab, with
  icon and name) and the one before it. Home is a menu of installed apps,
  not a place.
- **Apps out of sight are frozen** (`SIGSTOP`) until brought back
  (`SIGCONT`): zero CPU, state kept, their last frame still on screen. Your
  app just stops between two instructions and carries on later; timers
  fire late, all at once. If you must keep working out of sight (a
  download, a timer that has to fire), call `chumby_stay_awake(true)`, and
  `false` once done. Alarms don't need this: the shell rings them.
- **Sound runs in its own processes** (the FM loop, `chumby-stream`), so
  freezing or closing an app never cuts it. See [Sound](#sound).

## An app in 30 lines

```c
#include <chumby.h>

static lv_obj_t *label;

static void on_mode(chumby_mode_t mode, void *user)
{
	(void)mode; (void)user;
	/* Colors are per mode: set them here, not once at start. */
	lv_obj_set_style_text_color(label, chumby_color(CHUMBY_COLOR_TEXT), 0);
}

int main(void)
{
	lv_obj_t *scr = chumby_app_init("Hello");	/* = Id in hello.app */

	lv_obj_set_scrollable(scr, false);
	label = lv_label_create(scr);
	lv_obj_set_style_text_font(label, chumby_font(CHUMBY_FONT_LARGE), 0);
	lv_label_set_text(label, "Hello, Chumby");
	lv_obj_center(label);
	chumby_on_mode_change(on_mode, NULL);	/* calls it once now, too */
	return chumby_run();
}
```

Plus a descriptor, `/usr/share/chumby/apps/hello.app`:

```
# Id must match the app_id the app passes to chumby_app_init().
Id = Hello
Name = Hello
Exec = /usr/bin/chumby-hello
Icon = home
Short = Hi
Color = #3a9a5c
```

- `Icon` is a name from the shell's table (`icons[]` in `shell.c`):
  audio, bell, home, settings, image, list, wifi, gps, video, envelope,
  eye, file, charge, play, call, keyboard, and radio, clock, globe, sun
  (Material Icons, see `chumby_icon_font`). Add to it for new ones.
- `Short` (optional) is the name on the tab when `Name` is too long for
  one (tab titles are capped at 80 px and cut with dots).
- `Color` (optional) is the app's hue: its tab, the tab line, its icon in
  the Home menu, and its `CHUMBY_COLOR_ACCENT` (see Theme).
- The shell starts the app from the Home menu and won't start a second
  copy while the first is coming up (`LAUNCH_TIMEOUT_S`). An app exits
  when its window is closed (holding its tab) or the compositor goes.

To package it, add it to `package/chumby-apps` (`src/Makefile`: one line
per binary, linking `pkg-config --libs libchumby`; `chumby-apps.mk`:
install the binary and the `.app`).

## libchumby (package/libchumby/src/chumby.h)

Everything below is declared in `chumby.h`, with comments.

### Windows and the main loop

| | |
|---|---|
| `chumby_app_init(id)` | an app's window (320x208), returns its screen |
| `chumby_init()`, `chumby_window_create(id, w, h)`, `chumby_window_close(d)` | several windows (the shell does this) |
| `chumby_run()` | the main loop: LVGL timers, Wayland, your fds; exits when the compositor goes |
| `chumby_watch_fd(fd, cb, user)` | call `cb` when an fd is readable (input devices, pipes, FIFOs) |
| `chumby_spawn(path, argv)` | `posix_spawn` a helper; reap it yourself with `waitpid(pid, .., WNOHANG)` from a timer |

**Never block the UI.** Anything slow (network, the tuner's 2.5 s init,
`chumby-stream` stopping a stream) runs as a child process via
`chumby_spawn`, reaped from an LVGL timer (see `child_poll` in `fm.c`,
`poll_cb` in `netradio.c`).

### Theme

| | |
|---|---|
| `chumby_font(f)` | Nimbus bitmap fonts: `SMALL`/`BODY` 14 px, `LARGE` 19, `MEDIUM` 28 (14 at 2x), `XL` 35, `HUGE` 70 (35 at 2x). LVGL's `LV_SYMBOL_*` icons come through as fallback. |
| `chumby_icon_font(px)` | 14, 20 or 28 px: `CHUMBY_ICON_RADIO`/`CLOCK`/`GLOBE`/`SUN`, icons LVGL lacks (Material Icons, made by `fonts/icons2lvgl.py`; the output is committed). `LV_SYMBOL_*` work in it too. Add an icon: put it in `ICONS`, rerun the script, add a `CHUMBY_ICON_*`. |
| `chumby_color(c)` | `BG`, `SURFACE`, `TEXT`, `TEXT_DIM`, `BAR` (greys) and `ACCENT`, for the current mode |
| `chumby_glossy(obj, c, sel)`, `chumby_on_accent()` | fill with a colour, a touch lighter at the top; the text colour on it |
| `chumby_accent(rgb)` | another app's hue for the current mode (the shell) |
| `chumby_mode()`, `chumby_on_mode_change(cb, user)` | day (dark, blue accent) / night (black, warm red, low glare) |

- All text is Nimbus (Rockbox's BDFs, converted at build time). Coverage:
  14 px has Latin (extended), Greek and Cyrillic; 19 px Latin-1 and
  Cyrillic; 35 px only Latin-1. Missing characters fall back to
  Montserrat (ASCII); no shaping, and no Vietnamese tone marks. Sizes:
  Rockbox has 10-14, 19 and 35; anything else is an integer scale
  (`nimbus_14x2` etc. in libchumby's Makefile).
- **Colour is for what's active only**, like the colour iPods: the app's
  tab, play while playing, a selected item, a slider's fill, the art
  placeholder, the "now playing" line. Everything else is grey (day: dark
  on light; night: soft grey on black). `CHUMBY_COLOR_ACCENT` is your
  app's `Color`; at night every accent is one warm, dim amber. Fill
  active things with `chumby_glossy()` and put `chumby_on_accent()` text
  on them; checked buttons get that from the theme already.
- Restyle in your mode callback: the palette changes at night. Night also
  dims things that glare (the FM/net radio art tile is drawn at 50 %).

### Layout helpers

| | |
|---|---|
| `chumby_group(parent, flow, gap)` | a transparent, content-sized flex row/column whose children are centred on each other (icon + label) |
| `chumby_center_y_on(obj, ref)` / `_between(obj, above, below)` / `_at(obj, y)` | centre an object vertically on the *visible* glyphs, not the font box: Nimbus's box sits high, so plain centring looks off |

Sizes for a resistive screen: anything a finger must hit is at least
`CHUMBY_TOUCH_MIN` (42 px). Smaller-looking buttons get the rest from
`lv_obj_set_ext_click_area()` (the FM app's scan, < > buttons).

### Text and images

| | |
|---|---|
| `chumby_scroll_label_init(l)`, `chumby_scroll_label_set(l, text)` | a one-line label that, given new text too long for it, scrolls through it twice (after a pause) and stops, cut with dots |
| `chumby_label_scroll_speed(l, text, px_s)` | for your own scrolling labels: `CHUMBY_SCROLL_SPEED` (20 px/s) whatever the text's length |
| `chumby_logo_set(img, path, box)` | a PNG or JPEG file (by extension) fitted into a box x box square |

### System

| | |
|---|---|
| `chumby_policy()` | `/etc/chumby/policy.conf`: night window, brightness, screen-off hours, snooze length, alarm volume |
| `chumby_time_valid()` | is the clock set (NTP synced, or the RTC holds a sane date)? Don't act on the time before. |
| `chumby_volume_get/set`, `chumby_brightness_get/set` | the codec's DAC level, the backlight (normally the shell's job) |
| `chumby_alarms_*`, `chumby_alarm_state()` | the alarms file the clock app edits and the shell rings |
| `chumby_run_dir()` | `/run/chumby` (or `$CHUMBY_RUN_DIR`): state shared between processes |
| `chumby_shell_command(cmd)` | the shell's FIFO: `snooze`, `stop`, `ring`, `volume`, `brightness`, `menu`, `calibrate` |
| `chumby_stay_awake(on)` | don't freeze me while out of sight |

## Sound

There is one sound card (the i.MX233's codec, ALSA `plughw:0,0`) and one
thing plays at a time. `chumby-audio`
(`board/falconwing-mainline/rootfs/usr/bin/chumby-audio`) decides:

```
chumby-audio play <source>      I'm playing now: every other source stops,
                                for good (FM off, the stream stopped)
chumby-audio duck <source>      I need the sound for a while (an alarm):
                                the playing sources pause...
chumby-audio release <source>   ...and resume once I'm done
chumby-audio stop               everything stops (the volume control's stop
                                button; snooze while that control is open)
chumby-audio list               who's registered, who plays, who's paused
```

A **source** is an executable in `/usr/share/chumby/audio.d/<name>` taking
`stop | pause | resume | active` (exit status: playing?). Today:

- `fm` -> the `radio` script. `stop` = audio loop off and the tuner chip
  to standby (`qn8005 sleep`); `pause` keeps it tuned for a quick resume.
- `stream` -> `chumby-stream`. `pause` remembers the URL and name;
  `resume` reconnects to it.

**To add a player** (local music, a podcast app...):

1. Do the playing in a background process (so freezing or closing the UI
   doesn't cut it), with a pidfile or some way to tell if it's running.
2. Write `audio.d/<name>` mapping the four verbs onto it. `stop` and
   `pause` must not return until the sound card is free.
3. Call `chumby-audio play <name>` *before* opening the sound card when
   the user starts it. Its `resume` must **not** call `chumby-audio`
   (chumby-audio holds its lock while resuming you).
4. Daemons started from a source must not inherit chumby-audio's lock fd:
   chumby-audio runs sources with fd 9 closed, but close inherited fds in
   your daemon anyway (`chumby-stream` does after forking).

For short sounds over whatever plays (a chime), `duck` / `release`
around them. The alarm does exactly this; snoozing stays quiet and the
radio comes back when the alarm is dismissed.

Volume is the codec's `DAC` control, owned by the shell (knob, slider);
players just play.

### Ready-made players

- **`chumby-stream <url> [name]`**: internet radio, in the background.
  MP3 (libmpg123) or AAC (faad2's fixed-point build: the ARM926 has no
  FPU), Shoutcast/Icecast with ICY titles, HLS (`.m3u8`, TS or packed
  audio), `.pls`/`.m3u`. Reconnects with backoff, buffers ~2 s ahead,
  resamples through ALSA. Its state is in `$run_dir/stream` (`state=`,
  `url=`, `name=`, `title=`, `codec=`, `bitrate=`), gone when stopped.
- **`radio on|off|tune|scan|pause|resume|active`**: the FM tuner (QN8005)
  and its audio loop; RDS goes to `$run_dir/rds` (`ps`, `rt`, `title`,
  `artist`, `psdyn`, `pstext`, `rssi`). See the script's header.

## LVGL 9.6 traps we've hit

- **Hidden windows and frame callbacks**: LVGL's Wayland backend blocks
  after each flush until the frame callback arrives. chumby-wm sends them
  to hidden windows too (frozen ones excepted); if you write another
  compositor, do the same, or the client hangs.
- **Closing a window, or deleting a widget, from inside its own event
  callback**: defer it with `lv_async_call()` (the shell's menu, the net
  radio list).
- **Labels**: `LV_LABEL_LONG_MODE_DOTS` needs a fixed height, or it wraps
  instead of cutting. A max-width alone wraps too: measure, then set the
  width. `SCROLL_CIRCULAR` restarts mid-way when the text changes unless
  you delete its animation first (`chumby_scroll_label_set` does).
- **JPEG**: TJPGD decodes tile by tile, and LVGL can't scale images
  decoded like that: they just don't draw. `chumby_logo_set()` decodes
  them whole first. Progressive JPEGs aren't supported at all.
- **Transparency**: windows are RGB565. For a window with see-through
  parts (rounded corners), call
  `lv_wayland_window_set_next_color_format(LV_COLOR_FORMAT_ARGB8888)`
  right before creating it (our LVGL patch; the shell's menu and slider).
- **Deprecated flag calls**: use `lv_obj_set_scrollable()`,
  `lv_obj_set_clickable()`, `lv_obj_set_hidden()` rather than
  `lv_obj_add_flag()` / `remove_flag()`.
- **Private headers**: some things (image decoder internals) need
  `lvgl/src/...private.h`; prefer adding a helper to libchumby.

## Input

Touch comes through the compositor (libinput, calibrated: hold the snooze
button 4 s to recalibrate; the result is `/var/lib/chumby/touch.cal`).
The knob and the snooze button are read by the shell directly: the knob
is volume everywhere, snooze toggles the screen / snoozes alarms. Apps
don't get them.

## Testing and deploying

- **Build** inside the container (see `CLAUDE.md`), e.g.
  `make ... chumby-apps-rebuild` then `make` for the image.
- **Headless**: the target's binaries run on the host under qemu-user,
  against a headless chumby-wm (`WLR_BACKENDS=headless
  WLR_RENDERER=pixman`, `QEMU_LD_PREFIX` = the target dir; `chumby-wm -s
  chumby-shell`, then run the app as a client with `WAYLAND_DISPLAY` set).
  qemu redirects absolute paths into the target dir, so `/usr/share`
  files are found. `chumby-screenshot out.ppm` captures the screen; the
  shell's FIFO (`$CHUMBY_RUN_DIR/shell.cmd`) opens menus and sliders.
  Use it for screenshots before deploying UI changes.
- **On the board**: `bin/update-chumby.sh <host>` updates the SD card
  over SSH (rsync deltas) and reboots. To try a binary first, copy it to
  `/tmp` and run it there; the root filesystem is read-only.
