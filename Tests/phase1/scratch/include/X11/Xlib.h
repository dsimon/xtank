/*
 * Headless bootstrap shadow of <X11/Xlib.h> -- TEMPORARY, P1.1 only.
 *
 * This file is a deliberate reimplementation of the minimal Xlib surface used
 * by Src/Display/x11.c and the X*() macros in Src/Include/x11.h.  It exists so
 * the P1.1 bootstrap can build a scratch, display-free candidate and reproduce
 * the extraction gate's headless evidence.  It is NOT the real X11 API and is
 * not linked into the reference or the eventual server build; P1.3/P1.5 replace
 * this whole approach with a neutral backend and this directory is deleted.
 *
 * Every function prototype here resolves to the no-op implementation in
 * Tests/phase1/scratch/stubs.c when the shadow directory is placed ahead of the
 * system headers on the include path.
 */
#ifndef P11_SHADOW_XLIB_H
#define P11_SHADOW_XLIB_H

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifndef _Xconst
#define _Xconst const
#endif

typedef int Bool;
typedef int Status;
typedef char *XPointer;
typedef unsigned long Atom;
typedef unsigned long Window;
typedef unsigned long Drawable;
typedef unsigned long Pixmap;
typedef unsigned long Cursor;
typedef unsigned long Colormap;
typedef unsigned long KeySym;
typedef unsigned char KeyCode;
typedef unsigned long XID;

#ifndef True
#define True  1
#define False 0
#endif

typedef struct _XDisplay {
	int planes;
	int dummy;
} Display;

typedef struct _XGC {
	int dummy;
} *GC;

typedef struct _XVisual {
	int dummy;
} Visual;

typedef struct {
	unsigned long pixel;
	unsigned short red, green, blue;
	char flags;
	char pad;
} XColor;

typedef struct {
	int width;
	int ascent;
	int descent;
} XCharStruct;

typedef struct {
	int ascent;
	int descent;
	int fid;
	XCharStruct max_bounds;
} XFontStruct;

typedef struct {
	short x, y;
} XPoint;

typedef struct {
	short x1, y1, x2, y2;
} XSegment;

typedef struct {
	short x, y;
	unsigned short width, height;
} XRectangle;

typedef struct {
	long function;
	unsigned long plane_mask;
	unsigned long foreground;
	unsigned long background;
	int line_width;
	int line_style;
	int cap_style;
	int join_style;
	int fill_style;
	int fill_rule;
	int arc_mode;
	Pixmap tile;
	Pixmap stipple;
	int ts_x_origin, ts_y_origin;
	Pixmap font;
	int subwindow_mode;
	Bool graphics_exposures;
	int clip_x_origin, clip_y_origin;
	Pixmap clip_mask;
	int dash_offset;
	char dashes;
} XGCValues;

typedef struct {
	int flags;
	int x, y;
	int width, height;
	int min_width, min_height;
	int max_width, max_height;
} XSizeHints;

typedef struct {
	long flags;
	Bool input;
	int initial_state;
	Pixmap icon_pixmap;
	Window icon_window;
	int icon_x, icon_y;
	Pixmap icon_mask;
	XID window_group;
} XWMHints;

typedef struct {
	char *res_name;
	char *res_class;
} XClassHint;

typedef struct {
	int type;
	unsigned long serial;
	Bool send_event;
	Display *display;
	Window window;
} XAnyEvent;

typedef struct {
	int type;
	unsigned long serial;
	Bool send_event;
	Display *display;
	Window window;
	Window root;
	Window subwindow;
	unsigned long time;
	int x, y, x_root, y_root;
	unsigned int state;
	unsigned int keycode;
} XKeyEvent;

typedef struct {
	int type;
	unsigned long serial;
	Bool send_event;
	Display *display;
	Window window;
	Window root;
	Window subwindow;
	unsigned long time;
	int x, y, x_root, y_root;
	unsigned int state;
	unsigned int button;
} XButtonEvent;

typedef struct {
	int type;
	unsigned long serial;
	Bool send_event;
	Display *display;
	Window window;
	Window root;
	Window subwindow;
	unsigned long time;
	int x, y, x_root, y_root;
	unsigned int state;
	char is_hint;
} XMotionEvent;

