// Copyright 2021 The golang.design Initiative Authors.
// All rights reserved. Use of this source code is governed
// by a MIT license that can be found in the LICENSE file.
//
// Written by Changkun Ou <changkun.de>

//go:build linux && !android

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <dlfcn.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#define INCR_CHUNK_SIZE (64 * 1024)
#define INCR_THRESHOLD (256 * 1024)

// syncStatus is a function from the Go side.
extern void syncStatus(uintptr_t handle, int status);

void *libX11;

Display* (*P_XOpenDisplay)(int);
void (*P_XCloseDisplay)(Display*);
Window (*P_XDefaultRootWindow)(Display*);
Window (*P_XCreateSimpleWindow)(Display*, Window, int, int, int, int, int, int, int);
Atom (*P_XInternAtom)(Display*, char*, int);
void (*P_XSetSelectionOwner)(Display*, Atom, Window, unsigned long);
Window (*P_XGetSelectionOwner)(Display*, Atom);
void (*P_XNextEvent)(Display*, XEvent*);
int (*P_XChangeProperty)(Display*, Window, Atom, Atom, int, int, unsigned char*, int);
void (*P_XSendEvent)(Display*, Window, int, long , XEvent*);
int (*P_XGetWindowProperty) (Display*, Window, Atom, long, long, Bool, Atom, Atom*, int*, unsigned long *, unsigned long *, unsigned char **);
void (*P_XFree) (void*);
void (*P_XDeleteProperty) (Display*, Window, Atom);
void (*P_XConvertSelection)(Display*, Atom, Atom, Atom, Window, Time);
int (*P_XSelectInput)(Display*, Window, long);

int initX11() {
	if (libX11) {
		return 1;
	}
	libX11 = dlopen("libX11.so", RTLD_LAZY);
	if (!libX11) {
		return 0;
	}
	P_XOpenDisplay = (Display* (*)(int)) dlsym(libX11, "XOpenDisplay");
	P_XCloseDisplay = (void (*)(Display*)) dlsym(libX11, "XCloseDisplay");
	P_XDefaultRootWindow = (Window (*)(Display*)) dlsym(libX11, "XDefaultRootWindow");
	P_XCreateSimpleWindow = (Window (*)(Display*, Window, int, int, int, int, int, int, int)) dlsym(libX11, "XCreateSimpleWindow");
	P_XInternAtom = (Atom (*)(Display*, char*, int)) dlsym(libX11, "XInternAtom");
	P_XSetSelectionOwner = (void (*)(Display*, Atom, Window, unsigned long)) dlsym(libX11, "XSetSelectionOwner");
	P_XGetSelectionOwner = (Window (*)(Display*, Atom)) dlsym(libX11, "XGetSelectionOwner");
	P_XNextEvent = (void (*)(Display*, XEvent*)) dlsym(libX11, "XNextEvent");
	P_XChangeProperty = (int (*)(Display*, Window, Atom, Atom, int, int, unsigned char*, int)) dlsym(libX11, "XChangeProperty");
	P_XSendEvent = (void (*)(Display*, Window, int, long , XEvent*)) dlsym(libX11, "XSendEvent");
	P_XGetWindowProperty = (int (*)(Display*, Window, Atom, long, long, Bool, Atom, Atom*, int*, unsigned long *, unsigned long *, unsigned char **)) dlsym(libX11, "XGetWindowProperty");
	P_XFree = (void (*)(void*)) dlsym(libX11, "XFree");
	P_XDeleteProperty = (void (*)(Display*, Window, Atom)) dlsym(libX11, "XDeleteProperty");
	P_XConvertSelection = (void (*)(Display*, Atom, Atom, Atom, Window, Time)) dlsym(libX11, "XConvertSelection");
	P_XSelectInput = (int (*)(Display*, Window, long)) dlsym(libX11, "XSelectInput");
	return 1;
}

int clipboard_test() {
	if (!initX11()) {
		return -1;
	}

    Display* d = NULL;
    for (int i = 0; i < 42; i++) {
        d = (*P_XOpenDisplay)(0);
        if (d == NULL) {
            continue;
        }
        break;
    }
    if (d == NULL) {
        return -1;
    }
    (*P_XCloseDisplay)(d);
    return 0;
}

typedef struct {
    unsigned char* data;
    size_t total_size;
    size_t offset;
    Display* display;
    Window requestor;
    Atom property;
    Atom target;
} incr_state_t;

static incr_state_t *incr_states = NULL;
static int incr_count = 0;

incr_state_t* find_incr_state(Window requestor, Atom property) {
    for (int i = 0; i < incr_count; i++) {
        if (incr_states[i].requestor == requestor && incr_states[i].property == property) {
            return &incr_states[i];
        }
    }
    return NULL;
}

