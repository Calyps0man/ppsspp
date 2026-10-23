#pragma once
#include <netinet/in.h>
extern "C" {
uint16_t htons(uint16_t);
int inet_pton(int, const char *, void *);
const char *inet_ntop(int, const void *, char *, socklen_t);
}
