#include "imports.h"
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

void usleep(uint32_t time)
{
	((void (*const)(uint32_t))0xe00d7ed4)(time);
}

void* memset(void* dst, int val, size_t size)
{
	char* _dst = dst;

	int i;
	for(i = 0; i < size; i++) _dst[i] = val;

	return dst;
}

void* (*const _memcpy)(void* dst, void* src, int size) = (void*)0xe00d6b78;

void* memcpy(void* dst, const void* src, size_t size)
{
	return _memcpy(dst, (void*)src, size);
}

int strlen(const char* str)
{
    unsigned int i = 0;
    while (str[i]) {
        i++;
    }
    return i;
}

char* strncpy(char* dst, const char* src, size_t size)
{
	int i;
	for(i = 0; i < size; i++)
	{
		dst[i] = src[i];
		if(src[i] == '\0') return dst;
	}

	return dst;
}

int vsnprintf(char * s, size_t n, const char * format, va_list arg)
{
    return ((int (*const)(char*, size_t, const char *, va_list))0xe00d75f0)(s, n, format, arg);
}
