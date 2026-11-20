/* The Artists page (patch/artists.c; docs/modules/artists.md). Copyright (c) 2026 Xin. MIT licence (see LICENSE). */
#pragma once
#include "libsrc.h"

#define AR_LABELS 64 /* artists_open's Labels page (tools/pod_pages.py AR_LABELS; no stock Home tile has this number) */
int artists_open(int home); /* 0 Artists, HOME_GENRE, HOME_COMPOSER, AR_LABELS; 0 once up, -1 if it could not open */
int artists_open_src(
    const lib_src_t *src); /* the Artists page fed by src->list(LS_ARTISTS); NULL or &lib_stock = artists_open(0) */
