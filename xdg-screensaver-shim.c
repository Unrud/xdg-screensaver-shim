/*
 * Copyright (c) 2021 Unrud <unrud@outlook.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <ctype.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <dbus/dbus.h>
#include <X11/Xlib.h>
#include <X11/Xlibint.h>
#include "project-config.h"

const int EXIT_SIGNALS[] = {SIGHUP, SIGINT, SIGPIPE, SIGQUIT, SIGTERM, 0};
const size_t NULL_BYTE_LEN = 1;

#define cleanReturn(value) do { returnValue = value; goto cleanReturn; } while (false)

bool allocSprintf(char **returnStr, const char *format, ...) {
    va_list ap;
    bool returnValue = true;
    char *str = NULL;
    va_start(ap, format);
    int strLen = vsnprintf(NULL, 0, format, ap);
    va_end(ap);
    if (strLen < 0) {
        fprintf(stderr, "Unexpected vsnprintf error encountered\n");
        cleanReturn(false);
    }
    size_t size = (size_t)strLen + NULL_BYTE_LEN;
    if ((str = malloc(size)) == NULL) {
        fprintf(stderr, "Out of memory\n");
        cleanReturn(false);
    }
    va_start(ap, format);
    int strLen2 = vsnprintf(str, size, format, ap);
    va_end(ap);
    if (strLen != strLen2) {
        fprintf(stderr, "Unexpected vsnprintf error encountered\n");
        cleanReturn(false);
    }
cleanReturn:
    if (!returnValue) {
        free(str);
        str = NULL;
    }
    *returnStr = str;
    return returnValue;
}

struct operationSuspendData_t {
    DBusError dbusErr;
    DBusConnection *dbusConn;
    char *inhibitReason;
    DBusMessage *inhibitMsg, *inhibitReplyMsg, *unInhibitMsg, *unInhibitReplyMsg;
    dbus_uint32_t screenSaverInhibitCookie;
    bool screenSaverInhibited;
    Display *display;
    int signalFd;
} operationSuspendData;

bool operationSuspendFinish() {
    bool returnValue = true;
    struct operationSuspendData_t *d = &operationSuspendData;
    if (d->inhibitReplyMsg != NULL) {
        dbus_message_unref(d->inhibitReplyMsg);
    }
    if (d->inhibitMsg != NULL) {
        dbus_message_unref(d->inhibitMsg);
    }
    free(d->inhibitReason);
    dbus_error_free(&d->dbusErr);
    // Un-inhibit screen saver
    if (!d->screenSaverInhibited) {
        goto unInhibitScreenSaverEnd;
    }
    if ((d->unInhibitMsg = dbus_message_new_method_call(
            "org.freedesktop.ScreenSaver",
            "/org/freedesktop/ScreenSaver",
            "org.freedesktop.ScreenSaver",
            "UnInhibit")) == NULL) {
        fprintf(stderr, "Out of memory\n");
        returnValue = false;
        goto unInhibitScreenSaverEnd;
    }
    DBusMessageIter unInhibitMsgIter;
    dbus_message_iter_init_append(d->unInhibitMsg, &unInhibitMsgIter);
    if (!dbus_message_iter_append_basic(
            &unInhibitMsgIter, DBUS_TYPE_UINT32, &d->screenSaverInhibitCookie)) {
        fprintf(stderr, "Out of memory\n");
        returnValue = false;
        goto unInhibitScreenSaverEnd;
    }
    if ((d->unInhibitReplyMsg = dbus_connection_send_with_reply_and_block(
            d->dbusConn, d->unInhibitMsg, DBUS_TIMEOUT_USE_DEFAULT, &d->dbusErr)) == NULL) {
        if (dbus_error_is_set(&d->dbusErr)) {
            fprintf(stderr, "Failed to call D-Bus method: %s\n", d->dbusErr.message);
        }
        returnValue = false;
        goto unInhibitScreenSaverEnd;
    }
    DBusMessageIter unInhibitReplyMsgIter;
    dbus_message_iter_init(d->unInhibitReplyMsg, &unInhibitReplyMsgIter);
    if (dbus_message_iter_get_arg_type(&unInhibitReplyMsgIter) != DBUS_TYPE_INVALID) {
        fprintf(stderr, "Unexpected D-Bus reply\n");
        returnValue = false;
        goto unInhibitScreenSaverEnd;
    }
unInhibitScreenSaverEnd:
    if (d->unInhibitReplyMsg != NULL) {
        dbus_message_unref(d->unInhibitReplyMsg);
    }
    if (d->unInhibitMsg != NULL) {
        dbus_message_unref(d->unInhibitMsg);
    }
    dbus_error_free(&d->dbusErr);
    if (d->dbusConn != NULL) {
        dbus_connection_unref(d->dbusConn);
    }
    if (d->signalFd != -1) {
        close(d->signalFd);
    }
    if (d->display != NULL) {
        // Unset custom X error handlers because they call this function
        XSetErrorHandler(NULL);
        XSetIOErrorHandler(NULL);
        XCloseDisplay(d->display);
    }
    return returnValue;
}

// X Error Handler that calls operationSuspendFinish before exiting
int operationSuspendXErrorHandler(Display *display, XErrorEvent *ev) {
    operationSuspendData.display = NULL; // don't free display structure
    operationSuspendFinish();
    _XDefaultError(display, ev); // may not return
    exit(EXIT_FAILURE);
}

// X IO Error Handler that calls operationSuspendFinish before exiting
int operationSuspendXIOErrorHandler(Display *display) {
    operationSuspendData.display = NULL; // don't free display structure
    operationSuspendFinish();
    _XDefaultIOError(display); // may not return
    exit(EXIT_FAILURE);
}

bool operationSuspend(const char *prog, Window window) {
    bool returnValue = true;
    struct operationSuspendData_t *d = &operationSuspendData;
    *d = (struct operationSuspendData_t){0};
    dbus_error_init(&d->dbusErr);
    d->signalFd = -1;
    // Set up signal fd
    sigset_t exit_sigset;
    sigemptyset(&exit_sigset);
    for (int i = 0; EXIT_SIGNALS[i] != 0; i++) {
        sigaddset(&exit_sigset, EXIT_SIGNALS[i]);
    }
    if (sigprocmask(SIG_BLOCK, &exit_sigset, NULL) < 0) {
        fprintf(stderr, "Failed to block signals: %s\n", strerror(errno));
        cleanReturn(false);
    }
    if ((d->signalFd = signalfd(-1, &exit_sigset, SFD_CLOEXEC)) < 0) {
        fprintf(stderr, "Failed to create signal fd: %s\n", strerror(errno));
        cleanReturn(false);
    }
    // Init D-Bus
    if ((d->dbusConn = dbus_bus_get(DBUS_BUS_SESSION, &d->dbusErr)) == NULL) {
        if (dbus_error_is_set(&d->dbusErr)) {
            fprintf(stderr, "Failed to connect D-Bus: %s\n", d->dbusErr.message);
        }
        cleanReturn(false);
    }
    // Inhibit screen saver
    if ((d->inhibitMsg = dbus_message_new_method_call(
            "org.freedesktop.ScreenSaver",
            "/org/freedesktop/ScreenSaver",
            "org.freedesktop.ScreenSaver",
            "Inhibit")) == NULL) {
        fprintf(stderr, "Out of memory\n");
        cleanReturn(false);
    }
    DBusMessageIter inhibitMsgIter;
    dbus_message_iter_init_append(d->inhibitMsg, &inhibitMsgIter);
    if (!dbus_message_iter_append_basic(
            &inhibitMsgIter, DBUS_TYPE_STRING, &prog)) {
        fprintf(stderr, "Out of memory\n");
        cleanReturn(false);
    }
    if (!allocSprintf(&d->inhibitReason, "waiting for X window %#lx", window)) {
        cleanReturn(false);
    }
    if (!dbus_message_iter_append_basic(
            &inhibitMsgIter, DBUS_TYPE_STRING, &d->inhibitReason)) {
        fprintf(stderr, "Out of memory\n");
        cleanReturn(false);
    }
    if ((d->inhibitReplyMsg = dbus_connection_send_with_reply_and_block(
            d->dbusConn, d->inhibitMsg, DBUS_TIMEOUT_USE_DEFAULT, &d->dbusErr)) == NULL) {
        if (dbus_error_is_set(&d->dbusErr)) {
            fprintf(stderr, "Failed to call D-Bus method: %s\n", d->dbusErr.message);
        }
        cleanReturn(false);
    }
    DBusMessageIter inhibitReplyMsgIter;
    dbus_message_iter_init(d->inhibitReplyMsg, &inhibitReplyMsgIter);
    if (dbus_message_iter_get_arg_type(&inhibitReplyMsgIter) != DBUS_TYPE_UINT32) {
        fprintf(stderr, "Unexpected D-Bus reply\n");
        cleanReturn(false);
    }
    dbus_message_iter_get_basic(&inhibitReplyMsgIter, &d->screenSaverInhibitCookie);
    d->screenSaverInhibited = true;
    dbus_message_iter_next(&inhibitReplyMsgIter);
    if (dbus_message_iter_get_arg_type(&inhibitReplyMsgIter) != DBUS_TYPE_INVALID) {
        fprintf(stderr, "Unexpected D-Bus reply\n");
        cleanReturn(false);
    }
    // Set custom X error handlers
    XSetErrorHandler(operationSuspendXErrorHandler);
    XSetIOErrorHandler(operationSuspendXIOErrorHandler);
    // Init X
    d->display = XOpenDisplay(NULL);
    if (d->display == NULL) {
        fprintf(stderr, "Failed to open X display\n");
        cleanReturn(false);
    }
    // Monitor X events for destruction of window (BadWindow error if window invalid)
    XSelectInput(d->display, window, StructureNotifyMask);
    // Flush requests and handle errors (esp. BadWindow)
    XSync(d->display, false);
    // Prepare select
    int xServerFd = XConnectionNumber(d->display);
    fd_set activeFdSet, readFdSet;
    FD_ZERO(&activeFdSet);
    FD_SET(d->signalFd, &activeFdSet);
    FD_SET(xServerFd, &activeFdSet);
    // Fork into background
    if (fork() != 0) {
        // Terminate immediately without cleanup
        exit(EXIT_SUCCESS);
    }
    while (true) {
        // Flush X requests and handle pending events (required before select)
        while (XPending(d->display) > 0) {
            XEvent ev;
            XNextEvent(d->display, &ev);
            if (ev.type == DestroyNotify && ev.xdestroywindow.event == window) {
                fprintf(stderr, "Window 0x%lx destroyed\n", window);
                cleanReturn(true);
            }
        }
        readFdSet = activeFdSet;
        if (select(FD_SETSIZE, &readFdSet, NULL, NULL, NULL) < 0) {
            fprintf(stderr, "Failed to block on file descriptors: %s\n", strerror(errno));
            cleanReturn(false);
        }
        if (FD_ISSET(d->signalFd, &readFdSet)) {
            struct signalfd_siginfo siginfo;
            if (read(d->signalFd, &siginfo, sizeof(struct signalfd_siginfo)) < 0) {
                fprintf(stderr, "Failed to read signal fd: %s\n", strerror(errno));
                cleanReturn(false);
            }
            fprintf(stderr, "Received signal %d (%s)\n", siginfo.ssi_signo,
                    strsignal((int)siginfo.ssi_signo));
            cleanReturn(siginfo.ssi_signo == SIGTERM);
        }
    }
cleanReturn:
    if (!operationSuspendFinish()) {
        returnValue = false;
    }
    return returnValue;
}

bool allocReadlink(char **returnLink, const char *path, bool ignoreAccesError) {
    bool returnValue = true;
    char *link = NULL;
    ssize_t linkSize;
    for (size_t size = 256; true; size *= 2) {
        if ((link = realloc(link, size)) == NULL) {
            fprintf(stderr, "Out of memory\n");
            cleanReturn(false);
        }
        if ((linkSize = readlink(path, link, size)) < 0) {
            if (ignoreAccesError && (errno == EACCES || errno == ENOENT)) {
                free(link);
                link = NULL;
                cleanReturn(true);
            }
            fprintf(stderr, "Failed to read link %s: %s\n", path, strerror(errno));
            cleanReturn(false);
        }
        if (linkSize < size) {
            break;
        }
    }
    link[linkSize] = '\0';
    // Cut " (deleted)" from the end of string
    char *suffix = " (deleted)";
    size_t suffixLen = strlen(suffix);
    if (linkSize >= suffixLen && strcmp(&link[(size_t)linkSize-suffixLen], suffix) == 0) {
        linkSize -= (ssize_t)suffixLen;
        link[linkSize] = '\0';
    }
cleanReturn:
    if (!returnValue) {
        free(link);
        link = NULL;
    }
    *returnLink = link;
    return returnValue;
}

bool checkAndResumeProcess(int pid, const char *selfExeLink, Window window) {
    bool returnValue = true;
    char *exePath = NULL, *cmdlinePath = NULL, *exeLink = NULL, *cmdline = NULL;
    int cmdlineFd = -1;
    // Check if process is same exe
    if (!allocSprintf(&exePath, "/proc/%d/exe", pid)) {
        cleanReturn(false);
    }
    if (!allocReadlink(&exeLink, exePath, true)) {
        cleanReturn(false);
    }
    if (exeLink == NULL) {
        cleanReturn(true);
    }
    // Check command line arguments of process
    if (!allocSprintf(&cmdlinePath, "/proc/%d/cmdline", pid)) {
        cleanReturn(false);
    }
    if ((cmdlineFd = open(cmdlinePath, O_RDONLY)) < 0) {
        if (errno == EACCES || errno == ENOENT) {
            cleanReturn(true);
        }
        fprintf(stderr, "Failed to open %s: %s\n", cmdlinePath, strerror(errno));
        cleanReturn(false);
    }
    ssize_t cmdlineSize = 0;
    for (size_t size = 1024; true; size *= 2) {
        if ((cmdline = realloc(cmdline, size)) == NULL) {
            fprintf(stderr, "Out of memory\n");
            cleanReturn(false);
        }
        ssize_t cmdlinePartSize = read(cmdlineFd, &cmdline[cmdlineSize], size-(size_t)cmdlineSize);
        if (cmdlinePartSize < 0) {
            fprintf(stderr, "Failed to read cmdline: %s\n", strerror(errno));
            cleanReturn(false);
        }
        cmdlineSize += cmdlinePartSize;
        if (cmdlineSize < size) {
            break;
        }
    }
    if (cmdlineSize < 1 || cmdline[cmdlineSize-1] != '\0') {
        fprintf(stderr, "Invalid cmdline encountered\n");
        cleanReturn(false);
    }
    // check argc >= 1 and argv[1] is "suspend"
    size_t cmdlineArg1Start = strlen(cmdline) + NULL_BYTE_LEN;
    if (cmdlineArg1Start >= cmdlineSize ||
            strcmp(&cmdline[cmdlineArg1Start], "suspend") != 0) {
        cleanReturn(true);
    }
    // check argc >= 2 and argv[2] is window
    size_t cmdlineArg2Start = (
        cmdlineArg1Start + strlen(&cmdline[cmdlineArg1Start]) + NULL_BYTE_LEN);
    if (cmdlineArg2Start >= cmdlineSize) {
        cleanReturn(true);
    }
    char *windowEnd;
    Window cmdlineWindow = strtoul(&cmdline[cmdlineArg2Start], &windowEnd, 0);
    if (cmdline[cmdlineArg2Start] == '\0' || windowEnd[0] != '\0' || cmdlineWindow != window) {
        cleanReturn(true);
    }
    // Check that argc == 3
    size_t cmdlineArgsEnd = (
        cmdlineArg2Start + strlen(&cmdline[cmdlineArg2Start]) + NULL_BYTE_LEN);
    if (cmdlineArgsEnd != cmdlineSize) {
        cleanReturn(true);
    }
    // Send SIGTERM to process
    if (kill(pid, SIGTERM) < 0) {
        if (errno == EPERM || errno == ESRCH) {
            cleanReturn(true);
        }
        fprintf(stderr, "Failed to kill process %d: %s\n", pid, strerror(errno));
        cleanReturn(false);
    }
cleanReturn:
    if (cmdlineFd != -1) {
        close(cmdlineFd);
    }
    free(exeLink);
    free(cmdline);
    free(exePath);
    free(cmdlinePath);
    return returnValue;
}

bool operationResume(const char *prog, Window window) {
    // Kill all processes that suspend screen saver for window
    bool returnValue = true;
    char *selfExeLink = NULL;
    DIR *procDir = NULL;
    if (!allocReadlink(&selfExeLink, "/proc/self/exe", false)) {
        cleanReturn(false);
    }
    // Search processes in /proc
    if ((procDir = opendir("/proc")) == NULL) {
        fprintf(stderr, "Failed to open /proc: %s\n", strerror(errno));
        cleanReturn(false);
    }
    struct dirent *procDirEnt;
    while ((procDirEnt = readdir(procDir)) != NULL) {
        for (int i = 0; procDirEnt->d_name[i] != '\0'; i++) {
            if (!isdigit(procDirEnt->d_name[i])) {
                continue;
            }
        }
        int pid = atoi(procDirEnt->d_name);
        if (!checkAndResumeProcess(pid, selfExeLink, window)) {
            returnValue = false;
            fprintf(stderr, "Continuing\n");
        }
    }
cleanReturn:
    if (procDir != NULL) {
        closedir(procDir);
    }
    free(selfExeLink);
    return returnValue;
}

void help(const char *prog) {
    printf("%s - command line tool for controlling the screensaver\n\n", prog);
    printf("%s suspend WindowID\n", prog);
    printf("%s resume WindowID\n", prog);
    printf("%s { --help | --version }\n", prog);
}

int main(int argc, char *argv[]) {
    // Parse command line arguments
    Window window;
    if (argc == 3) {
        bool (*op)(const char*, Window);
        if (strcmp(argv[1], "suspend") == 0) {
            op = operationSuspend;
        } else if (strcmp(argv[1], "resume") == 0) {
            op = operationResume;
        } else {
            goto invalidArguments;
        }
        char *windowEnd;
        window = strtoul(argv[2], &windowEnd, 0);
        if (argv[2][0] == '\0' || windowEnd[0] != '\0') {
            fprintf(stderr, "Invalid WindowId: %s\n", argv[2]);
            return EXIT_FAILURE;
        }
        return op(argv[0], window) ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        help(argv[0]);
        return EXIT_SUCCESS;
    }
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("%s %s\n", argv[0], VERSION);
        return EXIT_SUCCESS;
    }
invalidArguments:
    fprintf(stderr, "Invalid command-line arguments (see --help)\n");
    return EXIT_FAILURE;    
}
