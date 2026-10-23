#!/usr/bin/env python3
"""Checks that every settings help text fits the help panel of the options tabs.

The panel (MenuScreens::SettingsPage) shows 3 lines of (1224 - 56 - 32) / 12 = 94 characters,
word-wrapped like MenuScreens.cpp's Wrap(); anything past line 3 is cut off on the PS3.
Run: python3 PS3/frontend/check-help.py   (exit code 1 if a text is too long)
"""
import os
import re
import sys

WIDTH = (1224 - 56 - 32) // 12
LINES = 3


def wrap(text, width):
    lines = []
    for part in text.split('\n'):
        start = 0
        while True:
            end = min(len(part), start + width)
            if end < len(part) and part[end] != ' ':
                space = part.rfind(' ', 0, end + 1)
                if space > start:
                    end = space
            lines.append(part[start:end])
            start = end
            while start < len(part) and part[start] == ' ':
                start += 1
            if start >= len(part):
                break
    return lines


def c_string(literal):
    return bytes(literal, 'utf-8').decode('unicode_escape')


def main():
    src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'Main.cpp'), encoding='utf-8').read()
    start = src.index('kSettingHelp[SETTING_OPTIONS] = {')
    end = src.index('\n\t};', start)
    texts = re.findall(r'^\t\t"((?:[^"\\]|\\.)*)",?\s*$', src[start:end], re.M)
    bad = 0
    for i, t in enumerate(texts):
        lines = wrap(c_string(t), WIDTH)
        if len(lines) > LINES:
            bad += 1
            print('option %d: %d lines (cut after "%s")' % (i, len(lines), lines[LINES - 1][-30:]))
    print('%d help texts, %d too long (%d lines of %d characters fit)' % (len(texts), bad, LINES, WIDTH))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
