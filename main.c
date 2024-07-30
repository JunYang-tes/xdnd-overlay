#include "uthash.h"
#include <X11/X.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <time.h>

typedef struct {
  void *winid;
  int is_dragging;
  int is_running;
  mtx_t mtx;
  UT_hash_handle hh;
} Overlay;
Overlay *g_overlays = NULL;

Overlay *add_overlay(Window window) {
  Overlay *overlay;
  overlay = malloc(sizeof *overlay);
  overlay->winid = (void *)window;
  overlay->is_running = 1;
  overlay->is_dragging = 0;
  mtx_init(&overlay->mtx, mtx_plain);
  HASH_ADD_PTR(g_overlays, winid, overlay);
  printf("Overlay added,key %ld\n", window);
  return overlay;
}
void delete_overlay_from_overlays(Window window) {
  Overlay *overlay = NULL;
  void *key = (void *)window;
  HASH_FIND_PTR(g_overlays, &key, overlay);
  if (overlay != NULL) {
    HASH_DEL(g_overlays, overlay);
    mtx_lock(&overlay->mtx);
    overlay->is_running = 0;
    mtx_unlock(&overlay->mtx);
  }
}
int is_runing(Window window) {
  Overlay *overlay = NULL;
  void *key = (void *)window;
  HASH_FIND_PTR(g_overlays, &key, overlay);
  if (overlay == NULL) {
    printf("[xdnd] No overlay for window %ld\n", window);
    return 0;
  }
  int running = 0;
  mtx_lock(&overlay->mtx);
  running = overlay->is_running;
  mtx_unlock(&overlay->mtx);
  return running;
}
int is_dragging(Window window) {
  Overlay *overlay = NULL;
  printf("try find:%ld\n", window);
  void *key = (void *)window;
  HASH_FIND_PTR(g_overlays, &key, overlay);
  printf("find:\n");
  if (overlay == NULL) {
    printf("[xdnd] No overlay for window %ld\n", window);
    return 0;
  }
  int dragging = 0;
  mtx_lock(&overlay->mtx);
  dragging = overlay->is_dragging;
  mtx_unlock(&overlay->mtx);
  return dragging;
}
void set_is_dragging(Overlay *overlay, int dragging) {
  mtx_lock(&overlay->mtx);
  overlay->is_dragging = dragging;
  mtx_unlock(&overlay->mtx);
}
Window make_x11_overlay(Display *display, Window target_window) {
  int screen = DefaultScreen(display);
  Window root = RootWindow(display, screen);
  // Create an overlay window
  XVisualInfo vinfo;
  XMatchVisualInfo(display, DefaultScreen(display), 32, TrueColor, &vinfo);

  XSetWindowAttributes overlay_attr;
  overlay_attr.colormap = XCreateColormap(display, DefaultRootWindow(display),
                                          vinfo.visual, AllocNone);
  overlay_attr.background_pixel = 0;
  overlay_attr.border_pixel = 0;
  overlay_attr.override_redirect = True;
  XWindowAttributes attr;
  if (!XGetWindowAttributes(display, target_window, &attr)) {
    printf("[xdnd] Cannot get target window(%ld) attributes\n", target_window);
    return 0;
  }

  Window overlay_window = XCreateWindow(
      display, root, attr.x, attr.y, attr.width, attr.height, 0, vinfo.depth,
      InputOutput, vinfo.visual,
      CWColormap | CWBackPixel | CWBorderPixel | CWOverrideRedirect,
      &overlay_attr);

  Atom xa_atom = XInternAtom(display, "XA_ATOM", False);

  // Set window type to dock to make it appear above other windows
  Atom _NET_WM_WINDOW_TYPE = XInternAtom(display, "_NET_WM_WINDOW_TYPE", False);
  Atom _NET_WM_WINDOW_TYPE_DOCK =
      XInternAtom(display, "_NET_WM_WINDOW_TYPE_DOCK", False);
  XChangeProperty(display, overlay_window, _NET_WM_WINDOW_TYPE, xa_atom, 32,
                  PropModeReplace, (unsigned char *)&_NET_WM_WINDOW_TYPE_DOCK,
                  1);
  Atom version = 5;

  Atom XdndAware = XInternAtom(display, "XdndAware", False);
  XChangeProperty(display, overlay_window, XdndAware, xa_atom, 32,
                  PropModeReplace, (unsigned char *)&version, 1);

  // Select input events
  XSelectInput(display, overlay_window,
               ExposureMask | ButtonPressMask | ButtonReleaseMask |
                   PointerMotionMask | KeyPressMask | KeyReleaseMask |
                   ConfigureNotify | StructureNotifyMask);

  XStoreName(display, overlay_window, "Xdnd overlay");
  // Map the overlay window
  XMapWindow(display, overlay_window);
  return overlay_window;
}

void forwardEvent(Display *display, XEvent *event, Window target_window) {
  event->xany.window = target_window;
  XSendEvent(display, target_window, False, SubstructureNotifyMask, event);
  XFlush(display);
}
void updateOverlayGeometry(Display *display, Window overlay_window,
                           Window target_window) {
  XWindowAttributes attr;
  if (XGetWindowAttributes(display, target_window, &attr)) {
    XMoveResizeWindow(display, overlay_window, attr.x, attr.y, attr.width,
                      attr.height);
  }
}
void sendMotion(int x_root, int y_root, int x, int y, Display *display,
                Window root, Window target) {

  XEvent event;
  memset(&event, 0, sizeof(event));
  event.type = MotionNotify;
  event.xmotion.window = target;
  event.xmotion.root = root;
  event.xmotion.subwindow = None;
  event.xmotion.time = CurrentTime;
  event.xmotion.x = x;
  event.xmotion.y = y;
  event.xmotion.x_root = x_root;
  event.xmotion.y_root = y_root;
  event.xmotion.state = 0;
  event.xmotion.is_hint = NotifyNormal;
  event.xmotion.same_screen = True;

  if (XSendEvent(display, target, False, NoEventMask, &event) == 0) {
    fprintf(stderr, "Error sending event\n");
  }

  XFlush(display);
}

