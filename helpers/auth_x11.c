/*
Copyright 2014 Google Inc. All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include <X11/X.h>       // for Success, None, Atom, KBBellPitch
#include <X11/Xlib.h>    // for DefaultScreen, Screen, XFree, True
#include <locale.h>      // for NULL, setlocale, LC_CTYPE, LC_TIME
#include <stdlib.h>      // for free, rand, mblen, size_t, EXIT_...
#include <string.h>      // for strlen, memcpy, memset, strcspn
#include <sys/select.h>  // for timeval, select, fd_set, FD_SET
#include <sys/time.h>    // for gettimeofday, timeval
#include <time.h>        // for time, nanosleep, localtime_r
#include <unistd.h>      // for close, _exit, dup2, pipe, dup

#ifdef HAVE_XFT_EXT
#include <X11/Xft/Xft.h>             // for XftColorAllocValue, XftColorFree
#include <X11/extensions/Xrender.h>  // for XRenderColor, XGlyphInfo
#include <fontconfig/fontconfig.h>   // for FcChar8
#endif

#ifdef HAVE_XKB_EXT
#include <X11/XKBlib.h>             // for XkbFreeKeyboard, XkbGetControls
#include <X11/extensions/XKB.h>     // for XkbUseCoreKbd, XkbGroupsWrapMask
#include <X11/extensions/XKBstr.h>  // for _XkbDesc, XkbStateRec, _XkbControls
#endif

#include "../env_info.h"          // for GetHostName, GetUserName
#include "../env_settings.h"      // for GetIntSetting, GetStringSetting
#include "../logging.h"           // for Log, LogErrno
#include "../mlock_page.h"        // for MLOCK_PAGE
#include "../util.h"              // for explicit_bzero
#include "../wait_pgrp.h"         // for WaitPgrp
#include "../wm_properties.h"     // for SetWMProperties
#include "../xscreensaver_api.h"  // for ReadWindowID
#include "authproto.h"            // for WritePacket, ReadPacket, PTYPE_R...
#include "monitors.h"             // for Monitor, GetMonitors, IsMonitorC...

//! Number of args.
int argc;

//! Args.
char *const *argv;

//! The authproto helper to use.
const char *authproto_executable;

//! The blinking interval in microseconds.
#define BLINK_INTERVAL (250 * 1000)

//! The maximum time to wait at a prompt for user input in seconds.
int prompt_timeout;

//! Number of dancers in the disco password display
#define DISCO_PASSWORD_DANCERS 5

//! Length of the "paranoid password display".
#define PARANOID_PASSWORD_LENGTH (1 << DISCO_PASSWORD_DANCERS)

//! Minimum distance the cursor shall move on keypress.
#define PARANOID_PASSWORD_MIN_CHANGE 5

//! Border of the window around the text.
#define WINDOW_BORDER 16

//! Draw border rectangle (mainly for debugging).
#undef DRAW_BORDER

//! Extra line spacing.
#define LINE_SPACING 4

//! Whether password display should hide the length.
int paranoid_password;

//! Whether the password should be completely hidden
int hide_password_completely;

//! Disco mode: instead of showing the password length or line input, show a disco ball
int disco;

// A disco password is composed of multiple disco_dancers (each selected at random from the array), joined by the disco_combiner
const char * disco_combiner = " ♪ ";
// Note: the disco_dancers MUST all have the same length
const char * disco_dancers[] = {
  "┏(･o･)┛",
  "┗(･o･)┓",
};

//! If set, we can start a new login session.
int have_switch_user_command;

//! If set, the prompt will be fixed by <username>@.
int show_username;

//! If set, the prompt will be fixed by <hostname>. If >1, the hostname will be
// shown in full and not cut at the first dot.
int show_hostname;

//! If set, data and time will be shown.
int show_datetime;

//! The date format to display.
const char *datetime_format = "%c";

//! The local hostname.
char hostname[256];

//! The username to authenticate as.
char username[256];

//! The X11 display.
Display *display;

//! The X11 window provided by main. Provided from $XSCREENSAVER_WINDOW.
Window main_window;

//! main_window's parent. Used to create per-monitor siblings.
Window parent_window;

//! The X11 core font for the PAM messages.
XFontStruct *core_font;

#ifdef HAVE_XFT_EXT
//! The Xft font for the PAM messages.
XftColor xft_color_foreground;
XftColor xft_color_warning;
XftFont *xft_font;
#endif

//! The background color.
XColor xcolor_background;

//! The foreground color.
XColor xcolor_foreground;

//! The warning color (used as foreground).
XColor xcolor_warning;

//! The cursor character displayed at the end of the masked password input.
static const char cursor[] = "_";

//! The x offset to apply to the entire display (to mitigate burn-in).
static int x_offset = 0;

//! The y offset to apply to the entire display (to mitigate burn-in).
static int y_offset = 0;

//! Maximum offset value when dynamic changes are enabled.
static int burnin_mitigation_max_offset = 0;

//! How much the offsets are allowed to change dynamically, and if so, how high.
static int burnin_mitigation_max_offset_change = 0;

//! Whether to play sounds during authentication.
static int auth_sounds = 0;

//! Whether we only want a single auth window.
static int single_auth_window = 0;

//! If set, we need to re-query monitor data and adjust windows.
int per_monitor_windows_dirty = 1;

#define MAIN_WINDOW 0
#define MAX_WINDOWS 16

//! The number of active X11 per-monitor windows.
size_t num_windows = 0;

//! The X11 per-monitor windows to draw on.
Window windows[MAX_WINDOWS];

//! The X11 graphics contexts to draw with.
GC gcs[MAX_WINDOWS];

//! The X11 graphics contexts to draw warnings with.
GC gcs_warning[MAX_WINDOWS];

#ifdef HAVE_XFT_EXT
//! The Xft draw contexts to draw with.
XftDraw *xft_draws[MAX_WINDOWS];
#endif

int have_xkb_ext;

enum Sound { SOUND_PROMPT, SOUND_INFO, SOUND_ERROR, SOUND_SUCCESS };

#define NOTE_DS3 156
#define NOTE_A3 220
#define NOTE_DS4 311
#define NOTE_E4 330
#define NOTE_B4 494
#define NOTE_E5 659
int sounds[][2] = {
    /* SOUND_PROMPT=  */ {NOTE_B4, NOTE_E5},   // V|I I
    /* SOUND_INFO=    */ {NOTE_E5, NOTE_E5},   // I 2x
    /* SOUND_ERROR=   */ {NOTE_A3, NOTE_DS3},  // V7 2x
    /* SOUND_SUCCESS= */ {NOTE_DS4, NOTE_E4},  // V I
};
#define SOUND_SLEEP_MS 125
#define SOUND_TONE_MS 100

