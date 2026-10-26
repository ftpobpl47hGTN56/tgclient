#!/usr/bin/env python3
"""Applies the BearGram name and app id on top of the upstream files.

Upstream changes the version lines in the same places where BearGram keeps
its name, so every merge of a new Telegram Desktop release conflicts there.
The sync script takes the upstream version of these files and runs this
script to put the BearGram branding back. Running it twice changes nothing.

Usage: python3 beargram/branding.py [repository root]
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else '.')

VERSION_H = ROOT / 'Telegram/SourceFiles/core/version.h'
TELEGRAM_RC = ROOT / 'Telegram/Resources/winrc/Telegram.rc'

VERSION_REPLACEMENTS = [
    (r'^constexpr auto AppId = .*$',
     'constexpr auto AppId = "{E1224481-783C-4424-8800-B4E601026B2B}"_cs; // BearGram'),
    (r'^constexpr auto AppNameOld = .*$',
     'constexpr auto AppNameOld = "BearGram (Old)"_cs;'),
    (r'^constexpr auto AppName = .*$',
     'constexpr auto AppName = "BearGram"_cs;'),
    (r'^constexpr auto AppFile = .*$',
     'constexpr auto AppFile = "BearGram"_cs;'),
]

RC_REPLACEMENTS = [
    (r'VALUE "CompanyName", "[^"]*"', 'VALUE "CompanyName", "BearGram"'),
    (r'VALUE "FileDescription", "[^"]*"', 'VALUE "FileDescription", "BearGram"'),
    (r'VALUE "ProductName", "[^"]*"', 'VALUE "ProductName", "BearGram"'),
]


def apply(path, replacements):
    text = path.read_text(encoding='utf-8')
    result = text
    for pattern, replacement in replacements:
        result, count = re.subn(pattern, replacement, result, flags=re.M)
        if count != 1:
            sys.exit(f'branding.py: expected one match of {pattern!r} '
                     f'in {path}, found {count}')
    if result != text:
        path.write_text(result, encoding='utf-8')
        print(f'branding.py: updated {path}')


def main():
    apply(VERSION_H, VERSION_REPLACEMENTS)
    apply(TELEGRAM_RC, RC_REPLACEMENTS)


if __name__ == '__main__':
    main()
