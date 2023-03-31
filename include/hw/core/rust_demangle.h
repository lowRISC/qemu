/* Demangler for the Rust programming language
   Copyright (C) 2016-2019 Free Software Foundation, Inc.
   Written by David Tolnay (dtolnay@gmail.com).

This file is part of the libiberty library.
Libiberty is free software; you can redistribute it and/or
modify it under the terms of the GNU Library General Public
License as published by the Free Software Foundation; either
version 2 of the License, or (at your option) any later version.

In addition to the permissions in the GNU Library General Public
License, the Free Software Foundation gives you unlimited permission
to link the compiled version of this file into combinations with other
programs, and to distribute those combinations without any restriction
coming from the use of this file.  (The Library Public License
restrictions do apply in other respects; for example, they cover
modification of the file, and distribution when not linked into a
combined executable.)

Libiberty is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Library General Public License for more details.

You should have received a copy of the GNU Library General Public
License along with libiberty; see the file COPYING.LIB.
If not, see <http://www.gnu.org/licenses/>.  */

#ifndef QEMU_RUST_DEMANGLE_H
#define QEMU_RUST_DEMANGLE_H

#define DMGL_NO_OPTS     0
#define DMGL_PARAMS      (1 << 0)
#define DMGL_ANSI        (1 << 1)
#define DMGL_JAVA        (1 << 2)
#define DMGL_VERBOSE     (1 << 3)
#define DMGL_TYPES       (1 << 4)
#define DMGL_RET_POSTFIX (1 << 5)
#define DMGL_RET_DROP    (1 << 6)
#define DMGL_AUTO        (1 << 8)
#define DMGL_GNU_V3      (1 << 14)
#define DMGL_GNAT        (1 << 15)
#define DMGL_DLANG       (1 << 16)
#define DMGL_RUST        (1 << 17)
#define DMGL_STYLE_MASK (DMGL_AUTO|DMGL_GNU_V3|DMGL_JAVA|DMGL_GNAT|DMGL_DLANG|DMGL_RUST)
#define DMGL_NO_RECURSE_LIMIT (1 << 18)
#define DEMANGLE_RECURSION_LIMIT 2048

/* Callback typedef for allocation-less demangler interfaces. */
typedef void (*demangle_callbackref)(const char *, size_t, void *);

int rust_demangle_callback(const char *mangled, int options,
                          demangle_callbackref callback, void *opaque);

char * rust_demangle(const char *mangled, int options);

#endif // QEMU_RUST_DEMANGLE_H