/*! \brief Play a sound sequence.
 */
void PlaySound(enum Sound snd) {
  XKeyboardState state;
  XKeyboardControl control;
  struct timespec sleeptime;

  if (!auth_sounds) {
    return;
  }

  XGetKeyboardControl(display, &state);

  // bell_percent changes note length on Linux, so let's use the middle value
  // to get a 1:1 mapping.
  control.bell_percent = 50;
  control.bell_duration = SOUND_TONE_MS;
  control.bell_pitch = sounds[snd][0];
  XChangeKeyboardControl(display, KBBellPercent | KBBellDuration | KBBellPitch,
                         &control);
  XBell(display, 0);

  XFlush(display);

  sleeptime.tv_sec = SOUND_SLEEP_MS / 1000;
  sleeptime.tv_nsec = 1000000L * (SOUND_SLEEP_MS % 1000);
  nanosleep(&sleeptime, NULL);

  control.bell_pitch = sounds[snd][1];
  XChangeKeyboardControl(display, KBBellPitch, &control);
  XBell(display, 0);

  control.bell_percent = state.bell_percent;
  control.bell_duration = state.bell_duration;
  control.bell_pitch = state.bell_pitch;
  XChangeKeyboardControl(display, KBBellPercent | KBBellDuration | KBBellPitch,
                         &control);

  XFlush(display);

  nanosleep(&sleeptime, NULL);
}

/*! \brief Switch to the next keyboard layout.
 */
void SwitchKeyboardLayout(void) {
#ifdef HAVE_XKB_EXT
  if (!have_xkb_ext) {
    return;
  }

  XkbDescPtr xkb;
  xkb = XkbGetMap(display, 0, XkbUseCoreKbd);
  if (XkbGetControls(display, XkbGroupsWrapMask, xkb) != Success) {
    Log("XkbGetControls failed");
    XkbFreeKeyboard(xkb, 0, True);
    return;
  }
  if (xkb->ctrls->num_groups < 1) {
    Log("XkbGetControls returned less than 1 group");
    XkbFreeKeyboard(xkb, 0, True);
    return;
  }
  XkbStateRec state;
  if (XkbGetState(display, XkbUseCoreKbd, &state) != Success) {
    Log("XkbGetState failed");
    XkbFreeKeyboard(xkb, 0, True);
    return;
  }

  XkbLockGroup(display, XkbUseCoreKbd,
               (state.group + 1) % xkb->ctrls->num_groups);

  XkbFreeKeyboard(xkb, 0, True);
#endif
}

/*! \brief Check which modifiers are active.
 *
 * \param warning Will be set to 1 if something's "bad" with the keyboard
 *     layout (e.g. Caps Lock).
 * \param have_multiple_layouts Will be set to 1 if more than one keyboard
 *     layout is available for switching.
 *
 * \return The current modifier mask as a string.
 */
const char *GetIndicators(int *warning, int *have_multiple_layouts) {
#ifdef HAVE_XKB_EXT
  static char buf[128];
  char *p;

  if (!have_xkb_ext) {
    return "";
  }

  XkbDescPtr xkb;
  xkb = XkbGetMap(display, 0, XkbUseCoreKbd);
  if (XkbGetControls(display, XkbGroupsWrapMask, xkb) != Success) {
    Log("XkbGetControls failed");
    XkbFreeKeyboard(xkb, 0, True);
    return "";
  }
  if (XkbGetNames(
          display,
          XkbIndicatorNamesMask | XkbGroupNamesMask | XkbSymbolsNameMask,
          xkb) != Success) {
    Log("XkbGetNames failed");
    XkbFreeKeyboard(xkb, 0, True);
    return "";
  }
  XkbStateRec state;
  if (XkbGetState(display, XkbUseCoreKbd, &state) != Success) {
    Log("XkbGetState failed");
    XkbFreeKeyboard(xkb, 0, True);
    return "";
  }
  unsigned int istate;
  if (XkbGetIndicatorState(display, XkbUseCoreKbd, &istate) != Success) {
    Log("XkbGetIndicatorState failed");
    XkbFreeKeyboard(xkb, 0, True);
    return "";
  }

  // Detect Caps Lock.
  // Note: in very pathological cases the modifier might be set without an
  // XkbIndicator for it; then we show the line in red without telling the user
  // why. Such a situation has not been observd yet though.
  if (state.mods & LockMask) {
    *warning = 1;
  }

  // Provide info about multiple layouts.
  if (xkb->ctrls->num_groups > 1) {
    *have_multiple_layouts = 1;
  }

  p = buf;

  const char *word = "Keyboard: ";
  size_t n = strlen(word);
  if (n >= sizeof(buf) - (p - buf)) {
    Log("Not enough space to store intro '%s'", word);
    XkbFreeKeyboard(xkb, 0, True);
    return "";
  }
  memcpy(p, word, n);
  p += n;

  int have_output = 0;
  Atom layouta = xkb->names->groups[state.group];  // Human-readable.
  if (layouta == None) {
    layouta = xkb->names->symbols;  // Machine-readable fallback.
  }
  if (layouta != None) {
    char *layout = XGetAtomName(display, layouta);
    n = strlen(layout);
    if (n >= sizeof(buf) - (p - buf)) {
      Log("Not enough space to store layout name '%s'", layout);
      XFree(layout);
      XkbFreeKeyboard(xkb, 0, True);
      return "";
    }
    memcpy(p, layout, n);
    XFree(layout);
    p += n;
    have_output = 1;
  }

  int i;
  for (i = 0; i < XkbNumIndicators; i++) {
    if (!(istate & (1U << i))) {
      continue;
    }
    Atom namea = xkb->names->indicators[i];
    if (namea == None) {
      continue;
    }
    if (have_output) {
      if (2 >= sizeof(buf) - (p - buf)) {
        Log("Not enough space to store another modifier name");
        break;
      }
      memcpy(p, ", ", 2);
      p += 2;
    }
    char *name = XGetAtomName(display, namea);
    size_t n = strlen(name);
    if (n >= sizeof(buf) - (p - buf)) {
      Log("Not enough space to store modifier name '%s'", name);
      XFree(name);
      break;
    }
    memcpy(p, name, n);
    XFree(name);
    p += n;
    have_output = 1;
  }
  *p = 0;
  XkbFreeKeyboard(xkb, 0, True);
  return have_output ? buf : "";
#else
  *warning = *warning;                              // Shut up clang-analyzer.
  *have_multiple_layouts = *have_multiple_layouts;  // Shut up clang-analyzer.
  return "";
#endif
}

