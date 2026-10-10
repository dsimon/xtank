/*
 * Headless bootstrap Xlib stubs -- TEMPORARY, P1.1 only.
 *
 * Deliberate reimplementation of every Xlib entry point referenced by
 * Src/Display/x11.c and Src/Include/x11.h, for the display-free scratch
 * candidate.  Nothing is drawn and no display is opened; the presentation
 * bookkeeping in x11.c still runs so its simulation-visible side effects are
 * preserved.  Replaced by the neutral backend in P1.3/P1.5.
 */
#include <X11/Xlib.h>

static unsigned long stub_next_id = 0x1000;

static Display stub_display = {24, 0};
static XFontStruct stub_font = {
	13, 2, 1,
	{6, 13, 2}
};
static struct _XGC stub_gc;

Display *
XOpenDisplay(char *display_name)
{
	(void) display_name;
	return &stub_display;
}

void
XCloseDisplay(Display *dpy)
{
	(void) dpy;
}

int
XInitThreads(void)
{
	return 1;
}

int
XSetErrorHandler(int (*handler)(Display *, XErrorEvent *))
{
	(void) handler;
	return 0;
}

char *
XDisplayName(char *string)
{
	(void) string;
	return (char *) "headless";
}

int
XGetErrorText(Display *dpy, int code, char *buffer, int length)
{
	(void) dpy;
	(void) code;
	(void) length;
	if (buffer)
		buffer[0] = '\0';
	return 0;
}

int
XGetErrorDatabaseText(Display *dpy, char *name, char *message,
					  char *default_string, char *buffer, int length)
{
	(void) dpy;
	(void) name;
	(void) message;
	(void) default_string;
	(void) length;
	if (buffer)
		buffer[0] = '\0';
	return 0;
}

int
XPending(Display *dpy)
{
	(void) dpy;
	return 0;
}

int
XNextEvent(Display *dpy, XEvent *event)
{
	(void) dpy;
	if (event)
		memset(event, 0, sizeof(*event));
	return 0;
}

int
XCheckTypedWindowEvent(Display *dpy, Window w, int type, XEvent *event)
{
	(void) dpy;
	(void) w;
	(void) type;
	(void) event;
	return 0;
}

int
XFlush(Display *dpy)
{
	(void) dpy;
	return 1;
}

int
XSync(Display *dpy, Bool discard)
{
	(void) dpy;
	(void) discard;
	return 1;
}

int
XSynchronize(Display *dpy, Bool onoff)
{
	(void) dpy;
	(void) onoff;
	return 1;
}

Window
XCreateSimpleWindow(Display *dpy, Window parent, int x, int y,
					unsigned int width, unsigned int height,
					unsigned int border, unsigned long fg,
					unsigned long bg)
{
	(void) dpy;
	(void) parent;
	(void) x;
	(void) y;
	(void) width;
	(void) height;
	(void) border;
	(void) fg;
	(void) bg;
	return stub_next_id++;
}

int
XMapWindow(Display *dpy, Window w)
{
	(void) dpy;
	(void) w;
	return 1;
}

int
XUnmapWindow(Display *dpy, Window w)
{
	(void) dpy;
	(void) w;
	return 1;
}

int
XClearWindow(Display *dpy, Window w)
{
	(void) dpy;
	(void) w;
	return 1;
}

int
XSelectInput(Display *dpy, Window w, long mask)
{
	(void) dpy;
	(void) w;
	(void) mask;
	return 1;
}

int
XSetStandardProperties(Display *dpy, Window w, char *name, char *icon_name,
					   Pixmap icon, char **argv, int argc,
					   XSizeHints *hints)
{
	(void) dpy;
	(void) w;
	(void) name;
	(void) icon_name;
	(void) icon;
	(void) argv;
	(void) argc;
	(void) hints;
	return 1;
}

int
XSetClassHint(Display *dpy, Window w, XClassHint *hint)
{
	(void) dpy;
	(void) w;
	(void) hint;
	return 1;
}

int
XSetWMHints(Display *dpy, Window w, XWMHints *hints)
{
	(void) dpy;
	(void) w;
	(void) hints;
	return 1;
}

int
XSetWMProperties(Display *dpy, Window w, char *name, char *icon_name,
				 char **argv, int argc, XSizeHints *hints,
				 XWMHints *wmhints, XClassHint *classhint)
{
	(void) dpy;
	(void) w;
	(void) name;
	(void) icon_name;
	(void) argv;
	(void) argc;
	(void) hints;
	(void) wmhints;
	(void) classhint;
	return 1;
}

