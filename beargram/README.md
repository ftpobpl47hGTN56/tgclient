# BearGram

Personal Telegram Desktop build with its own name, icon and data folder.

## What differs from upstream

- App name `BearGram`, executable `BearGram.exe` (`Telegram/SourceFiles/core/version.h`,
  `Telegram/CMakeLists.txt`, `Telegram/Resources/winrc/Telegram.rc`).
- Own app id, Start menu shortcut and notification id, so BearGram and the
  official Telegram Desktop can be installed and run side by side.
- Data lives in a `tdata` folder next to `BearGram.exe`, or in
  `%APPDATA%\BearGram` when that folder is not writable.
- Icons in `Telegram/Resources/art` are generated from `beargram/icon_master.png`
  with `python beargram/make_icons.py`.
- UI texts are not changed: the Russian and other interface languages are
  downloaded from Telegram servers, so editing `lang.strings` would only affect English.

## Features

All BearGram code lives in `Telegram/SourceFiles/beargram`. Settings open from
Settings > BearGram and are stored in `tdata/beargram.json`.

- Long messages: text longer than N lines (10-50, default 20) is collapsed to
  a short preview with a "Развернуть" link and can be collapsed back.
- Filters: messages from blocked users in groups and channel posts marked as
  ads (`erid`, `#реклама`, "Реклама ... ИНН", a closing "Реклама" line) are
  hidden. Own messages never are.
- Channel posts with comments turned off are hidden when the channel has a
  discussion group (pinned posts are kept).
- Delete locally (only in this session, back after restart): "Удалить у себя"
  for one message, "Удалить выбранные у себя" for a selection, "Удалить у себя
  всё от ..." for a sender in groups and channels' comments.

- Search field in Settings > Privacy > Blocked users (loads the whole list).
- Up to 15 accounts without Telegram Premium (`Main::Domain::kPremiumMaxAccounts`).

## Configure (Windows, x64)

Run from `E:\TBuild\tdesktop\Telegram` in the build terminal:

```bat
configure.bat x64 -D TDESKTOP_API_ID=YOUR_API_ID -D TDESKTOP_API_HASH=YOUR_API_HASH -D DESKTOP_APP_DISABLE_AUTOUPDATE=ON
```

`DESKTOP_APP_DISABLE_AUTOUPDATE=ON` is required: otherwise the built-in updater
would download the official Telegram Desktop and replace BearGram with it.

Then open `out\Telegram.slnx` in Visual Studio 2026 and build the `Telegram`
project. The result is `out\Debug\BearGram.exe` (or `out\Release\BearGram.exe`).
