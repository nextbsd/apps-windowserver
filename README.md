# apps-windowserver

WindowServer for NextBSD: a native display server in the style of NeXTSTEP's and macOS's Window Server.

It owns the screen, input, window ordering and application activation. GNUstep apps draw their own windows and hand the finished pixels to WindowServer, which composites them. There is no X11 and no Wayland in between. XLibre stays available for X11 apps.

This is a proof of concept. It isn't usable yet.

![Terminal running on WindowServer: its NeXT-style main menu at the top left, a zsh window, and its app icon at the bottom left, on a black screen](docs/terminal-headless.png)

Terminal running on WindowServer through the GNUstep `nextbsd` backend, captured in headless mode with `wsctl snapshot`.

## NextBSD only

WindowServer is built on what NextBSD provides, and isn't meant to run anywhere else:

- **launchd** starts it at boot as a LaunchDaemon. Each user's session apps are LaunchAgents.
- **Mach** carries messages between apps and the server. Apps find it as a Mach service through the bootstrap.
- **IOKit** reports GPUs, displays and input devices as they appear.
- **DRM/KMS** from NextBSD's kernel extensions drives the display, with Mesa's EGL and GBM for GPU compositing.
- **libinput** and **libxkbcommon** read the keyboard, mouse and touchpad.

## How the pieces fit