typedef union _XEvent {
	int type;
	XAnyEvent xany;
	XKeyEvent xkey;
	XButtonEvent xbutton;
	XMotionEvent xmotion;
	long pad[24];
} XEvent;

typedef struct {
	int type;
	Display *display;
	XID resourceid;
	unsigned long serial;
	unsigned char error_code;
	unsigned char request_code;
	unsigned char minor_code;
} XErrorEvent;

typedef unsigned long XrmQuark;
typedef XrmQuark XrmName;
typedef XrmQuark XrmClass;
typedef XrmQuark XrmRepresentation;
typedef struct {
	unsigned int size;
	char *addr;
} XrmValue;
typedef struct _XrmHashBucketRec *XrmDatabase;

struct _XrmHashBucketRec {
	int dummy;
};

struct XKeytrans {
	int x;
};

#define DefaultScreen(dpy)		0
#define DisplayWidth(dpy,scr)		1024
#define DisplayHeight(dpy,scr)		864
#define DisplayPlanes(dpy,scr)		((dpy)->planes)
#define RootWindow(dpy,scr)		((Window) 0x100)
#define DefaultColormap(dpy,scr)	((Colormap) 0)
#define DefaultVisual(dpy,scr)		((Visual *) 0)
#define BlackPixel(dpy,scr)		((unsigned long) 0)
#define WhitePixel(dpy,scr)		((unsigned long) 1)

#define Button1		1
#define Button2		2
#define Button3		3

#define ButtonPressMask		(1L<<2)
#define ButtonReleaseMask	(1L<<3)
#define KeyPressMask		(1L<<0)
#define PointerMotionMask	(1L<<6)
#define ExposureMask		(1L<<15)

#define PPosition	(1<<2)
#define PSize		(1<<3)
#define InputHint	(1<<0)
#define IconPixmapHint	(1<<2)

#define GXcopy	3
#define GXxor	6
#define GCFunction		(1L<<0)
#define GCPlaneMask		(1L<<1)
#define GCForeground	(1L<<2)
#define GCBackground	(1L<<3)
#define GCLineWidth		(1L<<4)
#define GCLineStyle		(1L<<5)
#define GCCapStyle		(1L<<6)
#define GCJoinStyle		(1L<<7)
#define GCFillStyle		(1L<<8)
#define GCFillRule		(1L<<9)
#define GCTile			(1L<<10)
#define GCStipple		(1L<<11)
#define GCFont			(1L<<14)
#define GCDashList		(1L<<11)
#define GCGraphicsExposures	(1L<<16)

#define LineSolid		0
#define LineOnOffDash	1
#define CoordModeOrigin	0
#define TileShape		1

#define Expose			12
#define KeyPress		2
#define ButtonPress		4
#define ButtonRelease		5
#define MotionNotify	6

#define NULLQUARK		((XrmQuark) 0L)
#define None			0L

#define IsKeypadKey(ks)		0

Display *XOpenDisplay(char *display_name);
void XCloseDisplay(Display *dpy);
int XInitThreads(void);
int XSetErrorHandler(int (*handler)(Display *, XErrorEvent *));
char *XDisplayName(char *string);
int XGetErrorText(Display *dpy, int code, char *buffer, int length);
int XGetErrorDatabaseText(Display *dpy, char *name, char *message,
						  char *default_string, char *buffer, int length);
int XPending(Display *dpy);
int XNextEvent(Display *dpy, XEvent *event);
int XCheckTypedWindowEvent(Display *dpy, Window w, int type, XEvent *event);
int XFlush(Display *dpy);
int XSync(Display *dpy, Bool discard);
int XSynchronize(Display *dpy, Bool onoff);
Window XCreateSimpleWindow(Display *dpy, Window parent, int x, int y,
						   unsigned int width, unsigned int height,
						   unsigned int border, unsigned long fg,
						   unsigned long bg);
int XMapWindow(Display *dpy, Window w);
int XUnmapWindow(Display *dpy, Window w);
int XClearWindow(Display *dpy, Window w);
int XSelectInput(Display *dpy, Window w, long mask);
int XSetStandardProperties(Display *dpy, Window w, char *name, char *icon_name,
						   Pixmap icon, char **argv, int argc,
						   XSizeHints *hints);
