#include <stdarg.h>
#include <stdio.h>

namespace aemu_postoffice_server {

void log_default(const char *format, ...){
	va_list args;
	va_start(args, format);

	char buf[2048] = {0};
	vsnprintf(buf, sizeof(buf), format, args);
	va_end(args);

	fprintf(stdout, "%s", buf);
}

void (*LOG)(const char *format, ...) = log_default;

}