| Piece | Repository | What it does |
|---|---|---|
| WindowServer | this repository | The daemon: display, input, compositing, window levels, focus and the active app |
| `nextbsd` backend | [nextbsd/libs-back](https://github.com/nextbsd/libs-back) | The GNUstep backend each app loads (`libgnustep-nextbsd`). It draws with cairo into shared buffers and talks to WindowServer over Mach |
| Build script | [pkgdemon/nextbsd-gnustep](https://github.com/pkgdemon/nextbsd-gnustep) | `build-windowserver.sh` builds the backend and WindowServer on top of the GNUstep core libraries |

libs-base and libs-gui are unmodified. The `nextbsd` backend installs beside the stock X11 backend, and the `GSBackend` default chooses between them, per app or for the whole session.

## Where it installs

| What | Path |
|---|---|
| Server | `/System/Library/CoreServices/WindowServer` |
| launchd job | `/System/Library/LaunchDaemons/org.nextbsd.WindowServer.plist` |
| Defaults | `/System/Library/Preferences/org.nextbsd.WindowServer.plist` |
| Client library | `/usr/lib/libWindowServerClient.so.1` and `.a` |
| Headers | `/usr/include/WindowServer/` |
| pkgconf file | `/usr/libdata/pkgconfig/WindowServerClient.pc` |
| Test tools | `/usr/libexec/WindowServer/` (`wstest`, `wsctl`, `ws-session`) |
| Client socket | `/var/run/WindowServer/ws.sock` |
| Account | `_windowserver` (uid 88) |

WindowServer starts as root to open the GPU and input devices, then runs as `_windowserver`. If that account doesn't exist yet, it says so and stays root.

The client library is a plain C library with no dependencies beyond libc and NextBSD's Mach libraries, so it goes in `/usr/lib` with its headers in `/usr/include`, next to libdrm and libinput. The GNUstep backend links it with `pkgconf --libs WindowServerClient`.

## Status

Phase 1, the software path, is written. The server, the client library and two test tools build, and the headless test passes on the arm64 VM. The DRM, VT and libinput code builds but hasn't run on real hardware yet. There is no GNUstep backend yet, so no real apps can use it. This is still a proof of concept.

What works, tested headless:

- The Mach service, the per-client socket and buffer passing.
- Windows stacked strictly by level (the libs-gui `NSWindowLevel` numbers), then by order within a level.
- One active app. A click in an inactive app's window deactivates the old app, activates the new one, raises the window, makes it key, and then delivers the click.
- Keys go only to the key window of the active app, with server-side key repeat.
- Damage-tracked compositing with pixman, buffer release and frame-done events.
- Minimized windows as live tiles along the bottom of the screen (see [Minimized windows](#minimized-windows)).
- The Dock's view of every app's windows, with change notices, activation, restoring and previews (see [The Dock](#the-dock)).
- Snapshots and synthetic input for testing.
- Touchpad scroll phases and momentum scrolling, with synthetic two-finger scrolls (see [Touchpad](#touchpad)).
- The window overview, Exposé and App Exposé, opened by a Start tap, Control+Up/Down, the Mission Control key or a three-finger swipe (see [The overview](#the-overview)).

Written but only compiled:

- DRM/KMS output. It picks the card that has a connected display, sets the preferred mode, uses two dumb buffers with page flips (or `drmModeSetCrtc` if flips fail), and the hardware cursor, with a software cursor as fallback.
- VT handling: no VT switching. The server takes over the VT that is on screen when it starts, puts it in `KD_GRAPHICS` and `K_RAW` with a raw termios, and locks switching with `VT_LOCKSWITCH`, so Ctrl+Alt+F*n* and `vidcontrol -s` can't take the screen away. It refuses to start if that VT is already in graphics mode or controlled by another process (Xorg, or another WindowServer). On exit, `SIGTERM`/`SIGINT`/`SIGHUP` or a crash it unlocks switching, gives the same VT back its VT mode, `KD_TEXT`, keyboard mode and termios, and drops DRM master, so the console comes back as it was.
- libinput on seat0 through the IOKit libudev, with xkbcommon (`evdev` rules), relative and absolute pointers, wheel and touchpad scrolling, and the touchpad settings in [Touchpad](#touchpad). Power and sleep keys are logged, never typed.
- Dropping from root to `_windowserver` after the devices are open.

Not done yet:

- The GNUstep backend (`libgnustep-nextbsd` in the libs-back fork) and a real `ws-session`.
- Waiting for the GPU kext's IOKit match, and display hotplug through IOKit.
- Images for the standard cursors other than the arrow.
- Touch, tablets, gestures other than swipes up and down, and switches. Per-device keyboard settings.
- Skipping windows hidden behind opaque ones while compositing.
- GPU compositing (EGL/GBM), several displays, HiDPI.
- A focus-stealing policy: today any app can make itself active.
- loginwindow, the lock shield and the app switcher. Their window levels are reserved.

## Building

On NextBSD, with the base display stack (libdrm, libinput, libxkbcommon, pixman) and gmake:

```sh
gmake
gmake check          # the headless test, see below
gmake install        # DESTDIR=... and the *DIR variables in GNUmakefile work
```

Everything is built into `build/`.

## Trying it without a display

Headless mode needs no GPU, VT or input devices and no privileges, so it runs anywhere, including over SSH while X is using the screen:

```sh
build/WindowServer --headless 1280x800 &
build/wstest &                 # apps Alpha and Beta (+ Gamma, joined to Alpha), four windows
build/wsctl list               # the window stack, bottom to top
build/wsctl pixel 350 300      # the colour on screen at 350,300
build/wsctl click 150 450      # click into Alpha's window
build/wsctl key a              # type "a" into the active app
build/wsctl snapshot out.ppm   # save the screen
```

`wstest` prints every event it gets. `tests/headless-test.sh` (`gmake check`) does all of this with its own service name and socket, and checks the screen colours, the order of the activation events, that only the active app gets keys, joining an app, the XKB events, XRGB8888 buffers, restack notices, and minimizing: the tile's picture, icon and live update, selecting and dragging it, and restoring the window with a double-click. It also checks the Dock's requests (`wsctl dock`): that apps can't list other apps' windows, the list with app ids, change notices, minimized windows going into the Dock instead of tiles, previews, activating and restoring from the Dock, and the tiles coming back when the Dock goes away. Then it checks touchpad scrolling: a two-finger flick's phases, momentum that slows down and stops when a finger rests on the touchpad, and no momentum when the fingers stopped before lifting. Last, the overview: opening it with a Start tap (animated), Control+Up, the Mission Control key and three-finger swipes, but not with Command+Q or Start held over a click; its layout (every app's windows, grouped by app, active app first, minimized ones included, menus left out, nothing overlapping, on screen), the dimmed desktop and live previews; keys and clicks going to no app; the arrow keys and Return, a click on a preview (which restores a minimized window), App Exposé, Escape, a second Start tap and a click on empty space.

`kill -USR1` makes the server write a snapshot to `/tmp/WindowServer-snapshot.ppm`.

## Running it on the display

Don't do this while Xorg is running: X owns the display. Log in on the text console you want to use, and as root:

```sh
ws-session --server build/WindowServer
```

`ws-session` loads a temporary launchd job for the server, so its Mach service is in the system bootstrap where every user's apps can find it, and stops it on Ctrl-C. The server takes over the VT that is on screen, the one you typed the command on, and VT switching stays locked while it runs. `Ctrl+Alt+Backspace` stops the server (the `AllowTerminate` default) and gives that console back as it was. From SSH, `ws-session stop` or `kill` (`SIGTERM`, not `SIGKILL`: a killed server can't restore the console) do the same. It only starts the server. A full desktop on it (gpbs, gdnc, the Dock, Workspace) comes from [loginwindow](https://github.com/nextbsd/apps-loginwindow)'s session.

You can also start `WindowServer` directly. That works too, but launchd then registers the Mach service in your own login's bootstrap, so only your own processes can find it.

The installed LaunchDaemon ships `Disabled`, because it would otherwise take the console at boot. `launchctl load -F` starts it. launchd-842 only gives the console to a job labelled `com.apple.WindowServer` (core.c, around line 2226), so `org.nextbsd.WindowServer` needs a small launchd patch before it can run as the real console owner.

## How apps talk to it

Apps use `libWindowServerClient` (`<WindowServer/WSClient.h>`). The wire format is in `<WindowServer/WSProtocol.h>`.

- **Mach** carries the requests: hello, create and destroy windows, frame, level, ordering, title, flags, attach a buffer, damage, commit, make key, cursor, activate and deactivate. Apps find the server by looking up `org.nextbsd.windowserver` (or `$WS_SERVICE_NAME`) in the bootstrap. The server reads each sender's pid from the Mach audit trailer.
- **A Unix socket** carries what Mach can't yet. NextBSD's Mach can't pass file descriptors or shared memory, so window buffers are memfds sent over the socket, and the server sends its events there too: input, key window and app activation changes, buffer release, frame done and screen info. Apps get one file descriptor to poll. The hello reply over Mach tells the app where the socket is and gives it a one-time token to connect with.

Window buffers are premultiplied ARGB8888, drawn by the app (with cairo, in the GNUstep backend), or XRGB8888 (`ws_buffer_create_format()`, `WS_FORMAT_XRGB8888` in `ws_sock_buffer_t.format`), whose top byte is ignored and whose window is always composited opaque, as an X server's depth-24 windows need. Servers that take XRGB8888 set `WS_SERVER_XRGB`. After a buffer is sent once, each frame only sends damage rectangles and a commit.

### One app, several connections

Each connection is normally an app of its own. A helper process can join another connection's app instead, so that, for example, the X server XLibre.app starts and XLibre.app itself are one app:

- The app passes its client id and token (`ws_client_id()`, `ws_client_token()`; in GNUstep, `-[NextBSDServer windowServerAppCredentials]`) to the helper, which connects with `ws_connect_join()`. On the wire this is `WS_CONNECT_JOIN` in `ws_req_hello_t.flags` plus `join_client` and `join_token`, an optional tail older clients don't send. The hello reply's `app_id` names the app the connection belongs to.
- Only a process given the token can join; a wrong id or token fails the hello with `EPERM`. Servers that can do this set `WS_SERVER_JOIN`; older ones leave `app_id` 0, and `ws_connect_join()` fails with `ENOTSUP`.
- All connections of an app are activated and deactivated together (`WS_EVENT_APP_ACTIVATED`/`DEACTIVATED` go to each). They share one key window, which can be any connection's window; clicking a joined connection's window only moves the key window. Key events go to the connection that owns the key window, except Command shortcuts: when that is a joined connection, a key pressed with Command held goes to the app's own connection (with window 0), so its menus get it, and so do that key's repeats and release.
- If the app's first connection goes away, the oldest remaining connection carries the app on.

### XKB state for X servers

Key events carry the keycode, the keysym, text and six coarse `WS_MOD_*` bits, which is all a GNUstep app needs. A client that runs its own XKB, such as an X server, connects with `WS_CONNECT_XKB` (servers that support it set `WS_SERVER_XKB`) and also gets:

- `WS_EVENT_KEYMAP` (`ws_event_keymap_t`), right after the welcome and again if the keymap changes: the compiled keymap (`xkb_keymap_get_as_string()`, text v1, NUL-terminated) in a sealed, read-only memfd passed like a snapshot's, plus the repeat delay and interval in milliseconds. The client maps it with `MAP_PRIVATE` and closes the descriptor.
- `WS_EVENT_MODIFIERS` (`ws_event_modifiers_t`): depressed, latched and locked modifiers and the effective layout group as XKB masks, like Wayland's `wl_keyboard.modifiers`, after any key that changes them.
- `WS_EVENT_KEYS_HELD` (`ws_event_keys_held_t`): the XKB keycodes still held down when the connection starts getting key events (its app is activated, or the key window moves to one of its windows), followed by `WS_EVENT_MODIFIERS`. Their key-ups come to it later, so nothing sticks.

Clients without the flag get none of these.

### Restack notices

When the server itself moves a window in the stack or orders it out (today that is RaiseOnClick), it sends the window's owner `WS_EVENT_WINDOW_ORDERED` (`ws_event_ordered_t`): `WS_ORDER_ABOVE` with the owner's own nearest visible windows below and above it, or `WS_ORDER_OUT`. A client's own ordering, level and destroy requests never cause one. An X server uses it to keep its stacking order right; clients that don't care ignore it. Servers that send it set `WS_SERVER_ORDERED`.

### Touchpad

Two-finger scrolling works as on a Mac. Servers that do this set `WS_SERVER_SCROLL_PHASES`.

- **Scroll phases.** `ws_event_scroll_t` has two fields at its end, `phase` and `momentum_phase`, with the `NSEventPhase` values (`WS_SCROLL_PHASE_*`). A touchpad scroll (`WS_SCROLL_FINGER`) is one `BEGAN` event, `CHANGED` events, and an `ENDED` event with no delta when the fingers lift. All of it goes to the window the scroll started over. Wheel and other scrolls have both phases `NONE`. Older clients read the event without the new fields, and older servers send it without them, so they read as 0 (`NONE`). `WS_PROTOCOL_VERSION` stays 1.
- **Momentum scrolling.** If the fingers were still moving when they lifted, the scroll carries on from their speed and slows down with friction, about 1.5 s after a fast flick: events with `phase` `NONE` and `momentum_phase` `BEGAN`, `CHANGED`..., then `ENDED` with no delta. It stops at once when a finger touches the touchpad again (libinput's hold gesture, a new scroll, pointer motion or a click). Apps that don't look at the phases just see more scroll events.
- **Clicks.** Tapping is a click (one finger left, two right, three middle); tap and drag is on, without drag lock. A two-finger click is a secondary click (libinput's clickfinger method), or a click in the bottom-right corner, or neither, as `SecondaryClick` says.
- **Three-finger drag**, macOS's accessibility option, is off by default. When it's on, libinput turns three-finger swipes into four-finger ones, and the WindowServer gestures that use three fingers need four.

| Key | Default | |
|---|---|---|
| `TapToClick` | `true` | Tap to click. |
| `TapToDrag` | `true` | Tap, then drag (no drag lock). |
| `SecondaryClick` | `TwoFingers` | `TwoFingers` (a two-finger click or tap), `BottomRightCorner`, or `Off` (every touchpad click and tap is a left click). |
| `ThreeFingerDrag` | `false` | Drag with three fingers without clicking. |
| `MomentumScrolling` | `true` | Scrolling carries on after the fingers lift. |
| `NaturalScrolling` | `false` | Content follows the fingers. |
| `PointerSpeed` | `0` | libinput's pointer acceleration speed, -1 (slowest) to 1, for every pointing device. |
| `ScrollSpeed` | `1` | Multiplies the scroll distance (touchpad, trackpoint and wheel pixel deltas, not the wheel's detents). |

`wsctl scroll DX DY finger` injects one two-finger scroll step (`0 0` lifts the fingers), `wsctl flick DX DY [STEPS]` a quick two-finger scroll and lift, and `wsctl hold` fingers resting on the touchpad, like the rest of the synthetic input: on a headless server, or for root and the server's own user.

### The overview

Exposé and Mission Control, without workspaces yet: WindowServer shows every window at once, and apps don't change. Servers that have it set `WS_SERVER_OVERVIEW`.

![The overview: two Terminal windows with Terminal's icon under them on the left, Workspace's File Viewer with the NextBSD cube on the right, the frontmost Terminal window selected with a blue border, over the dimmed desktop](docs/overview.png)

- **Opening it.** Tap the Start key on its own (pressed and released with no other key or click in between, so Command shortcuts work as before; `StartKeyShowsOverview`), press Control+Up or the Mission Control key (`KEY_SCALE`, F3 on Apple keyboards), or swipe up with three fingers.
- **App Exposé.** Control+Down or a three-finger swipe down show only the active app's windows. A swipe up from there shows every window.
- **What it shows.** Every normal and floating window, visible or minimized, as a live scaled-down preview that follows what the app draws. Windows are grouped by app, the active app first, with the app's icon (`WS_MSG_SET_APP_ICON`) under each group. They're laid out without overlap and never enlarged, in as many group columns as give the largest previews. Desktop-level windows stay, dimmed; menus, the Dock, app icons and tiles fade out. WindowServer draws no text, so there are no titles. A workspace strip comes with workspaces (#13).
- **Choosing.** Hovering or the arrow keys (and Tab) select a window. A click or Return activates its app with that window in front and key, or restores a minimized one as its tile does. Escape, a click on empty space, Control+Up again or another Start tap close the overview. While it's open, no app gets pointer or key events, except modifier keys.
- **Animation.** Windows move from their places into the overview and back, about 0.25 s with an ease-out, while the desktop dims. `ReduceMotion` turns the animations off.
- **Three-finger drag.** With `ThreeFingerDrag` on, the swipes take four fingers.

| Key | Default | |
|---|---|---|
| `StartKeyShowsOverview` | `true` | A tap of the Start key on its own opens and closes the overview. |
| `ReduceMotion` | `false` | No animations. |

`WS_MSG_OVERVIEW` (`ws_overview()`, from `WS_CONNECT_CONTROL` and `WS_CONNECT_DOCK` connections, with the window list's user check) opens it (`WS_OVERVIEW_ALL`, `WS_OVERVIEW_APP`), closes it (`WS_OVERVIEW_CLOSE`) or only asks (`WS_OVERVIEW_QUERY`). It's answered with `WS_EVENT_OVERVIEW_STATE`, then a `WS_EVENT_OVERVIEW_ITEM` for each window with where its preview and its group's icon are, then `WS_EVENT_LIST_END`. Dock connections get `WS_EVENT_OVERVIEW_STATE` when it starts opening and when it has closed.

`wsctl overview [all|app|close|status]` does this from the command line, and `wsctl swipe FINGERS DX DY [STEPS]` injects a touchpad swipe, like the rest of the synthetic input.

### Minimized windows

WindowServer minimizes windows itself, macOS-style, instead of leaving each app to draw a miniwindow. A minimized window becomes a tile in the bottom row of the screen: a scaled-down picture of the window's current buffer with the app's icon in the bottom-right corner, and no text. The tile is redrawn from every frame the app commits, so a window that keeps drawing while it's minimized (Terminal with output arriving) stays live, and one that stops shows its last frame. Servers that do this set `WS_SERVER_MINIMIZE`.

![Two tiles at the bottom-left of the screen: a Terminal showing ls output and Workspace's File Viewer, each with its app's icon in the corner](docs/minimize-tiles.png)

- `WS_MSG_WINDOW_MINIMIZE` (`ws_window_minimize()`): a bare request naming the window. WindowServer orders the window out and shows its tile, placed like app icons: the first free place along the bottom of the screen, left to right, then the row above.
- `WS_MSG_SET_APP_ICON` (`ws_set_app_icon()`): the app icon for the tiles, a premultiplied ARGB8888 buffer of at most `WS_MAX_APP_ICON` (512) pixels square, which the server copies. A connection that joined an app and has no icon of its own uses the app's.
- One click on a tile selects it, a drag moves it, and a double-click activates the app and sends it `WS_EVENT_WINDOW_RESTORE` for the window. The app restores the window by ordering it in, which removes the tile; ordering a minimized window out (the app is hiding) or destroying it removes the tile too. Tiles are the server's own: clicks on them reach no app.
- `wsctl list` shows a minimized window where its tile is in the stack, marked `minimized` (`WS_WINDOW_INFO_MINIMIZED`).

The GNUstep backend uses this when the server has it: `-appOwnsMiniwindow` is NO, so libs-gui draws no miniwindow, `-miniwindow:` sends the minimize request, the backend uploads `[NSApp applicationIconImage]`, and a restore event goes through `-[NSWindow deminiaturize:]`. With an older server, apps draw their miniwindows as before.

Two defaults set the look, and nothing theme-specific is compiled in:

| Key | Default | |
|---|---|---|
| `MinimizeTileSize` | `64` | Tile size in pixels, 32 to 256. 64 lines tiles up with GNUstep app icons. |
| `MinimizeTileImage` | empty | A picture drawn behind the window instead of the plain dark rounded tile, scaled to the tile: an 8-bit binary PAM (`RGB_ALPHA` or `RGB`) or PPM file, so the server needs no image libraries. |
| `MinimizeIntoDock` | `false` | "Minimize into the Dock": while a Dock that can show minimized windows is connected, it shows them and WindowServer draws no tiles (see [The Dock](#the-dock)). |

### The Dock

Apps see only their own windows. The Dock ([apps-dock](https://github.com/nextbsd/apps-dock)) needs every app's, as Darwin's Dock gets them from CoreGraphics' private window list, so it opens a second connection, beside its GNUstep backend's, with `WS_CONNECT_DOCK`. Servers that have this set `WS_SERVER_DOCK`. The details are in "The Dock" in `WSProtocol.h`.

- **Who gets it.** A Dock connection, like a `WS_CONNECT_CONTROL` one (`wsctl`), is a tool, not an app. The hello fails with `EPERM` unless the sender is root, the server's own user or the console user: the user of the loginwindow session, which has a root-owned `/var/run/loginwindow/<uid>.session` record. With no loginwindow session (a server started by hand) and on a headless server, any user may, as for the rest of the protocol. The same check now guards `WS_MSG_LIST_WINDOWS`, which only tool connections may send. The trust boundary is the user: the list holds other apps' window titles, and they all belong to the console user, so other users (an SSH login) can't read them, while the console user's own programs can, as on macOS before 10.15.
- **The list.** `ws_list_windows_flags()` gives every window with its id, owner pid, app id (`app_id`, shared by connections that joined an app), title, level, frame, the flags the owner set (`window_flags`) and its state: visible, minimized, in the Dock, key, active. `WS_LIST_ORDERED_OUT` adds the windows that aren't shown. The new fields are at the end of `ws_event_window_info_t`, so older clients still read the list.
- **Change notices.** `WS_EVENT_WINDOW_LIST_CHANGED` comes when a window is created, destroyed, ordered in or out, restacked, minimized, restored, retitled or gets a new level or flags, when the active app or a key window changes, or when a client goes away; at most one per pass of the event loop, so the Dock lists again instead of polling. A frame change alone sends none.
- **Requests.** `ws_dock_activate()` activates an app by its app id and brings its windows forward, or activates a window's app with that window in front and key (a minimized window is restored instead). `ws_dock_restore()` asks the owner of a minimized window to restore it, with the same `WS_EVENT_WINDOW_RESTORE` a double-click on its tile sends. `ws_dock_preview()` returns a scaled picture of a window (premultiplied ARGB8888 in a sealed memfd, at most `WS_MAX_PREVIEW` pixels on its longer side, with no tile or icon).
- **Minimize into the Dock.** A Dock that can show minimized windows says so with `ws_dock_show_minimized()`. While `MinimizeIntoDock` is on, the oldest such Dock gets `WS_DOCK_MINIMIZED_WINDOWS` in `WS_EVENT_DOCK_STATE`: WindowServer draws no tiles and takes away the ones it drew, minimized windows are listed with `WS_WINDOW_INFO_IN_DOCK`, and every frame such a window commits sends the Dock `WS_EVENT_PREVIEW_CHANGED`. When that Dock goes away, another one takes over, or the tiles come back.

`wsctl dock list|activate|restore|preview|watch` makes the same requests from the command line.

## License

BSD 2-Clause. See [LICENSE](LICENSE).

The `nextbsd` backend in the libs-back fork is LGPL-2.1-or-later, like the rest of libs-back. Protocol headers shared by both halves live in this repository under BSD 2-Clause, so the backend can include them.