int
XBell(Display *dpy, int percent)
{
	(void) dpy;
	(void) percent;
	return 1;
}

int
XDefineCursor(Display *dpy, Window w, Cursor cursor)
{
	(void) dpy;
	(void) w;
	(void) cursor;
	return 1;
}

int
XSetForeground(Display *dpy, GC gc, unsigned long fg)
{
	(void) dpy;
	(void) gc;
	(void) fg;
	return 1;
}

Pixmap
XCreateBitmapFromData(Display *dpy, Drawable d, const char *data,
					  unsigned int width, unsigned int height)
{
	(void) dpy;
	(void) d;
	(void) data;
	(void) width;
	(void) height;
	return stub_next_id++;
}

Pixmap
XCreatePixmap(Display *dpy, Drawable d, unsigned int width,
			  unsigned int height, unsigned int depth)
{
	(void) dpy;
	(void) d;
	(void) width;
	(void) height;
	(void) depth;
	return stub_next_id++;
}

int
XFreePixmap(Display *dpy, Pixmap pixmap)
{
	(void) dpy;
	(void) pixmap;
	return 1;
}

int
XCopyArea(Display *dpy, Drawable src, Drawable dest, GC gc, int src_x,
		  int src_y, unsigned int w, unsigned int h, int dest_x, int dest_y)
{
	(void) dpy;
	(void) src;
	(void) dest;
	(void) gc;
	(void) src_x;
	(void) src_y;
	(void) w;
	(void) h;
	(void) dest_x;
	(void) dest_y;
	return 1;
}

int
XCopyPlane(Display *dpy, Drawable src, Drawable dest, GC gc, int src_x,
		   int src_y, unsigned int w, unsigned int h, int dest_x,
		   int dest_y, unsigned long plane)
{
	(void) dpy;
	(void) src;
	(void) dest;
	(void) gc;
	(void) src_x;
	(void) src_y;
	(void) w;
	(void) h;
	(void) dest_x;
	(void) dest_y;
	(void) plane;
	return 1;
}

GC
XCreateGC(Display *dpy, Drawable d, unsigned long valuemask, XGCValues *values)
{
	(void) dpy;
	(void) d;
	(void) valuemask;
	(void) values;
	return &stub_gc;
}

int
XFreeGC(Display *dpy, GC gc)
{
	(void) dpy;
	(void) gc;
	return 1;
}

int
XDrawPoint(Display *dpy, Drawable d, GC gc, int x, int y)
{
	(void) dpy;
	(void) d;
	(void) gc;
	(void) x;
	(void) y;
	return 1;
}

int
XDrawPoints(Display *dpy, Drawable d, GC gc, XPoint *points,
			int npoints, int mode)
{
	(void) dpy;
	(void) d;
	(void) gc;
	(void) points;
	(void) npoints;
	(void) mode;
	return 1;
}

int
XDrawLine(Display *dpy, Drawable d, GC gc, int x1, int y1, int x2, int y2)
{
	(void) dpy;
	(void) d;
	(void) gc;
	(void) x1;
	(void) y1;
	(void) x2;
	(void) y2;
	return 1;
}

int
XDrawSegments(Display *dpy, Drawable d, GC gc, XSegment *segs, int nsegs)
{
	(void) dpy;
	(void) d;
	(void) gc;
	(void) segs;
	(void) nsegs;
	return 1;
}

int
XDrawRectangle(Display *dpy, Drawable d, GC gc, int x, int y,
			   unsigned int width, unsigned int height)
{
	(void) dpy;
	(void) d;
	(void) gc;
	(void) x;
	(void) y;
	(void) width;
	(void) height;
	return 1;
}

int
XFillRectangle(Display *dpy, Drawable d, GC gc, int x, int y,
			   unsigned int width, unsigned int height)
{
	(void) dpy;
	(void) d;
	(void) gc;
	(void) x;
	(void) y;
	(void) width;
	(void) height;
	return 1;
}

int
XDrawArc(Display *dpy, Drawable d, GC gc, int x, int y,
		 unsigned int width, unsigned int height, int angle1, int angle2)
{
	(void) dpy;
	(void) d;
	(void) gc;
	(void) x;
	(void) y;
	(void) width;
	(void) height;
	(void) angle1;
	(void) angle2;
	return 1;
}

int
XDrawString(Display *dpy, Drawable d, GC gc, int x, int y, char *string,
			int length)
{
	(void) dpy;
	(void) d;
	(void) gc;
	(void) x;
	(void) y;
	(void) string;
	(void) length;
	return 1;
}

