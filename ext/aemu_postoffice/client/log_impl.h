#ifndef __LOG_IMPL_H
#define __LOG_IMPL_H

#ifdef __cplusplus
extern "C" {
#endif

// on the psp, this points to the printk function provided by aemu
extern int (*log_func)(const char *format, ...);

#define LOG(...){ \
	log_func(__VA_ARGS__); \
}

#ifdef __cplusplus
}
#endif

#endif
