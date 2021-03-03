# xdg-screensaver-shim

A simple replacment for **xdg-screensaver** using [org.freedesktop.ScreenSaver](https://people.freedesktop.org/~hadess/idle-inhibition-spec/re01.html).
Only the **suspend** and **resume** commands are implemented.

Requires **dbus-1** and **x11**.

## Usage

```
xdg-screensaver - command line tool for controlling the screensaver

xdg-screensaver suspend WindowID
xdg-screensaver resume WindowID
xdg-screensaver { --help | --version }
```
