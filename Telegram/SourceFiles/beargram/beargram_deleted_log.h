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
// not destroyed but kept with a "удалено" mark until the app is restarted
// (Telegram Desktop does not store message history on disk).
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

void AddEditHistoryAction(
	not_null<Ui::PopupMenu*> menu,
	not_null<HistoryItem*> item,
	std::shared_ptr<Ui::Show> show);

// The user cleared the history or deleted the chat.
void ForgetPeer(not_null<PeerData*> peer);

// Destroys all kept deleted messages and erases the edit history.
void ClearAll();

} // namespace BearGram
