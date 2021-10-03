/*
 * SPDX-License-Identifier: GPL-2.0
 * Copyright (C) 2012-2013  Jonathan Neuschäfer <j.neuschaefer@gmx.net>
 *
 * utility functions and stuff.
 */

#include <stdlib.h>

void *xmalloc(size_t sz);
FILE *xfopen(const char *path, const char *mode);

struct growbuf;
struct growbuf *growbuf_new(void);
void growbuf_free(struct growbuf *buf);
void growbuf_add(struct growbuf *buf, const char *text, size_t length);
const char *growbuf_buf(struct growbuf *buf);
size_t growbuf_len(struct growbuf *buf);