void DestroyPerMonitorWindows(size_t keep_windows) {
  size_t i;
  for (i = keep_windows; i < num_windows; ++i) {
#ifdef HAVE_XFT_EXT
    XftDrawDestroy(xft_draws[i]);
#endif
    XFreeGC(display, gcs_warning[i]);
    XFreeGC(display, gcs[i]);
    if (i == MAIN_WINDOW) {
      XUnmapWindow(display, windows[i]);
    } else {
      XDestroyWindow(display, windows[i]);
    }
  }
  if (num_windows > keep_windows) {
    num_windows = keep_windows;
  }
}

void CreateOrUpdatePerMonitorWindow(size_t i, const Monitor *monitor,
                                    int region_w, int region_h, int x_offset,
                                    int y_offset) {
  // Desired box.
  int w = region_w;
  int h = region_h;
  int x = monitor->x + (monitor->width - w) / 2 + x_offset;
  int y = monitor->y + (monitor->height - h) / 2 + y_offset;
  // Clip to monitor.
  if (x < 0) {
    w += x;
    x = 0;
  }
  if (y < 0) {
    h += y;
    y = 0;
  }
  if (x + w > monitor->x + monitor->width) {
    w = monitor->x + monitor->width - x;
  }
  if (y + h > monitor->y + monitor->height) {
    h = monitor->y + monitor->height - y;
  }

  if (i < num_windows) {
    // Move the existing window.
    XMoveResizeWindow(display, windows[i], x, y, w, h);
    return;
  }

  if (i > num_windows) {
    // Need to add windows in ]num_windows..i[ first.
    Log("Unreachable code - can't create monitor sequences with holes");
    abort();
  }

  // Add a new window.
  XSetWindowAttributes attrs = {0};
  attrs.background_pixel = xcolor_background.pixel;
  if (i == MAIN_WINDOW) {
    // Reuse the main_window (so this window gets protected from overlap by
    // main).
    XMoveResizeWindow(display, main_window, x, y, w, h);
    XChangeWindowAttributes(display, main_window, CWBackPixel, &attrs);
    windows[i] = main_window;
  } else {
    // Create a new window.
    windows[i] =
        XCreateWindow(display, parent_window, x, y, w, h, 0, CopyFromParent,
                      InputOutput, CopyFromParent, CWBackPixel, &attrs);
    SetWMProperties(display, windows[i], "xsecurelock", "auth_x11_screen", argc,
                    argv);
    // We should always make sure that main_window stays on top of all others.
    // I.e. our auth sub-windows shall between "sandwiched" between auth and
    // saver window. That way, main.c's protections of the auth window can stay
    // effective.
    Window stacking_order[2];
    stacking_order[0] = main_window;
    stacking_order[1] = windows[i];
    XRestackWindows(display, stacking_order, 2);
  }

  // Create its data structures.
  XGCValues gcattrs;
  gcattrs.function = GXcopy;
  gcattrs.foreground = xcolor_foreground.pixel;
  gcattrs.background = xcolor_background.pixel;
  if (core_font != NULL) {
    gcattrs.font = core_font->fid;
  }
  gcs[i] = XCreateGC(display, windows[i],
                     GCFunction | GCForeground | GCBackground |
                         (core_font != NULL ? GCFont : 0),
                     &gcattrs);
  gcattrs.foreground = xcolor_warning.pixel;
  gcs_warning[i] = XCreateGC(display, windows[i],
                             GCFunction | GCForeground | GCBackground |
                                 (core_font != NULL ? GCFont : 0),
                             &gcattrs);
#ifdef HAVE_XFT_EXT
  xft_draws[i] = XftDrawCreate(
      display, windows[i], DefaultVisual(display, DefaultScreen(display)),
      DefaultColormap(display, DefaultScreen(display)));
#endif

  // This window is now ready to use.
  XMapWindow(display, windows[i]);
  num_windows = i + 1;
}

void UpdatePerMonitorWindows(int monitors_changed, int region_w, int region_h,
                             int x_offset, int y_offset) {
  static size_t num_monitors = 0;
  static Monitor monitors[MAX_WINDOWS];

  if (monitors_changed) {
    num_monitors = GetMonitors(display, parent_window, monitors, MAX_WINDOWS);
  }

  if (single_auth_window) {
    Window unused_root, unused_child;
    int unused_root_x, unused_root_y, x, y;
    unsigned int unused_mask;
    XQueryPointer(display, parent_window, &unused_root, &unused_child,
                  &unused_root_x, &unused_root_y, &x, &y, &unused_mask);
    size_t i;
    for (i = 0; i < num_monitors; ++i) {
      if (x >= monitors[i].x && x < monitors[i].x + monitors[i].width &&
          y >= monitors[i].y && y < monitors[i].y + monitors[i].height) {
        CreateOrUpdatePerMonitorWindow(0, &monitors[i], region_w, region_h,
                                       x_offset, y_offset);
        return;
      }
    }
    if (num_monitors > 0) {
      CreateOrUpdatePerMonitorWindow(0, &monitors[0], region_w, region_h,
                                     x_offset, y_offset);
      DestroyPerMonitorWindows(1);
    } else {
      DestroyPerMonitorWindows(0);
    }
    return;
  }

  // 1 window per monitor.
  size_t new_num_windows = num_monitors;

  // Update or create everything.
  size_t i;
  for (i = 0; i < new_num_windows; ++i) {
    CreateOrUpdatePerMonitorWindow(i, &monitors[i], region_w, region_h,
                                   x_offset, y_offset);
  }

  // Kill all the old stuff.
  DestroyPerMonitorWindows(new_num_windows);

  if (num_windows != new_num_windows) {
    Log("Unreachable code - expected to get %d windows, got %d",
        (int)new_num_windows, (int)num_windows);
  }
}

int TextAscent(void) {
#ifdef HAVE_XFT_EXT
  if (xft_font != NULL) {
    return xft_font->ascent;
  }
#endif
  return core_font->max_bounds.ascent;
}

int TextDescent(void) {
#ifdef HAVE_XFT_EXT
  if (xft_font != NULL) {
    return xft_font->descent;
  }
#endif
  return core_font->max_bounds.descent;
}

