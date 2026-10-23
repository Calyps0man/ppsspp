#ifndef LOG_H__
#define LOG_H__

namespace aemu_postoffice_server {

extern void (*LOG)(const char *format, ...);
}

#endif
