/*
 * Wine X11DRV X Present interface
 *
 * Copyright 2026 Wine contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __WINE_XPRESENT_H
#define __WINE_XPRESENT_H

#ifndef __WINE_CONFIG_H
# error You must include config.h to use this header
#endif

#ifdef SONAME_LIBXPRESENT

#include <X11/extensions/Xpresent.h>

#define MAKE_FUNCPTR(f) extern typeof(f) * p##f;
MAKE_FUNCPTR(XPresentQueryExtension)
MAKE_FUNCPTR(XPresentQueryVersion)
MAKE_FUNCPTR(XPresentPixmap)
MAKE_FUNCPTR(XPresentSelectInput)
MAKE_FUNCPTR(XPresentFreeInput)
MAKE_FUNCPTR(XPresentQueryCapabilities)
#undef MAKE_FUNCPTR

#endif

#endif /* __WINE_XPRESENT_H */
