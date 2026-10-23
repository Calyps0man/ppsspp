#include "Common/Log.h"

#include <stdio.h>
#include <stdarg.h>

static int log_impl(const char *format, ...){
	va_list args;
	char log_buf[2048] = {0};

	va_start(args, format);
	int len = vsnprintf(log_buf, sizeof(log_buf), format, args);
	va_end(args);

	if (len > 0){
		// chew off the ending '\n' which is a standard in this project, but not in PPSSPP
		log_buf[len - 1] = '\0';
		ERROR_LOG(Log::sceNet, "%s", log_buf);
	}

	return 0;
}

extern "C" {

int (*log_func)(const char *format, ...) = log_impl;

}