#ifdef HAVE_XFT_EXT
// Returns the amount of pixels to expand the logical box in extents so it
// covers the visible box.
int XGlyphInfoExpandAmount(XGlyphInfo *extents) {
  // Use whichever is larger - visible bounding box (bigger if font is italic)
  // or spacing to next character (bigger if last character is a space).
  // Best reference I could find:
  //   https://keithp.com/~keithp/render/Xft.tutorial
  // Visible bounding box: [-x, -x + width[
  // Logical bounding box: [0, xOff[
  // For centering we should always use the logical bounding box, however for
  // erasing we should use the visible bounding box. Thus our goal is to
  // expand the _logical_ box to fully cover the _visible_ box:
  int expand_left = extents->x;
  int expand_right = -extents->x + extents->width - extents->xOff;
  int expand_max = expand_left > expand_right ? expand_left : expand_right;
  int expand_positive = expand_max > 0 ? expand_max : 0;
  return expand_positive;
}
#endif

int TextWidth(const char *string, int len) {
#ifdef HAVE_XFT_EXT
  if (xft_font != NULL) {
    XGlyphInfo extents;
    XftTextExtentsUtf8(display, xft_font, (const FcChar8 *)string, len,
                       &extents);
    return extents.xOff + 2 * XGlyphInfoExpandAmount(&extents);
  }
#endif
  return XTextWidth(core_font, string, len);
}

void DrawString(int monitor, int x, int y, int is_warning, const char *string,
                int len) {
#ifdef HAVE_XFT_EXT
  if (xft_font != NULL) {
    // HACK: Query text extents here to make the text fit into the specified
    // box. For y this is covered by the usual ascent/descent behavior - for x
    // we however do have to work around font descents being drawn to the left
    // of the cursor.
    XGlyphInfo extents;
    XftTextExtentsUtf8(display, xft_font, (const FcChar8 *)string, len,
                       &extents);
    XftDrawStringUtf8(xft_draws[monitor],
                      is_warning ? &xft_color_warning : &xft_color_foreground,
                      xft_font, x + XGlyphInfoExpandAmount(&extents), y,
                      (const FcChar8 *)string, len);
    return;
  }
#endif
  XDrawString(display, windows[monitor],
              is_warning ? gcs_warning[monitor] : gcs[monitor], x, y, string,
              len);
}

void StrAppend(char **output, size_t *output_size, const char *input,
               size_t input_size) {
  if (*output_size <= input_size) {
    // Cut the input off. Sorry.
    input_size = *output_size - 1;
  }
  memcpy(*output, input, input_size);
  *output += input_size;
  *output_size -= input_size;
}

void BuildTitle(char *output, size_t output_size, const char *input) {
  if (show_username) {
    size_t username_len = strlen(username);
    StrAppend(&output, &output_size, username, username_len);
  }

  if (show_username && show_hostname) {
    StrAppend(&output, &output_size, "@", 1);
  }

  if (show_hostname) {
    size_t hostname_len =
        show_hostname > 1 ? strlen(hostname) : strcspn(hostname, ".");
    StrAppend(&output, &output_size, hostname, hostname_len);
  }

  if (*input == 0) {
    *output = 0;
    return;
  }

  if (show_username || show_hostname) {
    StrAppend(&output, &output_size, " - ", 3);
  }
  strncpy(output, input, output_size - 1);
  output[output_size - 1] = 0;
}

/*! \brief Display a string in the window.
 *
 * The given title and message will be displayed on all screens. In case caps
 * lock is enabled, the string's case will be inverted.
 *
 * \param title The title of the message.
 * \param str The message itself.
 * \param is_warning Whether to use the warning style to display the message.
 */
void DisplayMessage(const char *title, const char *str, int is_warning) {
  char full_title[256];
  BuildTitle(full_title, sizeof(full_title), title);

  int th = TextAscent() + TextDescent() + LINE_SPACING;
  int to = TextAscent() + LINE_SPACING / 2;  // Text at to fits into 0 to th.

  int len_full_title = strlen(full_title);
  int tw_full_title = TextWidth(full_title, len_full_title);

  int len_str = strlen(str);
  int tw_str = TextWidth(str, len_str);

  int indicators_warning = 0;
  int have_multiple_layouts = 0;
  const char *indicators =
      GetIndicators(&indicators_warning, &have_multiple_layouts);
  int len_indicators = strlen(indicators);
  int tw_indicators = TextWidth(indicators, len_indicators);

  const char *switch_layout =
      have_multiple_layouts ? "Press Ctrl-Tab to switch keyboard layout" : "";
  int len_switch_layout = strlen(switch_layout);
  int tw_switch_layout = TextWidth(switch_layout, len_switch_layout);

  const char *switch_user = have_switch_user_command
                                ? "Press Ctrl-Alt-O or Win-O to switch user"
                                : "";
  int len_switch_user = strlen(switch_user);
  int tw_switch_user = TextWidth(switch_user, len_switch_user);

  char datetime[80] = "";
  if (show_datetime) {
    time_t rawtime;
    struct tm timeinfo_buf;
    struct tm *timeinfo;

    time(&rawtime);
    timeinfo = localtime_r(&rawtime, &timeinfo_buf);
    if (timeinfo == NULL ||
        strftime(datetime, sizeof(datetime), datetime_format, timeinfo) == 0) {
      // The datetime buffer was too small to fit the time format, and in this
      // case the buffer contents are undefined. Let's just make it a valid
      // empty string then so all else will go well.
      datetime[0] = 0;
    }
  }

  int len_datetime = strlen(datetime);
  int tw_datetime = TextWidth(datetime, len_datetime);

  // Compute the region we will be using, relative to cx and cy.
  int box_w = tw_full_title;
  if (box_w < tw_datetime) {
    box_w = tw_datetime;
  }
  if (box_w < tw_str) {
    box_w = tw_str;
  }
  if (box_w < tw_indicators) {
    box_w = tw_indicators;
  }
  if (box_w < tw_switch_layout) {
    box_w = tw_switch_layout;
  }
  if (box_w < tw_switch_user) {
    box_w = tw_switch_user;
  }
  int box_h = (4 + have_multiple_layouts + have_switch_user_command +
               show_datetime * 2) *
              th;
  int region_w = box_w + 2 * WINDOW_BORDER;
  int region_h = box_h + 2 * WINDOW_BORDER;

  if (burnin_mitigation_max_offset_change > 0) {
    x_offset += rand() % (2 * burnin_mitigation_max_offset_change + 1) -
                burnin_mitigation_max_offset_change;
    if (x_offset < -burnin_mitigation_max_offset) {
      x_offset = -burnin_mitigation_max_offset;
    }
    if (x_offset > burnin_mitigation_max_offset) {
      x_offset = burnin_mitigation_max_offset;
    }
    y_offset += rand() % (2 * burnin_mitigation_max_offset_change + 1) -
                burnin_mitigation_max_offset_change;
    if (y_offset < -burnin_mitigation_max_offset) {
      y_offset = -burnin_mitigation_max_offset;
    }
    if (y_offset > burnin_mitigation_max_offset) {
      y_offset = burnin_mitigation_max_offset;
    }
  }

  UpdatePerMonitorWindows(per_monitor_windows_dirty, region_w, region_h,
                          x_offset, y_offset);
  per_monitor_windows_dirty = 0;

  size_t i;
  for (i = 0; i < num_windows; ++i) {
    int cx = region_w / 2;
    int cy = region_h / 2;
    int y = cy + to - box_h / 2;

    XClearWindow(display, windows[i]);

#ifdef DRAW_BORDER
    XDrawRectangle(display, windows[i], gcs[i],     //
                   cx - box_w / 2, cy - box_h / 2,  //
                   box_w - 1, box_h - 1);
#endif

    if (show_datetime) {
      DrawString(i, cx - tw_datetime / 2, y, 0, datetime, len_datetime);
      y += th * 2;
    }

    DrawString(i, cx - tw_full_title / 2, y, is_warning, full_title,
               len_full_title);
    y += th * 2;

    DrawString(i, cx - tw_str / 2, y, is_warning, str, len_str);
    y += th;

    DrawString(i, cx - tw_indicators / 2, y, indicators_warning, indicators,
               len_indicators);
    y += th;

    if (have_multiple_layouts) {
      DrawString(i, cx - tw_switch_layout / 2, y, 0, switch_layout,
                 len_switch_layout);
      y += th;
    }

    if (have_switch_user_command) {
      DrawString(i, cx - tw_switch_user / 2, y, 0, switch_user,
                 len_switch_user);
      // y += th;
    }
  }

  // Make the things just drawn appear on the screen as soon as possible.
  XFlush(display);
}