incr_state_t* add_incr_state(Display* display, Window requestor, Atom property, Atom target, unsigned char* data, size_t size) {
    incr_states = realloc(incr_states, (incr_count + 1) * sizeof(incr_state_t));
    incr_state_t* state = &incr_states[incr_count++];
    state->display = display;
    state->requestor = requestor;
    state->property = property;
    state->target = target;
    state->data = malloc(size);
    memcpy(state->data, data, size);
    state->total_size = size;
    state->offset = 0;
    return state;
}

void remove_incr_state(incr_state_t* state) {
    if (!state) return;
    
    free(state->data);
    int index = state - incr_states;
    if (index < incr_count - 1) {
        incr_states[index] = incr_states[incr_count - 1];
    }
    incr_count--;
    if (incr_count == 0) {
        free(incr_states);
        incr_states = NULL;
    } else {
        incr_states = realloc(incr_states, incr_count * sizeof(incr_state_t));
    }
}

void send_incr_chunk(incr_state_t* state) {
    size_t remaining = state->total_size - state->offset;
    size_t chunk_size = remaining > INCR_CHUNK_SIZE ? INCR_CHUNK_SIZE : remaining;
    
    if (chunk_size > 0) {
        (*P_XChangeProperty)(state->display, state->requestor, state->property,
                            state->target, 8, PropModeReplace, 
                            state->data + state->offset, chunk_size);
        state->offset += chunk_size;
    } else {
        (*P_XChangeProperty)(state->display, state->requestor, state->property,
                            state->target, 8, PropModeReplace, NULL, 0);
        remove_incr_state(state);
    }
}

// clipboard_write writes the given buf of size n as type typ.
// if start is provided, the value of start will be changed to 1 to indicate
// if the write is availiable for reading.
int clipboard_write(char *typ, unsigned char *buf, size_t n, uintptr_t handle) {
	if (!initX11()) {
		return -1;
	}

    Display* d = NULL;
    for (int i = 0; i < 42; i++) {
        d = (*P_XOpenDisplay)(0);
        if (d == NULL) {
            continue;
        }
        break;
    }
    if (d == NULL) {
        syncStatus(handle, -1);
        return -1;
    }
    Window w = (*P_XCreateSimpleWindow)(d, (*P_XDefaultRootWindow)(d), 0, 0, 1, 1, 0, 0, 0);

    (*P_XSelectInput)(d, w, PropertyChangeMask);

    // Use False because these may not available for the first time.
    Atom sel         = (*P_XInternAtom)(d, "CLIPBOARD", 0);
    Atom atomString  = (*P_XInternAtom)(d, "UTF8_STRING", 0);
    Atom atomImage   = (*P_XInternAtom)(d, "image/png", 0);
    Atom targetsAtom = (*P_XInternAtom)(d, "TARGETS", 0);
    Atom incrAtom    = (*P_XInternAtom)(d, "INCR", 0);

    // Use True to makesure the requested type is a valid type.
    Atom target = (*P_XInternAtom)(d, typ, 1);
    if (target == None) {
        (*P_XCloseDisplay)(d);
        syncStatus(handle, -2);
        return -2;
    }

    (*P_XSetSelectionOwner)(d, sel, w, CurrentTime);
    if ((*P_XGetSelectionOwner)(d, sel) != w) {
        (*P_XCloseDisplay)(d);
        syncStatus(handle, -3);
        return -3;
    }

    XEvent event;
    XSelectionRequestEvent* xsr;
    int notified = 0;
    for (;;) {
        if (notified == 0) {
            syncStatus(handle, 1); // notify Go side
            notified = 1;
        }

        (*P_XNextEvent)(d, &event);
        switch (event.type) {
        case SelectionClear:
            while (incr_count > 0) {
                remove_incr_state(&incr_states[0]);
            }
            (*P_XCloseDisplay)(d);
            return 0;
            
        case PropertyNotify:
            if (event.xproperty.state == PropertyDelete) {
                incr_state_t* state = find_incr_state(event.xproperty.window, event.xproperty.atom);
                if (state) {
                    send_incr_chunk(state);
                }
            }
            break;
            
        case SelectionNotify:
            break;
            
        case SelectionRequest:
            if (event.xselectionrequest.selection != sel) {
                break;
            }

            XSelectionRequestEvent * xsr = &event.xselectionrequest;
            XSelectionEvent ev = {0};
            int R = 0;

            ev.type      = SelectionNotify;
            ev.display   = xsr->display;
            ev.requestor = xsr->requestor;
            ev.selection = xsr->selection;
            ev.time      = xsr->time;
            ev.target    = xsr->target;
            ev.property  = xsr->property;

            if ((ev.target == atomString || ev.target == atomImage) && ev.target == target) {
                if (n > INCR_THRESHOLD) {
                    unsigned long size = n;
                    R = (*P_XChangeProperty)(ev.display, ev.requestor, ev.property,
                                            incrAtom, 32, PropModeReplace, 
                                            (unsigned char*)&size, 1);
                    
                    add_incr_state(ev.display, ev.requestor, ev.property, ev.target, buf, n);
                    
                    (*P_XSelectInput)(ev.display, ev.requestor, PropertyChangeMask);
                } else {
                    R = (*P_XChangeProperty)(ev.display, ev.requestor, ev.property,
                                            ev.target, 8, PropModeReplace, buf, n);
                }
            } else if (ev.target == targetsAtom) {
                // Reply atoms for supported targets, other clients should
                // request the clipboard again and obtain the data if their
                // implementation is correct.
                Atom targets[] = { atomString, atomImage };
                R = (*P_XChangeProperty)(ev.display, ev.requestor, ev.property,
                    XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)&targets, sizeof(targets)/sizeof(Atom));
            } else {
                ev.property = None;
            }

            if ((R & 2) == 0) (*P_XSendEvent)(d, ev.requestor, 0, 0, (XEvent *)&ev);
            break;
        }
    }
}

