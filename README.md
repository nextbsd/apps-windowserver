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
- Snapshots and synthetic input for testing.

Written but only compiled:

- DRM/KMS output. It picks the card that has a connected display, sets the preferred mode, uses two dumb buffers with page flips (or `drmModeSetCrtc` if flips fail), and the hardware cursor, with a software cursor as fallback.
- VT handling: no VT switching. The server takes over the VT that is on screen when it starts, puts it in `KD_GRAPHICS` and `K_RAW` with a raw termios, and locks switching with `VT_LOCKSWITCH`, so Ctrl+Alt+F*n* and `vidcontrol -s` can't take the screen away. It refuses to start if that VT is already in graphics mode or controlled by another process (Xorg, or another WindowServer). On exit, `SIGTERM`/`SIGINT`/`SIGHUP` or a crash it unlocks switching, gives the same VT back its VT mode, `KD_TEXT`, keyboard mode and termios, and drops DRM master, so the console comes back as it was.
- libinput on seat0 through the IOKit libudev, with xkbcommon (`evdev` rules), relative and absolute pointers, wheel and touchpad scrolling. Power and sleep keys are logged, never typed.
- Dropping from root to `_windowserver` after the devices are open.

Not done yet:

- The GNUstep backend (`libgnustep-nextbsd` in the libs-back fork) and a real `ws-session`.
- Waiting for the GPU kext's IOKit match, and display hotplug through IOKit.
- Images for the standard cursors other than the arrow.
- Touch, tablets, gestures and switches. Per-device keyboard settings.
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

`wstest` prints every event it gets. `tests/headless-test.sh` (`gmake check`) does all of this with its own service name and socket, and checks the screen colours, the order of the activation events, that only the active app gets keys, joining an app, the XKB events, XRGB8888 buffers and restack notices.

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
- All connections of an app are activated and deactivated together (`WS_EVENT_APP_ACTIVATED`/`DEACTIVATED` go to each). They share one key window, which can be any connection's window; clicking a joined connection's window only moves the key window. Key events, Command shortcuts included, go to the connection that owns the key window.
- If the app's first connection goes away, the oldest remaining connection carries the app on.

### XKB state for X servers

Key events carry the keycode, the keysym, text and six coarse `WS_MOD_*` bits, which is all a GNUstep app needs. A client that runs its own XKB, such as an X server, connects with `WS_CONNECT_XKB` (servers that support it set `WS_SERVER_XKB`) and also gets:

- `WS_EVENT_KEYMAP` (`ws_event_keymap_t`), right after the welcome and again if the keymap changes: the compiled keymap (`xkb_keymap_get_as_string()`, text v1, NUL-terminated) in a sealed, read-only memfd passed like a snapshot's, plus the repeat delay and interval in milliseconds. The client maps it with `MAP_PRIVATE` and closes the descriptor.
- `WS_EVENT_MODIFIERS` (`ws_event_modifiers_t`): depressed, latched and locked modifiers and the effective layout group as XKB masks, like Wayland's `wl_keyboard.modifiers`, after any key that changes them.
- `WS_EVENT_KEYS_HELD` (`ws_event_keys_held_t`): the XKB keycodes still held down when the connection starts getting key events (its app is activated, or the key window moves to one of its windows), followed by `WS_EVENT_MODIFIERS`. Their key-ups come to it later, so nothing sticks.

Clients without the flag get none of these.

### Restack notices

When the server itself moves a window in the stack or orders it out (today that is RaiseOnClick), it sends the window's owner `WS_EVENT_WINDOW_ORDERED` (`ws_event_ordered_t`): `WS_ORDER_ABOVE` with the owner's own nearest visible windows below and above it, or `WS_ORDER_OUT`. A client's own ordering, level and destroy requests never cause one. An X server uses it to keep its stacking order right; clients that don't care ignore it. Servers that send it set `WS_SERVER_ORDERED`.

## License

BSD 2-Clause. See [LICENSE](LICENSE).

The `nextbsd` backend in the libs-back fork is LGPL-2.1-or-later, like the rest of libs-back. Protocol headers shared by both halves live in this repository under BSD 2-Clause, so the backend can include them.
