/*
This file is part of BearGram, a personal build of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class History;
class HistoryItem;
class PeerData;

namespace Ui {
class PopupMenu;
class Show;
} // namespace Ui

namespace BearGram {

// Logger of deleted and edited messages.
//
// Deleted: when the server deletes messages in a tracked chat, the items are
// not destroyed but kept with a "удалено" mark. Telegram Desktop does not
// store message history on disk and loads it from the server again after a
// restart, so a full copy of each kept message (the serialized MTPMessage)
// is saved in tdata/beargram_deleted.json and put back into the history
// slices received from the server (see InjectDeleted).
// Edited: the previous text is saved in tdata/beargram_log.json and shown by
// "История правок" in the message menu.
//
// Which chats are tracked: a mode for private chats and one for groups and
// channels (Settings::logPrivateMode / logGroupsMode), plus per-chat lists.

enum class LogMode {
	Off = 0,
	Selected = 1,
	AllExcept = 2,
};

[[nodiscard]] bool IsTracked(not_null<PeerData*> peer);
[[nodiscard]] LogMode ModeFor(not_null<PeerData*> peer);
bool ToggleTracked(not_null<PeerData*> peer);
[[nodiscard]] QString TrackMenuText(not_null<PeerData*> peer);

// Returns true if the item is kept (then it must not be destroyed).
bool KeepDeleted(not_null<HistoryItem*> item);
[[nodiscard]] bool IsKeptDeleted(not_null<const HistoryItem*> item);

// Call before an edit from the server is applied.
void RecordEdit(not_null<HistoryItem*> item, const QString &newText);

// Remembers the raw message as it came from the server, so that a full copy
// can be saved if the message gets deleted. Kept in memory only.
void RememberRaw(not_null<PeerData*> peer, const MTPMessage &message);

// Adds the saved deleted messages that belong to the range of this slice.
// older == true for History::addOlderSlice, false for addNewerSlice.
// Must be called before the slice is added to the history.
void InjectDeleted(
	not_null<History*> history,
	QVector<MTPMessage> &slice,
	bool older);

void AddEditHistoryAction(
	not_null<Ui::PopupMenu*> menu,
	not_null<HistoryItem*> item,
	std::shared_ptr<Ui::Show> show);

// The user cleared the history or deleted the chat.
void ForgetPeer(not_null<PeerData*> peer);

// Destroys all kept deleted messages and erases the edit history.
void ClearAll();

} // namespace BearGram