int
XDrawImageString(Display *dpy, Drawable d, GC gc, int x, int y,
				 char *string, int length)
{
	(void) dpy;
	(void) d;
	(void) gc;
	(void) x;
	(void) y;
	(void) string;
	(void) length;
	return 1;
}

int
XQueryBestSize(Display *dpy, int class, Drawable d, unsigned int width,
			   unsigned int height, unsigned int *ret_width,
			   unsigned int *ret_height)
{
	(void) dpy;
	(void) class;
	(void) d;
	if (ret_width)
		*ret_width = width;
	if (ret_height)
		*ret_height = height;
	return 1;
}

XFontStruct *
XLoadQueryFont(Display *dpy, char *name)
{
	(void) dpy;
	(void) name;
	return &stub_font;
}

int
XQueryColor(Display *dpy, Colormap cmap, XColor *def)
{
	(void) dpy;
	(void) cmap;
	(void) def;
	return 1;
}

int
XParseColor(Display *dpy, Colormap cmap, char *spec, XColor *def)
{
	(void) dpy;
	(void) cmap;
	(void) spec;
	if (def) {
		def->red = def->green = def->blue = 0;
		def->pixel = 0;
	}
	return 1;
}

int
XAllocColor(Display *dpy, Colormap cmap, XColor *def)
{
	(void) dpy;
	(void) cmap;
	if (def)
		def->pixel = def->red;
	return 1;
}

int
XAllocNamedColor(Display *dpy, Colormap cmap, char *name, XColor *screen_def,
				 XColor *exact_def)
{
	(void) dpy;
	(void) cmap;
	(void) name;
	if (screen_def)
		screen_def->pixel = 0;
	if (exact_def)
		exact_def->pixel = 0;
	return 1;
}

Cursor
XCreatePixmapCursor(Display *dpy, Pixmap source, Pixmap mask,
					XColor *fg, XColor *bg, unsigned int x, unsigned int y)
{
	(void) dpy;
	(void) source;
	(void) mask;
	(void) fg;
	(void) bg;
	(void) x;
	(void) y;
	return stub_next_id++;
}

KeySym
XLookupKeysym(XKeyEvent *key_event, int index)
{
	(void) key_event;
	(void) index;
	return 0;
}

int
XLookupString(XKeyEvent *event, char *buffer, int nbytes, KeySym *keysym,
			  void *compose)
{
	(void) event;
	(void) nbytes;
	(void) compose;
	if (buffer && nbytes > 0)
		buffer[0] = '\0';
	if (keysym)
		*keysym = 0;
	return 0;
}

KeyCode
XKeysymToKeycode(Display *dpy, KeySym keysym)
{
	(void) dpy;
	return (KeyCode) keysym;
}

char *
XKeysymToString(KeySym keysym)
{
	(void) keysym;
	return (char *) "";
}

char **
XGetFontPath(Display *dpy, int *npaths)
{
	(void) dpy;
	if (npaths)
		*npaths = 0;
	return (char **) 0;
}

int
XFreeFontPath(char **list)
{
	(void) list;
	return 1;
}

void
XrmInitialize(void)
{
}

char *
XResourceManagerString(Display *dpy)
{
	(void) dpy;
	return (char *) 0;
}

static struct _XrmHashBucketRec stub_db;

XrmDatabase
XrmGetStringDatabase(char *data)
{
	(void) data;
	return &stub_db;
}

XrmQuark
XrmStringToName(char *string)
{
	return (XrmQuark) (unsigned long) string;
}

XrmQuark
XrmStringToClass(char *string)
{
	return (XrmQuark) (unsigned long) string;
}

void
XrmStringToNameList(char *str, XrmName *list)
{
	(void) str;
	if (list)
		list[0] = (XrmName) 0;
}

void
XrmStringToClassList(char *str, XrmClass *list)
{
	(void) str;
	if (list)
		list[0] = (XrmClass) 0;
}

int
XrmQGetResource(XrmDatabase db, XrmName *name, XrmClass *class,
				XrmRepresentation *type, XrmValue *value)
{
	(void) db;
	(void) name;
	(void) class;
	(void) type;
	(void) value;
	return 0;
}

char *
XGetDefault(Display *dpy, char *program, char *option)
{
	(void) dpy;
	(void) program;
	(void) option;
	return (char *) 0;
}

/* Display/XMultiSync.c is omitted from the scratch build; supply its symbol. */
int
XMultiSync(Display *dpys[], int num_dpys, int discard)
{
	(void) dpys;
	(void) num_dpys;
	(void) discard;
	return 0;
}
