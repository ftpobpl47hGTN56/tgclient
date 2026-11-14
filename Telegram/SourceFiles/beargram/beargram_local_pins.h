/*
This file is part of BearGram, a personal build of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class History;

namespace Main {
class Session;
} // namespace Main

namespace Data {
class Folder;
} // namespace Data

namespace Dialogs {
class Key;
} // namespace Dialogs

// Pinned chats without the server limit (5, 10 with Premium).
//
// Every chat pinned on this device is kept here as a "local" pin, whatever
// the server answers (it keeps only its limit and may not say so):
// - the server lists of pinned chats (startup, other devices) are merged
//   with the local pins instead of replacing them;
// - the order the chats were dragged to is kept on this device, the server
//   gets the top chats within its limit;
// - unpinning a chat here removes it.
//
// Kept per account in the BearGram settings file tdata/beargram.json under
// "pins", with the access hashes of the chats, so a chat far down the list
// is loaded and pinned right after the start.
namespace BearGram::LocalPins {

// The pinned list limit in the main list and in the archive.
inline constexpr auto kLimit = 500;

[[nodiscard]] bool IsLocal(not_null<History*> history);
[[nodiscard]] bool IsLocalKey(const Dialogs::Key &key);

void AddLocal(not_null<History*> history);
void RemoveLocal(not_null<History*> history);

// The user changed the pinned chats or their order in the folder list.
void RememberOrder(not_null<Main::Session*> session, Data::Folder *folder);

// Puts the local pins and the remembered order back a moment later, after
// the server list (or a part of the chats list) is applied.
void ScheduleMerge(not_null<Main::Session*> session);

} // namespace BearGram::LocalPins