void WaitForKeypress(int seconds) {
  // Sleep for up to 1 second _or_ a key press.
  struct timeval timeout;
  timeout.tv_sec = seconds;
  timeout.tv_usec = 0;
  fd_set set;
  memset(&set, 0, sizeof(set));  // For clang-analyzer.
  FD_ZERO(&set);
  FD_SET(0, &set);
  select(1, &set, NULL, NULL, &timeout);
}

/*! \brief Bump the position for the password "cursor".
 *
 * Precondition: pos in 0..PARANOID_PASSWORD_LENGTH-1.
 * Postcondition: pos' in 1..PARANOID_PASSWORD_LENGTH-1.
 * Postcondition: abs(pos' - pos) >= PARANOID_PASSWORD_MIN_CHANGE.
 * Postcondition: pos' is uniformly distributed among all permitted choices.
 *
 * \param pos The initial cursor position.
 * \return pos', the new position.
 */
int BumpDisplayMarker(int pos) {
  for (;;) {
    int new_pos = 1 + rand() % (PARANOID_PASSWORD_LENGTH - 1);
    if (abs(new_pos - pos) >= PARANOID_PASSWORD_MIN_CHANGE) {
      return new_pos;
    }
  }
}

//! The size of the buffer to store the password in. Not NUL terminated.
#define PWBUF_SIZE 256

//! The size of the buffer to use for display, with space for cursor and NUL.
#define DISPLAYBUF_SIZE (PWBUF_SIZE + 2)

/*! \brief Ask a question to the user.
 *
 * \param msg The message.
 * \param response The response will be stored in a newly allocated buffer here.
 *   The caller is supposed to eventually free() it.
 * \param echo If true, the input will be shown; otherwise it will be hidden
 *   (password entry).
 * \return 1 if successful, anything else otherwise.
 */
