/* utility stuff */

#include "ecb.h"

#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define MIN(a,b) (((a) < (b))? (a):(b))

/* panic-on-OOM allocation functions / other fatal stuff */
static void oom(size_t sz)
{
	fprintf(stderr, "fatal error: Can't allocate %lu bytes of memory!\n",
			(unsigned long) sz);
	exit(EXIT_FAILURE);
}

void *xmalloc(size_t sz)
{
	void *p = malloc(sz);
	if (!p)
		oom(sz);
	return p;
}

FILE *xfopen(const char *path, const char *mode)
{
	FILE *f = fopen(path, mode);

	if (!f) {
		fprintf(stderr, "Can't open '%s': %s", path, strerror(errno));
		exit(EXIT_FAILURE);
	}

	return f;
}


/* the growbuf "library" */

#define GROWBUF_NODE_SIZE 256

static char EMPTY_STRING[1];

struct growbuf_node {
	struct growbuf_node *next;
	size_t size, filled;
	char *buf;
};

struct growbuf {
	struct growbuf_node *first, *last;
};

struct growbuf *growbuf_new(void)
{
	struct growbuf *buf = xmalloc(sizeof(*buf));

	buf->first = NULL;
	buf->last = NULL;

	return buf;
}

void growbuf_free(struct growbuf *buf)
{
	struct growbuf_node *node, *next;

	assert(buf);

	for (node = buf->first; node; node = next) {
		next = node->next;

		assert(node->buf);
		free(node->buf);
		free(node);
	}

	free(buf);
}

static struct growbuf_node *growbuf_new_node(void)
{
	struct growbuf_node *node = xmalloc(sizeof(*node));

	node->next = NULL;
	node->size = GROWBUF_NODE_SIZE;
	node->filled = 0;
	node->buf = xmalloc(node->size);

	return node;
}

static void growbuf_check_node(struct growbuf *buf)
{
	assert(buf);

	if (!buf->last) {
		assert(buf->first == NULL);
		buf->last = buf->first = growbuf_new_node();
	}

	if (buf->last->filled == buf->last->size) {
		struct growbuf_node *node = growbuf_new_node();
		assert(buf->last->next == NULL);
		buf->last->next = node;
		buf->last = node;
	}
}

void growbuf_add(struct growbuf *buf, const char *text, size_t len)
{
	while (len) {
		growbuf_check_node(buf);
		size_t to_copy = MIN(len, buf->last->size - buf->last->filled);
		memcpy(buf->last->buf + buf->last->filled, text, to_copy);
		text += to_copy;
		len -= to_copy;
		buf->last->filled += to_copy;
	}
}

size_t growbuf_len(struct growbuf *buf)
{
	size_t len = 0;
	struct growbuf_node *node;

	assert(buf != NULL);

	for (node = buf->first; node; node = node->next)
		len += node->filled;

	return len;
}

static void growbuf_concat(struct growbuf *buf)
{
	char *dst, *p;
	size_t size;
	struct growbuf_node *node, *next;

	assert(buf != NULL);

	if (buf->first == NULL || buf->first->next == NULL)
		return;

	size = growbuf_len(buf);
	p = dst = xmalloc(size);

	for (node = buf->first; node; node = next) {
		memcpy(p, node->buf, node->filled);
		p += node->filled;

		free(node->buf);
		next = node->next;
		free(node);
	}

	node = xmalloc(sizeof(*node));
	node->next = NULL;
	node->size = size;
	node->filled = size;
	node->buf = dst;
	buf->last = buf->first = node;
}

char *growbuf_buf(struct growbuf *buf)
{
	assert(buf != NULL);

	growbuf_concat(buf);
	if (buf->first)
		return buf->first->buf;
	return EMPTY_STRING;
}
