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
- Rich messages (headings, lists; laid out by the article engine in
  `HistoryMessageRichPage`) are cut by height with the same thresholds, a
  fade and the same toggle (`ComputeCollapsedRich`, `Message::collapsedRich`).
- Filters: messages from blocked users in groups and channel posts marked as
  ads (`erid`, `#реклама`, "Реклама ... ИНН", a closing "Реклама" line) are
  hidden. Own messages never are.
- Channel posts with comments turned off are hidden when the channel has a
  discussion group (pinned posts are kept).
- Delete locally (only in this session, back after restart): "Удалить у себя"
  for one message, "Удалить выбранные у себя" for a selection, "Удалить у себя
  всё от ..." for a sender in groups and channels' comments.

- Optional hiding of Telegram sponsored messages (`SponsoredMessages::canHaveFor`).
- Account limit 1..15 as a slider (`Settings::maxAccounts`, `Domain::maxAccounts`).
- "Читать полностью" for long messages (`beargram_read_full`).
- Local names of users (`beargram_nicknames`, `UserData::setName`,
  `PeerData::shortName`), menu item "Локальное имя" in chat and profile menus.
- Deleted messages logger (`beargram_deleted_log`): server deletions in tracked
  chats keep the items with an "удалено" mark (`Data::Session::processMessagesDeleted`,
  `BottomInfo`). A full copy of each kept message is saved in
  `tdata/beargram_deleted.json` and put back into the history slices from the
  server after a restart (`History::addOlderSlice` / `addNewerSlice`); edits keep the previous text in
  `tdata/beargram_log.json` ("История правок"). Modes and a clear button in
  the BearGram box, a per-chat switch in the chat menu.
- Local chat wallpapers (`beargram_wallpapers`): chat menu > "Мои обои для чата"
  for any chat, groups and channels too; the picture is copied to
  `tdata/beargram_wallpapers` and its chat theme replaces the server one in
  `Window::ChatThemeValueFromPeer`. Nothing is sent to the server.
- Search field in Settings > Privacy > Blocked users (loads the whole list).
- "Report" for users in private chats: in the profile / chat menu and on
  single messages (`Filler::addReport`, `HistoryItem::suggestReport`).
- Up to 15 accounts without Telegram Premium (`Main::Domain::kPremiumMaxAccounts`).
- Account transfer (`beargram_account_transfer`): Settings > BearGram >
  Перенос аккаунтов exports every logged in account (its MTP authorization:
  user id, main DC, keys) into a `.bgaccounts` file encrypted with
  AES-256-GCM and a password (PBKDF2-SHA256), and imports such a file into
  another desktop BearGram as new accounts (`Main::Domain::addWithAuthorization`),
  also from the welcome screen. "Загрузить чаты заново" restarts the app.
- Speech text of voice and round messages is collapsed like long messages,
  and "Читать полностью" shows it (`history_view_document.cpp`).
- "Выделить всё" in the selection bar of a chat selects the loaded messages
  from the newest up to 100 (`HistoryInner::selectAllLoaded`).

## Configure (Windows, x64)

Run from `E:\TBuild\tdesktop\Telegram` in the build terminal:

```bat
configure.bat x64 -D TDESKTOP_API_ID=YOUR_API_ID -D TDESKTOP_API_HASH=YOUR_API_HASH -D DESKTOP_APP_DISABLE_AUTOUPDATE=ON
```

`DESKTOP_APP_DISABLE_AUTOUPDATE=ON` is required: otherwise the built-in updater
would download the official Telegram Desktop and replace BearGram with it.

Then open `out\Telegram.slnx` in Visual Studio 2026 and build the `Telegram`
project. The result is `out\Debug\BearGram.exe` (or `out\Release\BearGram.exe`).

## Updates from Telegram Desktop and cloud builds

- Update check in the client (`beargram/beargram_updates`): shortly after the
  start and then every 6 hours BearGram reads the latest GitHub release
  (`beargram-<version>-build<N>`) and shows "Обновить BearGram (...)" at the
  bottom of the chats list; a click opens the release page. Cloud builds get
  their number as `BEARGRAM_BUILD_NUMBER`, so a newer build of the same
  version is reported too; a local build only reports a newer version.
  Settings > BearGram > Обновления: automatic checks on/off, "Проверить сейчас".

- `.github/workflows/beargram-sync.yml` runs daily: when a new stable Telegram
  Desktop release (`vX.Y.Z`) is out, `beargram/sync_upstream.sh` merges it into
  `beargram`. Conflicts in `core/version.h` and `winrc/Telegram.rc` are solved
  by taking the upstream file and running `beargram/branding.py`; upstream
  workflows are removed again. Any other conflict fails the run and nothing is
  pushed. Needs the secret `BEARGRAM_PAT` (a token with Contents and Workflows
  write access), because upstream commits change `.github/workflows`.
- `.github/workflows/beargram-windows.yml` builds `BearGram.exe` (Release, x64)
  on every push to `beargram` and publishes it in Releases as
  `BearGram-<version>-x64.zip`. Own API keys go to the secrets
  `TDESKTOP_API_ID` and `TDESKTOP_API_HASH`.
- The same merge by hand: `bash beargram/sync_upstream.sh` (Git Bash).
