# Writing Octopus apps

How the UI and sound fit together in Octopus OS, on a Raspberry Pi
(`rpi64`) or the Chumby One (`falconwing`), what's already there to build
on, and the traps we fell into so you don't have to. Read
this before writing an app; the headers it points at have the details.

## The pieces

```
octopus-wm (wlroots compositor, package/octopus-wm)
 ├── octopus-shell  (package/octopus-shell)  tab bar, Home menu, volume /
 │                  brightness sliders, alarms, buttons, screen power
 ├── system apps   (package/octopus-apps)  Clock, AirPlay, Settings, FM
 │                  Radio (Chumby One); the stream player octopus-stream
 └── app bundles   Internet Radio, Podcasts (apps/, preinstalled in
                    /usr/lib/octopus/apps), and anyone's (the SDK, installed
                    in /var/apps): one Wayland client each
liboctopus (package/liboctopus)  LVGL 9.6 compiled in, plus Octopus's
                    framework every app and the shell link against
```

- **Screen**: 320x240 (on a bigger screen, scaled up by a whole number:
  apps don't see it). The shell's tab bar is the top `OCTOPUS_BAR_H` (32)
  px; an app gets `OCTOPUS_APP_W` x `OCTOPUS_APP_H` (320x208) under it. The
  compositor places windows by their app_id (see the top of
  `octopus-wm.c`); an app doesn't position itself.
- **One app in front at a time.** The tab bar shows it (filled tab, with
  icon and name) and the one before it. Home is a menu of installed apps,
  not a place.
- **Apps out of sight are frozen** (`SIGSTOP`) until brought back
  (`SIGCONT`): zero CPU, state kept, their last frame still on screen. Your
  app just stops between two instructions and carries on later; timers
  fire late, all at once. If you must keep working out of sight (a
  download, a timer that has to fire), call `octopus_stay_awake(true)`, and
  `false` once done. Alarms don't need this: the shell rings them.
- **Sound runs in its own processes** (`octopus-stream`, the FM loop), so
  freezing or closing an app never cuts it. See [Sound](#sound).

## An app in 30 lines

```c
#include <octopus.h>

static lv_obj_t *label;

static void on_mode(octopus_mode_t mode, void *user)
{
	(void)mode; (void)user;
	/* Colors are per mode: set them here, not once at start. */
	lv_obj_set_style_text_color(label, octopus_color(OCTOPUS_COLOR_TEXT), 0);
}

int main(void)
{
	lv_obj_t *scr = octopus_app_init("Hello");	/* = Id in hello.app */

	lv_obj_set_scrollable(scr, false);
	label = lv_label_create(scr);
	lv_obj_set_style_text_font(label, octopus_font(OCTOPUS_FONT_LARGE), 0);
	lv_label_set_text(label, "Hello, Octopus");
	lv_obj_center(label);
	octopus_on_mode_change(on_mode, NULL);	/* calls it once now, too */
	return octopus_run();
}
```

Plus a descriptor, `/usr/share/octopus/apps/hello.app`:

```
# Id must match the app_id the app passes to octopus_app_init().
Id = Hello
Name = Hello
Exec = /usr/bin/octopus-hello
Icon = home
Short = Hi
Color = #3a9a5c
```

- `Icon` is a name from the shell's table (`icons[]` in `shell.c`):
  audio, bell, home, settings, image, list, wifi, gps, video, envelope,
  eye, file, charge, play, call, keyboard, and radio, clock, globe, sun,
  podcast
  (Material Icons, see `octopus_icon_font`). Add to it for new ones.
- `Short` (optional) is the name on the tab when `Name` is too long for
  one (tab titles are capped at 80 px and cut with dots).
- `Color` (optional) is the app's hue: its tab, the tab line, its icon in
  the Home menu, and its `OCTOPUS_COLOR_ACCENT` (see Theme).
- The shell starts the app from the Home menu and won't start a second
  copy while the first is coming up (`LAUNCH_TIMEOUT_S`). An app exits
  when its window is closed (holding its tab) or the compositor goes.

An app is a **bundle** (a directory: `app.conf` like the above, with
`Exec` relative to it, plus the program and its files): build it with the
SDK ([sdk.md](sdk.md), the walk-through in
[sdk-tutorial.md](sdk-tutorial.md)) and install it from a USB stick or
over the network. To ship one in the image, put it in `apps/<name>/` (a
Makefile making `bundle/`, as the SDK's examples do):
`package/octopus-bundled-apps` builds every app there and preinstalls it,
read-only, in `/usr/lib/octopus/apps/<Id>/` - Internet Radio and Podcasts
are made so, and are no different from anyone's. Only the system's own
apps (Clock, FM, AirPlay, Settings: tied to the hardware, the alarms or
root's settings) are in `package/octopus-apps`, with `.app` descriptors in
`/usr/share/octopus/apps`.

## liboctopus (package/liboctopus/src/octopus.h)

Everything below is declared in `octopus.h`, with comments.

### Windows and the main loop

| | |
|---|---|
| `octopus_app_init(id)` | an app's window (320x208), returns its screen |
| `octopus_init()`, `octopus_window_create(id, w, h)`, `octopus_window_close(d)` | several windows (the shell does this) |
| `octopus_run()` | the main loop: LVGL timers, Wayland, your fds; exits when the compositor goes |
| `octopus_watch_fd(fd, cb, user)` | call `cb` when an fd is readable (input devices, pipes, FIFOs) |
| `octopus_spawn(path, argv)` | `posix_spawn` a helper; reap it yourself with `waitpid(pid, .., WNOHANG)` from a timer |

**Never block the UI.** Anything slow (network, the tuner's 2.5 s init,
`octopus-stream` stopping a stream) runs as a child process via
`octopus_spawn`, reaped from an LVGL timer (see `child_poll` in `fm.c`,
`poll_cb` in `apps/netradio/netradio.c`).

### Theme

| | |
|---|---|
| `octopus_font(f)` | Nimbus bitmap fonts: `SMALL`/`BODY` 14 px, `LARGE` 19, `MEDIUM` 28 (14 at 2x), `XL` 35, `HUGE` 70 (35 at 2x). LVGL's `LV_SYMBOL_*` icons come through as fallback. |
| `octopus_icon_font(px)` | 14, 20 or 28 px: `OCTOPUS_ICON_RADIO`/`CLOCK`/`GLOBE`/`SUN`, icons LVGL lacks (Material Icons, made by `fonts/icons2lvgl.py`; the output is committed). `LV_SYMBOL_*` work in it too. Add an icon: put it in `ICONS`, rerun the script, add a `OCTOPUS_ICON_*`. |
| `octopus_color(c)` | `BG`, `SURFACE`, `TEXT`, `TEXT_DIM`, `BAR` (greys) and `ACCENT`, for the current mode |
| `octopus_glossy(obj, c, sel)`, `octopus_on_accent()` | fill with a colour, a touch lighter at the top; the text colour on it |
| `octopus_accent(rgb)` | another app's hue for the current mode (the shell) |
| `octopus_mode()`, `octopus_on_mode_change(cb, user)` | day (dark, blue accent) / night (black, warm red, low glare) |

- All text is Nimbus (Rockbox's BDFs, converted at build time). Coverage:
  14 px has Latin (extended), Greek and Cyrillic; 19 px Latin-1 and
  Cyrillic; 35 px only Latin-1. Missing characters fall back to
  Montserrat (ASCII); no shaping, and no Vietnamese tone marks. Sizes:
  Rockbox has 10-14, 19 and 35; anything else is an integer scale
  (`nimbus_14x2` etc. in liboctopus's Makefile).
- **Colour is for what's active only**, like the colour iPods: the app's
  tab, play while playing, a selected item, a slider's fill, the art
  placeholder, the "now playing" line. Everything else is grey (day: dark
  on light; night: soft grey on black). `OCTOPUS_COLOR_ACCENT` is your
  app's `Color`; at night every accent is one warm, dim amber. Fill
  active things with `octopus_glossy()` and put `octopus_on_accent()` text
  on them; checked buttons get that from the theme already.
- Restyle in your mode callback: the palette changes at night. Night also
  dims things that glare (the FM/net radio art tile is drawn at 50 %).

### Layout helpers

| | |
|---|---|
| `octopus_group(parent, flow, gap)` | a transparent, content-sized flex row/column whose children are centred on each other (icon + label) |
| `octopus_center_y_on(obj, ref)` / `_between(obj, above, below)` / `_at(obj, y)` | centre an object vertically on the *visible* glyphs, not the font box: Nimbus's box sits high, so plain centring looks off |

Sizes for a resistive screen: anything a finger must hit is at least
`OCTOPUS_TOUCH_MIN` (42 px). Smaller-looking buttons get the rest from
`lv_obj_set_ext_click_area()` (the FM app's scan, < > buttons).

### Text and images

| | |
|---|---|
| `octopus_scroll_label_init(l)`, `octopus_scroll_label_set(l, text)` | a one-line label that, given new text too long for it, scrolls through it twice (after a pause) and stops, cut with dots |
| `octopus_label_scroll_speed(l, text, px_s)` | for your own scrolling labels: `OCTOPUS_SCROLL_SPEED` (20 px/s) whatever the text's length |
| `octopus_logo_set(img, path, box)` | a PNG or JPEG file (by extension) fitted into a box x box square |

### System

| | |
|---|---|
| `octopus_policy()` | `/etc/octopus/policy.conf`: night window, brightness, screen-off hours, snooze length, alarm volume |
| `octopus_time_valid()` | is the clock set (NTP synced, or the RTC holds a sane date)? Don't act on the time before. |
| `octopus_volume_get/set`, `octopus_brightness_get/set` | the codec's DAC level, the backlight (normally the shell's job) |
| `octopus_alarms_*`, `octopus_alarm_state()` | the alarms file the clock app edits and the shell rings |
| `octopus_run_dir()` | `/run/octopus` (or `$OCTOPUS_RUN_DIR`): state shared between processes |
| `octopus_shell_command(cmd)` | the shell's FIFO: `snooze`, `stop`, `ring`, `volume`, `brightness`, `menu`, `calibrate` |
| `octopus_stay_awake(on)` | don't freeze me while out of sight |

## Sound

Players open ALSA's `octopus` (or `default`) device: on a Pi that's
PipeWire, through the speaker chain (`board/rpi64/README.md`); on the
Chumby One, its codec directly. One thing plays at a time, and
`octopus-audio` (`board/falconwing/rootfs/usr/bin/octopus-audio`, on both
boards) decides:

```
octopus-audio play <source>      I'm playing now: every other source stops,
                                for good (FM off, the stream stopped)
octopus-audio duck <source>      I need the sound for a while (an alarm):
                                the playing sources pause...
octopus-audio release <source>   ...and resume once I'm done
octopus-audio stop               everything stops (the volume control's stop
                                button; snooze while that control is open)
octopus-audio playing            exit status 0 if anything plays
octopus-audio list               who's registered, who plays, who's paused
```

A **source** is an executable in `/usr/share/octopus/audio.d/<name>` taking
`stop | pause | resume | active` (exit status: playing?). Today:

- `fm` (the Chumby One) -> the `radio` script. `stop` = audio loop off
  and the tuner chip to standby (`qn8005 sleep`); `pause` keeps it tuned
  for a quick resume.
- `stream` -> `octopus-stream`. `pause` remembers the URL and name (and,
  in a podcast episode, the byte it was at); `resume` reconnects to it.

**To add a player** (local music, a podcast app...):

1. Do the playing in a background process (so freezing or closing the UI
   doesn't cut it), with a pidfile or some way to tell if it's running.
2. Write `audio.d/<name>` mapping the four verbs onto it. `stop` and
   `pause` must not return until the sound card is free.
3. Call `octopus-audio play <name>` *before* opening the sound card when
   the user starts it. Its `resume` must **not** call `octopus-audio`
   (octopus-audio holds its lock while resuming you). When it stops by
   itself (its own stop button, the end, an error), call
   `octopus-audio stopped <name>`: it takes no lock, and keeps
   `$run_dir/audio/playing` (what plays; the shell's stop button watches
   it) right.
4. Daemons started from a source must not inherit octopus-audio's lock fd:
   octopus-audio runs sources with fd 9 closed, but close inherited fds in
   your daemon anyway (`octopus-stream` does after forking).

For short sounds over whatever plays (a chime), `duck` / `release`
around them. The alarm does exactly this; snoozing stays quiet and the
radio comes back when the alarm is dismissed.

Volume is owned by the shell (its slider; the Chumby One's knob): ALSA's
`Master` on a Pi, the codec's `DAC` on the Chumby One. Players just play.

### Ready-made players

- **`octopus-stream <url> [name]`**: internet radio, in the background.
  MP3 (libmpg123) or AAC (faad2's fixed-point build, for the Chumby One's
  FPU-less ARM9), Shoutcast/Icecast with ICY titles, HLS (`.m3u8`, TS or packed
  audio), `.pls`/`.m3u`, and MP4 audio files (`.m4a`: AAC, index first or
  last). Reconnects with backoff, buffers ~2 s ahead,
  resamples through ALSA. Its state is in `$run_dir/stream` (`state=`,
  `url=`, `name=`, `title=`, `codec=`, `bitrate=`), gone when stopped.
  **`octopus-stream <url> <name> <byte>`** plays a file (a podcast episode)
  from a byte on (an HTTP range); for a file the state adds `byte=` (where
  it is: resume from there), `length=` and `played=` (s), about once a
  second, and at its end it's `state=ended` (left in place for the app).
- **`octopus-podcast-feed <feed> <cache> [n]`**: a podcast's RSS, parsed as
  it downloads (expat), its newest n episodes (30) as tab-separated lines:
  published (epoch), duration (s), bytes, audio URL, title.
- **`radio on|off|tune|scan|pause|resume|active`** (the Chumby One): the
  FM tuner (QN8005) and its audio loop; RDS goes to `$run_dir/rds` (`ps`, `rt`, `title`,
  `artist`, `psdyn`, `pstext`, `rssi`). See the script's header.

## LVGL 9.6 traps we've hit

- **Hidden windows and frame callbacks**: LVGL's Wayland backend blocks
  after each flush until the frame callback arrives. octopus-wm sends them
  to hidden windows too (frozen ones excepted); if you write another
  compositor, do the same, or the client hangs.
- **Closing a window, or deleting a widget, from inside its own event
  callback**: defer it with `lv_async_call()` (the shell's menu, the net
  radio list).
- **Labels**: `LV_LABEL_LONG_MODE_DOTS` needs a fixed height, or it wraps
  instead of cutting. A max-width alone wraps too: measure, then set the
  width. `SCROLL_CIRCULAR` restarts mid-way when the text changes unless
  you delete its animation first (`octopus_scroll_label_set` does).
- **JPEG**: TJPGD decodes tile by tile, and LVGL can't scale images
  decoded like that: they just don't draw. `octopus_logo_set()` decodes
  them whole first. Progressive JPEGs aren't supported at all.
- **Transparency**: windows are RGB565. For a window with see-through
  parts (rounded corners), call
  `lv_wayland_window_set_next_color_format(LV_COLOR_FORMAT_ARGB8888)`
  right before creating it (our LVGL patch; the shell's menu and slider).
- **Deprecated flag calls**: use `lv_obj_set_scrollable()`,
  `lv_obj_set_clickable()`, `lv_obj_set_hidden()` rather than
  `lv_obj_add_flag()` / `remove_flag()`.
- **Private headers**: some things (image decoder internals) need
  `lvgl/src/...private.h`; prefer adding a helper to liboctopus.

## Input

Touch comes through the compositor (libinput, calibrated: hold the snooze
button 4 s to recalibrate; the result is `/var/lib/octopus/touch.cal`).
The knob and the snooze button are read by the shell directly: the knob
is volume everywhere, snooze toggles the screen / snoozes alarms. Apps
don't get them.

## Testing and deploying

- **Build** inside the container (see `CLAUDE.md`), e.g.
  `make ... octopus-apps-rebuild` then `make` for the image.
- **Headless**: the target's binaries run on the host under qemu-user,
  against a headless octopus-wm (`WLR_BACKENDS=headless
  WLR_RENDERER=pixman`, `QEMU_LD_PREFIX` = the target dir; `octopus-wm -s
  octopus-shell`, then run the app as a client with `WAYLAND_DISPLAY` set).
  qemu redirects absolute paths into the target dir, so `/usr/share`
  files are found. `octopus-screenshot out.ppm` captures the screen; the
  shell's FIFO (`$OCTOPUS_RUN_DIR/shell.cmd`) opens menus and sliders.
  Use it for screenshots before deploying UI changes.
- **On a device**: `bin/update-rpi64.sh <host>` (a Pi) or
  `bin/update-chumby.sh <host>` (the Chumby One) updates it over SSH
  (rsync deltas) and reboots. For apps alone, the SDK's
  `octopus-app deploy` is quicker (docs/sdk.md).
