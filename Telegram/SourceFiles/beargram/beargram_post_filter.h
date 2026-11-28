/*
This file is part of BearGram, a personal build of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "data/data_peer_id.h"

class HistoryItem;
class PeerData;

namespace Main {
class Session;
} // namespace Main

namespace Ui {
class GenericBox;
class PopupMenu;
} // namespace Ui

// Post filter: hides posts in groups and channels by sender id or by words.
//
// One list of rules and one on/off switch for the whole app: every account
// uses the same rules (like the sound settings). Stored in
// tdata/beargram_post_filter.json, never sent anywhere.
//
// Senders are matched by id, so a rule keeps working after the person
// changes the username or the name. The name and the username are kept
// only to show who is who in the list.
namespace BearGram::PostFilter {

enum class RuleType {
	Peer,
	Word,
};

struct Rule {
	RuleType type = RuleType::Word;
	PeerId peer = 0; // RuleType::Peer.
	QString text; // Word for RuleType::Word, last known name for Peer.
	QString username; // Last known username for RuleType::Peer.
	bool wholeWord = false; // RuleType::Word.
	bool enabled = true;
	TimeId created = 0; // Unix time, 0 in rules made before this.
};

[[nodiscard]] bool Enabled();
[[nodiscard]] rpl::producer<bool> EnabledValue();
void SetEnabled(bool enabled);

struct FilterGroupsOpen {
	bool channels = true;
	bool users = true;
	bool words = true;
};

// Folded state of the groups in the filter box, kept between openings.
[[nodiscard]] FilterGroupsOpen GroupsOpen();
void SetGroupsOpen(FilterGroupsOpen open);

[[nodiscard]] const std::vector<Rule> &Rules();
[[nodiscard]] rpl::producer<> RulesChanges();
[[nodiscard]] bool HasPeer(PeerId peer);

// All of them return false if such a rule already exists.
bool AddPeer(not_null<PeerData*> peer);
bool AddPeerId(PeerId peer);
bool AddWord(const QString &word, bool wholeWord);
bool AddRule(Rule rule);

// Stores the current name and username of a filtered chat (display only).
void UpdatePeerInfo(not_null<PeerData*> peer);

void SetRuleEnabled(int index, bool enabled);
void RemoveRule(int index);
// Removes several rules with a single save, to delete a selection.
void RemoveRuleIndices(const std::vector<int> &indices);
void ClearRules();

// Puts a new rule in place of the one at index, keeping its on/off state.
bool ReplaceRule(int index, Rule rule);

// Export as JSON: the rules, the local pins of every account and, when
// asked, the local nicknames. Plain text is one rule per line. Import
// takes either, merging rules into the current ones; the pins and the
// nicknames in the file replace the stored ones.
[[nodiscard]] QByteArray ExportJson(bool nicknames = false);
[[nodiscard]] QByteArray ExportText();
struct ImportResult {
	int added = 0;
	int skipped = 0; // Already there.
	QStringList usernames; // "@name" lines: need the API to find the id.
	int pins = 0; // Accounts in the "pins" group.
	int nicknames = 0; // Nicknames in the "nicknames" group.
};
ImportResult Import(const QByteArray &content);

// The id as people see it: 1099189294 for a user,
// -1001234567890 for a channel or a supergroup, -123456 for a basic group.
[[nodiscard]] QString DisplayId(PeerId peer);
// Accepts the same forms as DisplayId() returns, 0 if it is not an id.
[[nodiscard]] PeerId ParseId(const QString &text);

[[nodiscard]] bool IsFilteredOut(
	not_null<const HistoryItem*> item,
	const HistoryItem *textItem);

// "Add the sender to the filter" items for the message context menu.
void AddSenderActions(
	not_null<Ui::PopupMenu*> menu,
	not_null<HistoryItem*> item);

// "Add to the post filter" for the selected text.
void AddSelectedTextAction(
	not_null<Ui::PopupMenu*> menu,
	const QString &selected);

void FilterBox(
	not_null<Ui::GenericBox*> box,
	not_null<Main::Session*> session);

} // namespace BearGram::PostFilter