int XSetClassHint(Display *dpy, Window w, XClassHint *hint);
int XSetWMHints(Display *dpy, Window w, XWMHints *hints);
int XSetWMProperties(Display *dpy, Window w, char *name, char *icon_name,
					 char **argv, int argc, XSizeHints *hints,
					 XWMHints *wmhints, XClassHint *classhint);
int XBell(Display *dpy, int percent);
int XDefineCursor(Display *dpy, Window w, Cursor cursor);
int XSetForeground(Display *dpy, GC gc, unsigned long fg);

Pixmap XCreateBitmapFromData(Display *dpy, Drawable d, const char *data,
							 unsigned int width, unsigned int height);
Pixmap XCreatePixmap(Display *dpy, Drawable d, unsigned int width,
					 unsigned int height, unsigned int depth);
int XFreePixmap(Display *dpy, Pixmap pixmap);
int XCopyArea(Display *dpy, Drawable src, Drawable dest, GC gc, int src_x,
			  int src_y, unsigned int w, unsigned int h, int dest_x, int dest_y);
int XCopyPlane(Display *dpy, Drawable src, Drawable dest, GC gc, int src_x,
			   int src_y, unsigned int w, unsigned int h, int dest_x,
			   int dest_y, unsigned long plane);
GC XCreateGC(Display *dpy, Drawable d, unsigned long valuemask, XGCValues *values);
int XFreeGC(Display *dpy, GC gc);
int XDrawPoint(Display *dpy, Drawable d, GC gc, int x, int y);
int XDrawPoints(Display *dpy, Drawable d, GC gc, XPoint *points,
				int npoints, int mode);
int XDrawLine(Display *dpy, Drawable d, GC gc, int x1, int y1, int x2, int y2);
int XDrawSegments(Display *dpy, Drawable d, GC gc, XSegment *segs, int nsegs);
int XDrawRectangle(Display *dpy, Drawable d, GC gc, int x, int y,
				   unsigned int width, unsigned int height);
int XFillRectangle(Display *dpy, Drawable d, GC gc, int x, int y,
				   unsigned int width, unsigned int height);
int XDrawArc(Display *dpy, Drawable d, GC gc, int x, int y,
			 unsigned int width, unsigned int height, int angle1, int angle2);
int XDrawString(Display *dpy, Drawable d, GC gc, int x, int y, char *string,
				int length);
int XDrawImageString(Display *dpy, Drawable d, GC gc, int x, int y,
					 char *string, int length);

int XQueryBestSize(Display *dpy, int class, Drawable d, unsigned int width,
				   unsigned int height, unsigned int *ret_width,
				   unsigned int *ret_height);
XFontStruct *XLoadQueryFont(Display *dpy, char *name);
int XQueryColor(Display *dpy, Colormap cmap, XColor *def);
int XParseColor(Display *dpy, Colormap cmap, char *spec, XColor *def);
int XAllocColor(Display *dpy, Colormap cmap, XColor *def);
int XAllocNamedColor(Display *dpy, Colormap cmap, char *name, XColor *screen_def,
					 XColor *exact_def);
Cursor XCreatePixmapCursor(Display *dpy, Pixmap source, Pixmap mask,
						   XColor *fg, XColor *bg, unsigned int x, unsigned int y);
KeySym XLookupKeysym(XKeyEvent *key_event, int index);
int XLookupString(XKeyEvent *event, char *buffer, int nbytes, KeySym *keysym,
				  void *compose);
KeyCode XKeysymToKeycode(Display *dpy, KeySym keysym);
char *XKeysymToString(KeySym keysym);
char **XGetFontPath(Display *dpy, int *npaths);
int XFreeFontPath(char **list);

void XrmInitialize(void);
char *XResourceManagerString(Display *dpy);
XrmDatabase XrmGetStringDatabase(char *data);
XrmQuark XrmStringToName(char *string);
XrmQuark XrmStringToClass(char *string);
void XrmStringToNameList(char *str, XrmName *list);
void XrmStringToClassList(char *str, XrmClass *list);
int XrmQGetResource(XrmDatabase db, XrmName *name, XrmClass *class,
					XrmRepresentation *type, XrmValue *value);
char *XGetDefault(Display *dpy, char *program, char *option);

#endif /* P11_SHADOW_XLIB_H */
