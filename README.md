# apps-windowserver

WindowServer for NextBSD: a native display server in the style of NeXTSTEP's and macOS's Window Server.

It owns the screen, input, window ordering and application activation. GNUstep apps draw their own windows and hand the finished pixels to WindowServer, which composites them. There is no X11 and no Wayland in between. XLibre stays available for X11 apps.

This is a proof of concept. It isn't usable yet.

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
| `nextbsd` backend | [pkgdemon/libs-back](https://github.com/pkgdemon/libs-back) | The GNUstep backend each app loads (`libgnustep-nextbsd`). It draws with cairo into shared buffers and talks to WindowServer over Mach |
| Build script | [pkgdemon/nextbsd-gnustep](https://github.com/pkgdemon/nextbsd-gnustep) | `build-windowserver.sh` builds the backend and WindowServer on top of the GNUstep core libraries |

libs-base and libs-gui are unmodified. The `nextbsd` backend installs beside the stock X11 backend, and the `GSBackend` default chooses between them, per app or for the whole session.

## Where it installs

| What | Path |
|---|---|
| Server | `/System/Library/PrivateFrameworks/SkyLight.framework/Resources/WindowServer` |
| launchd job | `/System/Library/LaunchDaemons/org.nextbsd.WindowServer.plist` |
| Account | `_windowserver` (uid 88) |

WindowServer starts as root to open the GPU and input devices, then runs as `_windowserver`.

## Status

Nothing is built yet. The first milestone is a server that drives the display, reads input, and stacks test windows by level. It is started by hand from a text console. GNUstep apps, loginwindow and GPU compositing come after that.

## License

BSD 2-Clause. See [LICENSE](LICENSE).

The `nextbsd` backend in the libs-back fork is LGPL-2.1-or-later, like the rest of libs-back. Protocol headers shared by both halves live in this repository under BSD 2-Clause, so the backend can include them.