// read_data reads the property of a selection if the target atom matches
unsigned long read_data(Display* display, Window window, XSelectionEvent *sev, Atom sel, Atom prop, Atom target, char **buf) {
	if (!initX11()) {
		return -1;
	}

    if (sev->property == None || sev->selection != sel || sev->property != prop) {
        return 0;
    }

    unsigned char *data;
    Atom actual;
    int format;
    unsigned long n    = 0;
    unsigned long size = 0;
    
    int ret = (*P_XGetWindowProperty)(sev->display, sev->requestor, sev->property,
        0L, (~0L), 0, AnyPropertyType, &actual, &format, &size, &n, &data);
    if (ret != Success) {
        return 0;
    }

    Atom incrAtom = (*P_XInternAtom)(display, "INCR", False);
    
    if (actual == incrAtom) {
        (*P_XFree)(data);
        (*P_XDeleteProperty)(sev->display, sev->requestor, sev->property);
        
        (*P_XSelectInput)(display, window, PropertyChangeMask);
        
        unsigned char *incr_buf = NULL;
        unsigned long total_size = 0;
        
        XEvent event;
        for (;;) {
            (*P_XNextEvent)(display, &event);
            if (event.type != PropertyNotify || 
                event.xproperty.window != window ||
                event.xproperty.atom != prop ||
                event.xproperty.state != PropertyNewValue) {
                continue;
            }
            
            ret = (*P_XGetWindowProperty)(display, window, prop,
                0L, (~0L), True, target, &actual, &format, &size, &n, &data);
            if (ret != Success) {
                break;
            }
            
            if (size == 0) {
                (*P_XFree)(data);
                break;
            }
            
            incr_buf = realloc(incr_buf, total_size + size);
            memcpy(incr_buf + total_size, data, size);
            total_size += size;
            
            (*P_XFree)(data);
        }
        
        if (buf != NULL && incr_buf != NULL) {
            *buf = (char*)incr_buf;
        } else if (incr_buf != NULL) {
            free(incr_buf);
        }
        
        return total_size;
    } else if (actual == target && buf != NULL) {
        *buf = (char *)malloc(size * sizeof(char));
        memcpy(*buf, data, size*sizeof(char));
        (*P_XFree)(data);
        (*P_XDeleteProperty)(sev->display, sev->requestor, sev->property);
        return size * sizeof(char);
    }
    
    (*P_XFree)(data);
    (*P_XDeleteProperty)(sev->display, sev->requestor, sev->property);
    return 0;
}

// clipboard_read reads the clipboard selection in given format typ.
// the read bytes are written into buf and returns the size of the buffer.
//
// The caller of this function should responsible for the free of the buf.
unsigned long clipboard_read(char* typ, char **buf) {
	if (!initX11()) {
		return -1;
	}

    Display* d = NULL;
    for (int i = 0; i < 42; i++) {
        d = (*P_XOpenDisplay)(0);
        if (d == NULL) {
            continue;
        }
        break;
    }
    if (d == NULL) {
        return -1;
    }

    Window w = (*P_XCreateSimpleWindow)(d, (*P_XDefaultRootWindow)(d), 0, 0, 1, 1, 0, 0, 0);

    // Use False because these may not available for the first time.
    Atom sel  = (*P_XInternAtom)(d, "CLIPBOARD", False);
    Atom prop = (*P_XInternAtom)(d, "GOLANG_DESIGN_DATA", False);

    // Use True to makesure the requested type is a valid type.
    Atom target = (*P_XInternAtom)(d, typ, True);
    if (target == None) {
        (*P_XCloseDisplay)(d);
        return -2;
    }

    (*P_XConvertSelection)(d, sel, target, prop, w, CurrentTime);
    XEvent event;
    for (;;) {
        (*P_XNextEvent)(d, &event);
        if (event.type != SelectionNotify) continue;
        break;
    }
    unsigned long n = read_data(d, w, (XSelectionEvent *)&event.xselection, sel, prop, target, buf);
    (*P_XCloseDisplay)(d);
    return n;
}
