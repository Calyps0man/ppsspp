int printk(const char *format, ...);
int (*log_func)(const char *format, ...) = printk;