int Prompt(const char *msg, char **response, int echo) {
  // Ask something. Return strdup'd string.
  struct {
    // The received X11 event.
    XEvent ev;

    // Input buffer. Not NUL-terminated.
    char pwbuf[PWBUF_SIZE];
    // Current input length.
    size_t pwlen;

    // Display buffer. If echo is 0, this will only contain asterisks, a
    // possible cursor, and be NUL-terminated.
    char displaybuf[DISPLAYBUF_SIZE];
    // Display buffer length.
    size_t displaylen;

    // The display marker changes on every input action to a value from 0 to
    // PARANOID_PASSWORD-1. It indicates where to display the "cursor".
    size_t displaymarker;

    // Character read buffer.
    char inputbuf;

    // Temporary position variables that might leak properties about the
    // password and thus are in the private struct too.
    size_t prevpos;
    size_t pos;
    int len;
  } priv;
  int blink_state = 0;

  if (!echo && MLOCK_PAGE(&priv, sizeof(priv)) < 0) {
    LogErrno("mlock");
    // We continue anyway, as the user being unable to unlock the screen is
    // worse. But let's alert the user.
    DisplayMessage("Error", "Password will not be stored securely.", 1);
    WaitForKeypress(1);
  }

  priv.pwlen = 0;
  priv.displaymarker = 0;

  time_t deadline = time(NULL) + prompt_timeout;

  // Unfortunately we may have to break out of multiple loops at once here but
  // still do common cleanup work. So we have to track the return value in a
  // variable.
  int status = 0;
  int done = 0;
  int played_sound = 0;

  while (!done) {
    if (echo) {
      if (priv.pwlen != 0) {
        memcpy(priv.displaybuf, priv.pwbuf, priv.pwlen);
      }
      priv.displaylen = priv.pwlen;
      // Note that priv.pwlen <= sizeof(priv.pwbuf) and thus
      // priv.pwlen + 2 <= sizeof(priv.displaybuf).
      priv.displaybuf[priv.displaylen] = blink_state ? ' ' : *cursor;
      priv.displaybuf[priv.displaylen + 1] = '\0';
    } else if (hide_password_completely) {
      priv.displaylen = 0;
      priv.displaybuf[0] = '\0';
    } else if (disco) {
      size_t combiner_length = strlen(disco_combiner);
      size_t dancer_length = strlen(disco_dancers[0]);
      size_t stride = combiner_length + dancer_length;
      priv.displaylen = stride * DISCO_PASSWORD_DANCERS * strlen(disco_dancers[0]) + strlen(disco_combiner);

      for (size_t i = 0, bit = 1; i < DISCO_PASSWORD_DANCERS; i++, bit <<= 1) {
        const char *dancer = disco_dancers[priv.displaymarker & bit ? 1 : 0];
        memcpy(priv.displaybuf + i * stride, disco_combiner, combiner_length);
        memcpy(priv.displaybuf + i * stride + combiner_length, dancer, dancer_length);
      }
      memcpy(priv.displaybuf + DISCO_PASSWORD_DANCERS * stride, disco_combiner, combiner_length);
      priv.displaybuf[priv.displaylen] = '\0';
    } else if (paranoid_password) {
      priv.displaylen = PARANOID_PASSWORD_LENGTH;
      memset(priv.displaybuf, '_', priv.displaylen);
      priv.displaybuf[priv.displaymarker] = blink_state ? '|' : '-';
      priv.displaybuf[priv.displaylen] = '\0';
    } else {
      mblen(NULL, 0);
      priv.pos = priv.displaylen = 0;
      while (priv.pos < priv.pwlen) {
        ++priv.displaylen;
        // Note: this won't read past priv.pwlen.
        priv.len = mblen(priv.pwbuf + priv.pos, priv.pwlen - priv.pos);
        if (priv.len <= 0) {
          // This guarantees to "eat" one byte each step. Therefore,
          // priv.displaylen <= priv.pwlen is ensured.
          break;
        }
        priv.pos += priv.len;
      }
      memset(priv.displaybuf, '*', priv.displaylen);
      // Note that priv.pwlen <= sizeof(priv.pwbuf) and thus
      // priv.pwlen + 2 <= sizeof(priv.displaybuf).
      priv.displaybuf[priv.displaylen] = blink_state ? ' ' : *cursor;
      priv.displaybuf[priv.displaylen + 1] = '\0';
    }
    DisplayMessage(msg, priv.displaybuf, 0);

    if (!played_sound) {
      PlaySound(SOUND_PROMPT);
      played_sound = 1;
    }

    // Blink the cursor.
    blink_state = !blink_state;

    struct timeval timeout;
    timeout.tv_sec = BLINK_INTERVAL / 1000000;
    timeout.tv_usec = BLINK_INTERVAL % 1000000;

    while (!done) {
      fd_set set;
      memset(&set, 0, sizeof(set));  // For clang-analyzer.
      FD_ZERO(&set);
      FD_SET(0, &set);
      int nfds = select(1, &set, NULL, NULL, &timeout);
      if (nfds < 0) {
        LogErrno("select");
        done = 1;
        break;
      }
      time_t now = time(NULL);
      if (now > deadline) {
        Log("AUTH_TIMEOUT hit");
        done = 1;
        break;
      }
      if (deadline > now + prompt_timeout) {
        // Guard against the system clock stepping back.
        deadline = now + prompt_timeout;
      }
      if (nfds == 0) {
        // Blink...
        break;
      }

      // From now on, only do nonblocking selects so we update the screen ASAP.
      timeout.tv_usec = 0;

      // Force the cursor to be in visible state while typing.
      blink_state = 0;

      // Reset the prompt timeout.
      deadline = now + prompt_timeout;

      ssize_t nread = read(0, &priv.inputbuf, 1);
      if (nread <= 0) {
        Log("EOF on password input - bailing out");
        done = 1;
        break;
      }
      switch (priv.inputbuf) {
        case '\b':      // Backspace.
        case '\177': {  // Delete (note: i3lock does not handle this one).
          // Backwards skip with multibyte support.
          mblen(NULL, 0);
          priv.pos = priv.prevpos = 0;
          while (priv.pos < priv.pwlen) {
            priv.prevpos = priv.pos;
            // Note: this won't read past priv.pwlen.
            priv.len = mblen(priv.pwbuf + priv.pos, priv.pwlen - priv.pos);
            if (priv.len <= 0) {
              // This guarantees to "eat" one byte each step. Therefore,
              // this cannot loop endlessly.
              break;
            }
            priv.pos += priv.len;
          }
          if (priv.prevpos != priv.pwlen) {
            if (priv.prevpos == 0) {
              priv.displaymarker = 0;
            } else {
              priv.displaymarker = BumpDisplayMarker(priv.displaymarker);
            }
          }
          priv.pwlen = priv.prevpos;
          break;
        }
        case '\001':  // Ctrl-A.
          // Clearing input line on just Ctrl-A is odd - but commonly
          // requested. In most toolkits, Ctrl-A does not immediately erase but
          // almost every keypress other than arrow keys will erase afterwards.
          priv.pwlen = 0;
          break;
        case '\023':  // Ctrl-S.
          SwitchKeyboardLayout();
          break;
        case '\025':  // Ctrl-U.
          // Delete the entire input line.
          // i3lock: supports Ctrl-U but not Ctrl-A.
          // xscreensaver: supports Ctrl-U and Ctrl-X but not Ctrl-A.
          priv.pwlen = 0;
          break;
        case 0:       // Shouldn't happen.
        case '\033':  // Escape.
          done = 1;
          break;
        case '\r':  // Return.
        case '\n':  // Return.
          *response = malloc(priv.pwlen + 1);
          if (!echo && MLOCK_PAGE(*response, priv.pwlen + 1) < 0) {
            LogErrno("mlock");
            // We continue anyway, as the user being unable to unlock the screen
            // is worse. But let's alert the user of this.
            DisplayMessage("Error", "Password has not been stored securely.",
                           1);
            WaitForKeypress(1);
          }
          if (priv.pwlen != 0) {
            memcpy(*response, priv.pwbuf, priv.pwlen);
          }
          (*response)[priv.pwlen] = 0;
          status = 1;
          done = 1;
          break;
        default:
          if (priv.inputbuf >= '\000' && priv.inputbuf <= '\037') {
            // Other control character. We ignore them (and specifically do not
            // update the cursor on them) to "discourage" their use in
            // passwords, as most login screens do not support them anyway.
            break;
          }
          if (priv.pwlen < sizeof(priv.pwbuf)) {
            priv.pwbuf[priv.pwlen] = priv.inputbuf;
            ++priv.pwlen;
            priv.displaymarker = BumpDisplayMarker(priv.displaymarker);
          } else {
            Log("Password entered is too long - bailing out");
            done = 1;
            break;
          }
          break;
      }
    }

    // Handle X11 events that queued up.
    while (!done && XPending(display) && (XNextEvent(display, &priv.ev), 1)) {
      if (IsMonitorChangeEvent(display, priv.ev.type)) {
        per_monitor_windows_dirty = 1;
      }
    }
  }

  // priv contains password related data, so better clear it.
  memset(&priv, 0, sizeof(priv));

  if (!done) {
    Log("Unreachable code - the loop above must set done");
  }
  return status;
}

