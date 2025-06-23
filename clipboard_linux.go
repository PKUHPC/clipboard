// Copyright 2021 The golang.design Initiative Authors.
// All rights reserved. Use of this source code is governed
// by a MIT license that can be found in the LICENSE file.
//
// Written by Changkun Ou <changkun.de>

//go:build linux && !android

package clipboard

/*
#cgo LDFLAGS: -ldl
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

int clipboard_test();
int clipboard_write(
	char*          typ,
	unsigned char* buf,
	size_t         n,
	uintptr_t      handle
);
unsigned long clipboard_read(char* typ, char **out);
*/
import "C"
import (
	"bytes"
	"context"
	"fmt"
	"os"
	"runtime"
	"runtime/cgo"
	"time"
	"unsafe"
)

var helpmsg = `%w: Failed to initialize the X11 display, and the clipboard package
will not work properly. Install the following dependency may help:

	apt install -y libx11-dev

If the clipboard package is in an environment without a frame buffer,
such as a cloud server, it may also be necessary to install xvfb:

	apt install -y xvfb

and initialize a virtual frame buffer:

	Xvfb :99 -screen 0 1024x768x24 > /dev/null 2>&1 &
	export DISPLAY=:99.0

Then this package should be ready to use.
`

func initialize() error {
	ok := C.clipboard_test()
	if ok != 0 {
		return fmt.Errorf(helpmsg, errUnavailable)
	}
	return nil
}

func read(t Format) (buf []byte, err error) {
	switch t {
	case FmtText:
		return readc("UTF8_STRING")
	case FmtImage:
		return readc("image/png")
	}
	return nil, errUnsupported
}

func readc(t string) ([]byte, error) {
	ct := C.CString(t)
	defer C.free(unsafe.Pointer(ct))

	var data *C.char
	n := C.clipboard_read(ct, &data)
	switch C.long(n) {
	case -1:
		return nil, errUnavailable
	case -2:
		return nil, errUnsupported
	}
	if data == nil {
		return nil, errUnavailable
	}
	defer C.free(unsafe.Pointer(data))
	switch {
	case n == 0:
		return nil, nil
	default:
		return C.GoBytes(unsafe.Pointer(data), C.int(n)), nil
	}
}

// write writes the given data to clipboard and
// returns true if success or false if failed.
func write(t Format, buf []byte) (<-chan struct{}, error) {
	var s string
	switch t {
	case FmtText:
		s = "UTF8_STRING"
	case FmtImage:
		s = "image/png"
	}

	start := make(chan int, 1)
	done := make(chan struct{}, 1)

	go func() { // serve as a daemon until the ownership is terminated.
		runtime.LockOSThread()
		defer runtime.UnlockOSThread()

		cs := C.CString(s)
		defer C.free(unsafe.Pointer(cs))

		h := cgo.NewHandle(start)
		defer cgo.Handle(h).Delete()

		var ok C.int
		if len(buf) == 0 {
			ok = C.clipboard_write(cs, nil, 0, C.uintptr_t(h))
		} else {
			ok = C.clipboard_write(cs, (*C.uchar)(unsafe.Pointer(&(buf[0]))), C.size_t(len(buf)), C.uintptr_t(h))
		}
		if ok != C.int(0) {
			if debug {
				fmt.Fprintf(os.Stderr, "clipboard write failed with status: %d\n", int(ok))
			}
		}
		done <- struct{}{}
		close(done)
	}()

	select {
	case status := <-start:
		if status < 0 {
			return nil, errUnavailable
		}
		return done, nil
	case <-time.After(5 * time.Second):
		return nil, fmt.Errorf("clipboard write timeout")
	}
}

func watch(ctx context.Context, t Format) <-chan []byte {
	recv := make(chan []byte, 1)
	ti := time.NewTicker(time.Second)
	last := Read(t)
	go func() {
		defer ti.Stop()
		defer close(recv)
		for {
			select {
			case <-ctx.Done():
				return
			case <-ti.C:
				b := Read(t)
				if b == nil {
					continue
				}
				if !bytes.Equal(last, b) {
					select {
					case recv <- b:
						last = b
					case <-ctx.Done():
						return
					}
				}
			}
		}
	}()
	return recv
}

//export syncStatus
func syncStatus(h uintptr, val int) {
	v := cgo.Handle(h).Value().(chan int)
	select {
	case v <- val:
	default:
	}
}
