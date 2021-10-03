/*
 * SPDX-License-Identifier: GPL-2.0
 * Copyright (C) 2012  Jonathan Neuschäfer
 *
 * It's a hack, really!
 */

#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

static void help(const char *argv0)
{
	printf(
"This is falseflat, an equivalent of topformflat for the False programming\n"
"language. See http://delta.tigris.org/ and https://strlen.com/false for more\n"
"information.\n\n"
"Usage: %s [level] <in_file.f >out_file.f\n", argv0);
}

static void help_die(const char *argv0)
{
	help(argv0);
	exit(EXIT_FAILURE);
}

static void die(const char *msg)
{
	fprintf(stderr, "%s\n", msg);
	exit(EXIT_FAILURE);
}

static void die_unex_eof(void)
{
	die("unexpected end of file");
}

static void flatten(int threshold)
{
	int level = 0;
	int ch, i;

	while (1) {
		ch = getchar();

		if (ch == EOF) {
			if (level > 0)
				die_unex_eof();
			break;
		}

		if (isspace(ch))
			continue;

		/* indentation */
		if (level <= threshold)
			for (i = (ch == ']'); i < level; i++)
				putchar(' ');

		if (ch == '{') {
			putchar(ch);
			while ((ch = getchar()) != '}') {
				if (ch == '\n')
					printf(" // ");
				else if (ch == EOF)
					die_unex_eof();
				else
					putchar(ch);
			}
			putchar(ch);
		} else if (ch == '[') {
			putchar(ch);
			level++;
		} else if (ch == ']') {
			if (level == 0)
				die("unexpected end of lambda");
			level--;
			putchar(ch);
		} else if (isdigit(ch)) {
			do {
				if (ch != EOF)
					putchar(ch);
			} while (isdigit( (ch = getchar()) ));
			if (ch != EOF)
				ungetc(ch, stdin);
		} else if (ch == '\'') {
			putchar(ch);
			putchar(getchar());
		} else if (ch == '"') {
			do {
				putchar(ch);
				ch = getchar();
			} while (ch != '"' && ch != EOF);
			if (ch == EOF)
				die_unex_eof();
			putchar(ch);
		} else {
			/* EVERYTHING ELSE is treated as valid single-character
			   False commands */
			putchar(ch);
		}

		if (level <= threshold)
			putchar('\n');
	}
}

int main (int argc, char ** argv)
{
	if (argc != 2)
		help_die(argv[0]);

	flatten(atoi(argv[1]));
}