/*! \brief Perform authentication using a helper proxy.
 *
 * \return The authentication status (0 for OK, 1 otherwise).
 */
int Authenticate() {
  int requestfd[2], responsefd[2];
  if (pipe(requestfd)) {
    LogErrno("pipe");
    return 1;
  }
  if (pipe(responsefd)) {
    LogErrno("pipe");
    return 1;
  }

  // Use authproto_pam.
  pid_t childpid = ForkWithoutSigHandlers();
  if (childpid == -1) {
    LogErrno("fork");
    return 1;
  }

  if (childpid == 0) {
    // Child process. Just run authproto_pam.
    // But first, move requestfd[1] to 1 and responsefd[0] to 0.
    close(requestfd[0]);
    close(responsefd[1]);

    if (requestfd[1] == 0) {
      // Tricky case. We don't _expect_ this to happen - after all,
      // initially our own fd 0 should be bound to xsecurelock's main
      // program - but nevertheless let's handle it.
      // At least this implies that no other fd is 0.
      int requestfd1 = dup(requestfd[1]);
      if (requestfd1 == -1) {
        LogErrno("dup");
        _exit(EXIT_FAILURE);
      }
      close(requestfd[1]);
      if (dup2(responsefd[0], 0) == -1) {
        LogErrno("dup2");
        _exit(EXIT_FAILURE);
      }
      close(responsefd[0]);
      if (requestfd1 != 1) {
        if (dup2(requestfd1, 1) == -1) {
          LogErrno("dup2");
          _exit(EXIT_FAILURE);
        }
        close(requestfd1);
      }
    } else {
      if (responsefd[0] != 0) {
        if (dup2(responsefd[0], 0) == -1) {
          LogErrno("dup2");
          _exit(EXIT_FAILURE);
        }
        close(responsefd[0]);
      }
      if (requestfd[1] != 1) {
        if (dup2(requestfd[1], 1) == -1) {
          LogErrno("dup2");
          _exit(EXIT_FAILURE);
        }
        close(requestfd[1]);
      }
    }

    execl(authproto_executable, authproto_executable, NULL);
    LogErrno("execl");
    sleep(2);  // Reduce log spam or other effects from failed execl.
    _exit(EXIT_FAILURE);
  }

  // Otherwise, we're in the parent process.
  close(requestfd[1]);
  close(responsefd[0]);
  for (;;) {
    char *message;
    char *response;
    char type = ReadPacket(requestfd[0], &message, 1);
    switch (type) {
      case PTYPE_INFO_MESSAGE:
        DisplayMessage("PAM says", message, 0);
        explicit_bzero(message, strlen(message));
        free(message);
        PlaySound(SOUND_INFO);
        WaitForKeypress(1);
        break;
      case PTYPE_ERROR_MESSAGE:
        DisplayMessage("Error", message, 1);
        explicit_bzero(message, strlen(message));
        free(message);
        PlaySound(SOUND_ERROR);
        WaitForKeypress(1);
        break;
      case PTYPE_PROMPT_LIKE_USERNAME:
        if (Prompt(message, &response, 1)) {
          WritePacket(responsefd[1], PTYPE_RESPONSE_LIKE_USERNAME, response);
          explicit_bzero(response, strlen(response));
          free(response);
        } else {
          WritePacket(responsefd[1], PTYPE_RESPONSE_CANCELLED, "");
        }
        explicit_bzero(message, strlen(message));
        free(message);
        DisplayMessage("Processing...", "", 0);
        break;
      case PTYPE_PROMPT_LIKE_PASSWORD:
        if (Prompt(message, &response, 0)) {
          WritePacket(responsefd[1], PTYPE_RESPONSE_LIKE_PASSWORD, response);
          explicit_bzero(response, strlen(response));
          free(response);
        } else {
          WritePacket(responsefd[1], PTYPE_RESPONSE_CANCELLED, "");
        }
        explicit_bzero(message, strlen(message));
        free(message);
        DisplayMessage("Processing...", "", 0);
        break;
      case 0:
        goto done;
      default:
        Log("Unknown message type %02x", (int)type);
        explicit_bzero(message, strlen(message));
        free(message);
        goto done;
    }
  }
done:
  close(requestfd[0]);
  close(responsefd[1]);
  int status;
  if (!WaitProc("authproto", &childpid, 1, 0, &status)) {
    Log("WaitPgrp returned false but we were blocking");
    abort();
  }
  if (status == 0) {
    PlaySound(SOUND_SUCCESS);
  }
  return status != 0;
}

/*! \brief The main program.
 *
 * Usage: XSCREENSAVER_WINDOW=window_id ./auth_x11; status=$?
 *
 * \return 0 if authentication successful, anything else otherwise.
 */