static void sendXdndStatus(Display *disp, Atom XdndStatus, Window source,
                           Window target, Atom action) {
  XEvent message;
  memset(&message, 0, sizeof(message));
  message.xclient.type = ClientMessage;
  message.xclient.display = disp;
  message.xclient.window = target;
  message.xclient.message_type = XdndStatus;
  message.xclient.format = 32;
  message.xclient.data.l[0] = source;
  message.xclient.data.l[1] = 1; // Sets accept and want position flags

  // Send back window rectangle coordinates and width
  message.xclient.data.l[2] = 0;
  message.xclient.data.l[3] = 0;

  // Specify action we accept
  message.xclient.data.l[4] = action;
  int r = XSendEvent(disp, target, False, 0, &message);
  if (r == 0) {
    printf("[xdnd] Failed to send status\n");
  } else {
    printf("[xdnd] Status sent\n");
  }
}

typedef struct {
  Window win;
  Overlay *overlay;
  Display *display;
  Window x11_overlay;
} x11_event_loop_param;
void *x11_event_loop(void *arg) {
  x11_event_loop_param *param = (x11_event_loop_param *)arg;
  Window target_window = param->win;
  Overlay *overlay = param->overlay;
  int running = 1;
  XEvent event;
  int screen = DefaultScreen(param->display);
  Window root = RootWindow(param->display, screen);
  Atom XdndEnter = XInternAtom(param->display, "XdndEnter", False);
  Atom XdndPosition = XInternAtom(param->display, "XdndPosition", False);
  Atom XdndLeave = XInternAtom(param->display, "XdndLeave", False);
  Atom XdndDrop = XInternAtom(param->display, "XdndDrop", False);
  Atom XdndFinished = XInternAtom(param->display, "XdndFinished", False);
  Atom XdndSelection = XInternAtom(param->display, "XdndSelection", False);
  Atom XdndStatus = XInternAtom(param->display, "XdndStatus", False);
  Atom XdndActionCopy = XInternAtom(param->display, "XdndActionCopy", False);

  while (running) {
    mtx_lock(&overlay->mtx);
    running = overlay->is_running;
    mtx_unlock(&overlay->mtx);
    XNextEvent(param->display, &event);

    if (event.type == ClientMessage) {
      if (event.xclient.message_type == XdndEnter ||
          event.xclient.message_type == XdndPosition ||
          event.xclient.message_type == XdndLeave ||
          event.xclient.message_type == XdndStatus ||
          event.xclient.message_type == XdndDrop) {
        if (event.xclient.message_type == XdndEnter ||
            event.xclient.message_type == XdndPosition) {
          set_is_dragging(overlay, 1);
        } else if (event.xclient.message_type == XdndLeave ||
                   event.xclient.message_type == XdndDrop) {
          set_is_dragging(overlay, 0);
        }
        if (event.xclient.message_type == XdndPosition) {
          sendXdndStatus(param->display, XdndStatus, param->x11_overlay,
                         event.xclient.data.l[0], event.xclient.data.l[4]);
        }
        int x_root = (event.xclient.data.l[2] >> 16) & 0xFFFF;
        int y_root = event.xclient.data.l[2] & 0xFFFF;
        XWindowAttributes attr;
        XGetWindowAttributes(param->display, param->x11_overlay, &attr);
        int x = x_root - attr.x;
        int y = y_root - attr.y;
        sendMotion(x_root, y_root, x, y, param->display, root, target_window);
      }
    } else if (event.type == ConfigureNotify &&
               event.xconfigure.window == target_window) {
      // Target window has been moved or resized
      updateOverlayGeometry(param->display, param->x11_overlay, target_window);
    } else {
      forwardEvent(param->display, &event, target_window);
    }
  }
  XDestroyWindow(param->display, param->x11_overlay);
  XFlush(param->display);
  mtx_destroy(&overlay->mtx);
  free(overlay);
  free(param);
}

int make_a_overlay(lua_State *L) {
  Display *display = XOpenDisplay(NULL);
  unsigned long win = luaL_checknumber(L, 1);
  printf("[xdnd] target_window %ld\n", win);
  Window x11_overlay = make_x11_overlay(display, win);
  if (x11_overlay == 0) {
    printf("[xdnd] can't create overlay\n");
    return 0;
  }

  Overlay *overlay = add_overlay(win);
  x11_event_loop_param *param = malloc(sizeof(x11_event_loop_param));
  param->win = win;
  param->overlay = overlay;
  param->x11_overlay = x11_overlay;
  param->display = display;
  pthread_t thread;
  pthread_create(&thread, NULL, x11_event_loop, param);
  return 0;
}
int dispose_overlay(lua_State *L) {
  unsigned long win = luaL_checknumber(L, 1);
  delete_overlay_from_overlays(win);

  return 0;
}

int lua_is_dragging(lua_State *L) {
  unsigned long win = luaL_checknumber(L, 1);
  lua_pushboolean(L, is_dragging(win));
  return 1;
}

__attribute__((visibility("default"))) int luaopen_libxdnd(lua_State *L) {
  static const luaL_Reg lib[] = {{"make_a_overlay", make_a_overlay},
                                 {"is_dragging", lua_is_dragging},
                                 {"dispose_overlay", dispose_overlay},
                                 {NULL, NULL}};
  luaL_newlib(L, lib);
  return 1;
}
