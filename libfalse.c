/* libfalse - the llfalse helper library */

#include <stdio.h>
#include <stdint.h>

/* TODO: add signedness flag */
void lf_printnum(uint32_t num)
{
	printf("%ld", (long) num);
}

void lf_printstring(const char *str)
{
	printf("%s", str);
}

void lf_putchar(uint32_t ch)
{
	putchar((int)ch);
}

uint32_t lf_getchar(void)
{
	int ch = getchar();
	if (ch == EOF)
		return ~0;
	return (uint32_t) ch;
}

void lf_flush(void)
{
	fflush(stdin);
	fflush(stdout);
}
