#include <stdio.h>
#include <stdarg.h>

static int log_impl(const char *format, ...){
	va_list args;
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
	fflush(stderr);
	return 0;
}

int (*log_func)(const char *format, ...) = log_impl;
