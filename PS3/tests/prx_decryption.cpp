// Copyright (c) 2012- PPSSPP Project. GPL-2.0-or-later.
// Standalone regression runner linked to the production PRX and KIRK objects.
#include <cstdio>
#include <cstring>
#include <vector>
#include "Common/Log.h"
#include "Core/ELF/PrxDecrypter.h"
#include "ext/libkirk/kirk_engine.h"

static bool logging = false;
bool *g_bLogEnabledSetting = &logging;
LogChannel g_log[(size_t)Log::NUMBER_OF_LOGS];
void GenericLog(Log, LogLevel, const char *, int, const char *, ...) {}
#ifdef PPSSPP_PS3_FRONTEND
namespace PS3Frontend {
void Trace(const char *, ...) {}
}
#endif

static int checks, failures;
static void Check(bool ok, const char *what) {
	++checks;
	if (!ok) { ++failures; printf("FAIL: %s\n", what); }
}

static std::vector<u8> Read(const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f) return {};
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	std::vector<u8> data(size);
	if (fread(data.data(), 1, size, f) != (size_t)size) data.clear();
	fclose(f);
	return data;
}

int main(int argc, char **argv) {
	if (argc != 4) return 2;
	const auto expected = Read(argv[1]);
	Check(expected.size() == 64, "fixture plaintext loaded");
	for (int i = 2; i < argc; ++i) {
		auto input = Read(argv[i]);
		Check(input.size() == 0x190, "PRX fixture loaded");
		if (input.size() != 0x190 || expected.size() != 64) continue;
		std::vector<u8> output(input.size());
		int size = pspDecryptPRX(input.data(), output.data(), (u32)input.size());
		Check(size == (int)expected.size(), "decrypt size");
		Check(memcmp(output.data(), expected.data(), expected.size()) == 0, "independent golden plaintext");
		auto inplace = input;
		size = pspDecryptPRX(inplace.data(), inplace.data(), (u32)inplace.size());
		Check(size == (int)expected.size(), "in-place decrypt size");
		Check(memcmp(inplace.data(), expected.data(), expected.size()) == 0, "in-place golden plaintext");
		input[0x150] ^= 1;
		Check(pspDecryptPRX(input.data(), output.data(), (u32)input.size()) < 0, "CMAC rejects altered payload");
		Check(pspDecryptPRX(input.data(), output.data(), 0x14f) < 0, "truncated PRX rejected");
	}
	// Unaligned wire headers, SHA1 known answer, and CBC known plaintext.
	u8 sha[8] = {0, 3, 0, 0, 0, 'a', 'b', 'c'};
	u8 digest[20];
	const u8 abcSHA1[20] = {0xa9,0x99,0x3e,0x36,0x47,0x06,0x81,0x6a,0xba,0x3e,0x25,0x71,0x78,0x50,0xc2,0x6c,0x9c,0xd0,0xd8,0x9d};
	KirkState kirk{};
	Check(kirk_init(&kirk) == 0, "KIRK init");
	Check(kirk_CMD11(&kirk, digest, sha + 1, 7) == 0, "unaligned SHA1 header");
	Check(memcmp(digest, abcSHA1, 20) == 0, "SHA1 abc known answer");
	u8 wire[0x14 + 32] = {};
	kirk_write32le(wire, 4);
	kirk_write32le(wire + 12, 0x4b);
	kirk_write32le(wire + 16, 32);
	for (int i = 0; i < 32; ++i) wire[0x14 + i] = (u8)i;
	u8 encrypted[sizeof(wire)] = {}, plain[32] = {};
	Check(kirk_CMD4(&kirk, encrypted, wire, sizeof(wire)) == 0, "CBC encrypt wire header");
	memcpy(encrypted, wire, 0x14);
	kirk_write32le(encrypted, 5);
	Check(kirk_CMD7(&kirk, plain, encrypted, sizeof(encrypted)) == 0, "CBC decrypt wire header");
	Check(memcmp(plain, wire + 0x14, 32) == 0, "CBC plaintext round trip");
	printf("PRX/KIRK: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