int main(int argc_local, char **argv_local) {
  argc = argc_local;
  argv = argv_local;

  setlocale(LC_CTYPE, "");
  setlocale(LC_TIME, "");

  // This is used by displaymarker only; there is slight security relevance here
  // as an attacker who has a screenshot and an exact startup time and PID can
  // guess the password length. Of course, an attacker who records the screen
  // as a video, or points a camera or a microphone at the keyboard, can too.
  struct timeval tv;
  gettimeofday(&tv, NULL);
  srand(tv.tv_sec ^ tv.tv_usec ^ getpid());

  authproto_executable = GetExecutablePathSetting("XSECURELOCK_AUTHPROTO",
                                                  AUTHPROTO_EXECUTABLE, 0);

  // Unless disabled, we shift the login prompt randomly around by a few
  // pixels. This should mostly mitigate burn-in effects from the prompt
  // being displayed all the time, e.g. because the user's mouse is "shivering"
  // and thus the auth prompt reappears soon after timeout.
  burnin_mitigation_max_offset =
      GetIntSetting("XSECURELOCK_BURNIN_MITIGATION", 16);
  if (burnin_mitigation_max_offset > 0) {
    x_offset = rand() % (2 * burnin_mitigation_max_offset + 1) -
               burnin_mitigation_max_offset;
    y_offset = rand() % (2 * burnin_mitigation_max_offset + 1) -
               burnin_mitigation_max_offset;
  }

  // If requested, mitigate burn-in even more by moving the auth prompt while
  // displayed. I bet many will find this annoying though.
  burnin_mitigation_max_offset_change =
      GetIntSetting("XSECURELOCK_BURNIN_MITIGATION_DYNAMIC", 0);

  prompt_timeout = GetIntSetting("XSECURELOCK_AUTH_TIMEOUT", 5 * 60);
  show_username = GetIntSetting("XSECURELOCK_SHOW_USERNAME", 1);
  show_hostname = GetIntSetting("XSECURELOCK_SHOW_HOSTNAME", 1);
  paranoid_password = GetIntSetting("XSECURELOCK_PARANOID_PASSWORD", 1);
  hide_password_completely = GetIntSetting("XSECURELOCK_HIDE_PASSWORD_COMPLETELY", 0);
  disco = GetIntSetting("XSECURELOCK_DISCO_PASSWORD", 0);
  show_datetime = GetIntSetting("XSECURELOCK_SHOW_DATETIME", 0);
  datetime_format = GetStringSetting("XSECURELOCK_DATETIME_FORMAT", "%c");
  have_switch_user_command =
      !!*GetStringSetting("XSECURELOCK_SWITCH_USER_COMMAND", "");
  auth_sounds = GetIntSetting("XSECURELOCK_AUTH_SOUNDS", 0);
  single_auth_window = GetIntSetting("XSECURELOCK_SINGLE_AUTH_WINDOW", 0);

  if ((display = XOpenDisplay(NULL)) == NULL) {
    Log("Could not connect to $DISPLAY");
    return 1;
  }

#ifdef HAVE_XKB_EXT
  int xkb_opcode, xkb_event_base, xkb_error_base;
  int xkb_major_version = XkbMajorVersion, xkb_minor_version = XkbMinorVersion;
  have_xkb_ext =
      XkbQueryExtension(display, &xkb_opcode, &xkb_event_base, &xkb_error_base,
                        &xkb_major_version, &xkb_minor_version);
#endif

  if (!GetHostName(hostname, sizeof(hostname))) {
    return 1;
  }
  if (!GetUserName(username, sizeof(username))) {
    return 1;
  }

  main_window = ReadWindowID();
  if (main_window == None) {
    Log("Invalid/no window ID in XSCREENSAVER_WINDOW");
    return 1;
  }
  Window unused_root;
  Window *unused_children = NULL;
  unsigned int unused_nchildren;
  XQueryTree(display, main_window, &unused_root, &parent_window,
             &unused_children, &unused_nchildren);
  XFree(unused_children);

  Colormap colormap = DefaultColormap(display, DefaultScreen(display));

  XColor dummy;
  XAllocNamedColor(
      display, DefaultColormap(display, DefaultScreen(display)),
      GetStringSetting("XSECURELOCK_AUTH_BACKGROUND_COLOR", "black"),
      &xcolor_background, &dummy);
  XAllocNamedColor(
      display, DefaultColormap(display, DefaultScreen(display)),
      GetStringSetting("XSECURELOCK_AUTH_FOREGROUND_COLOR", "white"),
      &xcolor_foreground, &dummy);
  XAllocNamedColor(display, DefaultColormap(display, DefaultScreen(display)),
                   GetStringSetting("XSECURELOCK_AUTH_WARNING_COLOR", "red"),
                   &xcolor_warning, &dummy);

  core_font = NULL;
#ifdef HAVE_XFT_EXT
  xft_font = NULL;
#endif

  const char *font_name = GetStringSetting("XSECURELOCK_FONT", "");

  // First try parsing the font name as an X11 core font. We're trying these
  // first as their font name format is more restrictive (usually starts with a
  // dash), except for when font aliases are used.
  int have_font = 0;
  if (font_name[0] != 0) {
    core_font = XLoadQueryFont(display, font_name);
    have_font = (core_font != NULL);
#ifdef HAVE_XFT_EXT
    if (!have_font) {
      xft_font = XftFontOpenName(display, DefaultScreen(display), font_name);
      have_font = (xft_font != NULL);
    }
#endif
  }
  if (!have_font) {
    if (font_name[0] != 0) {
      Log("Could not load the specified font %s - trying a default font",
          font_name);
    }
#ifdef HAVE_XFT_EXT
    xft_font = XftFontOpenName(display, DefaultScreen(display), "monospace");
    have_font = (xft_font != NULL);
#endif
    if (!have_font) {
      core_font = XLoadQueryFont(display, "fixed");
      have_font = (core_font != NULL);
    }
  }
  if (!have_font) {
    Log("Could not load a mind-bogglingly stupid font");
    return 1;
  }

#ifdef HAVE_XFT_EXT
  if (xft_font != NULL) {
    XRenderColor xrcolor;
    xrcolor.alpha = 65535;

    // Translate the X11 colors to XRender colors.
    xrcolor.red = xcolor_foreground.red;
    xrcolor.green = xcolor_foreground.green;
    xrcolor.blue = xcolor_foreground.blue;
    XftColorAllocValue(display, DefaultVisual(display, DefaultScreen(display)),
                       DefaultColormap(display, DefaultScreen(display)),
                       &xrcolor, &xft_color_foreground);

    xrcolor.red = xcolor_warning.red;
    xrcolor.green = xcolor_warning.green;
    xrcolor.blue = xcolor_warning.blue;
    XftColorAllocValue(display, DefaultVisual(display, DefaultScreen(display)),
                       DefaultColormap(display, DefaultScreen(display)),
                       &xrcolor, &xft_color_warning);
  }
#endif

  SelectMonitorChangeEvents(display, main_window);

  InitWaitPgrp();

  int status = Authenticate();

  // Clear any possible processing message by closing our windows.
  DestroyPerMonitorWindows(0);

#ifdef HAVE_XFT_EXT
  if (xft_font != NULL) {
    XftColorFree(display, DefaultVisual(display, DefaultScreen(display)),
                 DefaultColormap(display, DefaultScreen(display)),
                 &xft_color_warning);
    XftColorFree(display, DefaultVisual(display, DefaultScreen(display)),
                 DefaultColormap(display, DefaultScreen(display)),
                 &xft_color_foreground);
    XftFontClose(display, xft_font);
  }
#endif

  XFreeColors(display, colormap, &xcolor_warning.pixel, 1, 0);
  XFreeColors(display, colormap, &xcolor_foreground.pixel, 1, 0);
  XFreeColors(display, colormap, &xcolor_background.pixel, 1, 0);

  return status;
}
