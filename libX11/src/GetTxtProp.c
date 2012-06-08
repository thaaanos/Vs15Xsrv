/***********************************************************
Copyright 1988 by Wyse Technology, Inc., San Jose, Ca.,

                        All Rights Reserved

Permission to use, copy, modify, and distribute this software and its
documentation for any purpose and without fee is hereby granted,
provided that the above copyright notice appear in all copies and that
both that copyright notice and this permission notice appear in
supporting documentation, and that the name Wyse not be
used in advertising or publicity pertaining to distribution of the
software without specific, written prior permission.

WYSE DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE, INCLUDING
ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO EVENT SHALL
DIGITAL BE LIABLE FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR
ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION,
ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
SOFTWARE.

******************************************************************/
/*

Copyright 1988, 1998  The Open Group

Permission to use, copy, modify, distribute, and sell this software and its
documentation for any purpose is hereby granted without fee, provided that
the above copyright notice appear in all copies and that both that
copyright notice and this permission notice appear in supporting
documentation.

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE OPEN GROUP BE LIABLE FOR ANY CLAIM, DAMAGES OR
OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
OTHER DEALINGS IN THE SOFTWARE.

Except as contained in this notice, the name of The Open Group shall
not be used in advertising or otherwise to promote the sale, use or
other dealings in this Software without prior written authorization
from The Open Group.

*/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <X11/Xlibint.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/Xos.h>
#include <stdio.h>

Status XGetTextProperty (
    Display *display,
    Window window,
    XTextProperty *tp,
    Atom property)
{
    Atom actual_type;
    int actual_format = 0;
    unsigned long nitems = 0L, leftover = 0L;
    unsigned char *prop = NULL;

    if (XGetWindowProperty (display, window, property, 0L, 1000000L, False,
			    AnyPropertyType, &actual_type, &actual_format,
			    &nitems, &leftover, &prop) == Success &&
	actual_type != None) {
	/* okay, fill it in */
	tp->value = prop;
	tp->encoding = actual_type;
	tp->format = actual_format;
	tp->nitems = nitems;
	return True;
    }

    tp->value = NULL;
    tp->encoding = None;
    tp->format = 0;
    tp->nitems = 0;
    return False;
}

Status XGetWMName (
    Display *dpy,
    Window w,
    XTextProperty *tp)
{
    return (XGetTextProperty (dpy, w, tp, XA_WM_NAME));
}

Status XGetWMIconName (
    Display *dpy,
    Window w,
    XTextProperty *tp)
{
    return (XGetTextProperty (dpy, w, tp, XA_WM_ICON_NAME));
}

Status XGetWMClientMachine (
    Display *dpy,
    Window w,
    XTextProperty *tp)
{
    return (XGetTextProperty (dpy, w, tp, XA_WM_CLIENT_MACHINE));
}

