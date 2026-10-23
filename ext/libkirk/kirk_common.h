#pragma once

typedef unsigned char u8;
typedef unsigned short int u16;
typedef unsigned int u32;


// KIRK command buffers are PSP little-endian bytes on every host. These helpers
// also permit unaligned guest buffers; never swap bytes in place before hashing.
static inline u32 kirk_read32le(const void *ptr) {
	const u8 *p = (const u8 *)ptr;
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static inline void kirk_write32le(void *ptr, u32 value) {
	u8 *p = (u8 *)ptr;
	p[0] = (u8)value;
	p[1] = (u8)(value >> 8);
	p[2] = (u8)(value >> 16);
	p[3] = (u8)(value >> 24);
}
