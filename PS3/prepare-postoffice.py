#!/usr/bin/env python3
"""Generate a PS3-only overlay without modifying the pinned upstream submodule."""
import pathlib
import shutil
import sys

src, dst = map(pathlib.Path, sys.argv[1:])
dst.mkdir(parents=True, exist_ok=True)
for path in src.glob('*.h'):
    shutil.copy2(path, dst / path.name)
shutil.copy2(src.parent / 'aemu_postoffice_packets.h', dst.parent / 'aemu_postoffice_packets.h')

def replace(text, old, new):
    if text.count(old) != 1:
        raise RuntimeError('Pinned postoffice source changed: ' + old[:60])
    return text.replace(old, new)

text = (src / 'sock_impl.h').read_text()
text = replace(text, '#if defined(__unix) || defined(__APPLE__) || defined(__PSP__)', '#if defined(__unix) || defined(__APPLE__) || defined(__PSP__) || defined(__PPU__)')
start = text.index('#ifdef __PSP__')
end = text.index('#define NATIVE_SOCK_ABORTED', start)
text = text[:start] + text[end:]
text = replace(text, 'void to_native_sock6_addr(native_sock6_addr *dst, const struct aemu_post_office_sock6_addr *src);', '')
(dst / 'sock_impl.h').write_text(text)
text = (src / 'postoffice_mem.h').read_text()
text = replace(text, 'sizeof(native_sock6_addr) > sizeof(native_sock_addr) ? sizeof(native_sock6_addr) : sizeof(native_sock_addr)', 'sizeof(native_sock_addr)')
(dst / 'postoffice_mem.h').write_text(text)
text = (src / 'postoffice.c').read_text()
for name in ['pdp_create_v6', 'ptp_listen_v6', 'ptp_connect_v6']:
    start = text.index('void *' + name + '(')
    body = text.index('{', start)
    end = text.index('\n}', body)
    text = text[:body + 1] + '\n\tif (state) *state = AEMU_POSTOFFICE_CLIENT_SESSION_NETWORK;\n\treturn NULL;' + text[end:]
(dst / 'postoffice.c').write_text(text)
# 4 sessions of each kind like the PSP build, not 32: 32 take 2 MB of the PS3's scarce main memory.
text = (src / 'postoffice_mem_stdc.c').read_text()
text = replace(text, '#define SESSIONS_PER_TYPE 32', '#define SESSIONS_PER_TYPE 4')
(dst / 'postoffice_mem_stdc.c').write_text(text)
