/*
This file is part of BearGram, a personal build of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "beargram/beargram_post_filter.h"

#include "core/application.h"
#include "data/data_channel.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/view/history_view_element.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "settings.h"
#include "ui/widgets/popup_menu.h"
#include "styles/style_menu_icons.h"

#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>

namespace BearGram::PostFilter {
namespace {

constexpr auto kMaxCached = 16384;

struct Cached {
	int signature = 0;
	int version = 0;
	bool hidden = false;
};

struct State {
	rpl::variable<bool> enabled = true;
	std::vector<Rule> rules;
	rpl::event_stream<> changes;

	// Built from the enabled rules by Rebuild().
	base::flat_set<PeerId> peers;
	QRegularExpression words;
	bool hasWords = false;

	int version = 0;
	base::flat_map<FullMsgId, Cached> cache;
};

[[nodiscard]] QString FilePath() {
	return cWorkingDir() + u"tdata/beargram_post_filter.json"_q;
}

[[nodiscard]] QString Normalize(QString text) {
	return text.toLower().replace(QChar(0x0451), QChar(0x0435)); // ё -> е
}

[[nodiscard]] QString WordPattern(const Rule &rule) {
	const auto escaped = QRegularExpression::escape(Normalize(rule.text));
	return rule.wholeWord
		? (u"(?<![\\p{L}\\p{N}_])"_q + escaped + u"(?![\\p{L}\\p{N}_])"_q)
		: escaped;
}

void Rebuild(State &state) {
	state.peers.clear();
	auto patterns = QStringList();
	for (const auto &rule : state.rules) {
		if (!rule.enabled) {
			continue;
		} else if (rule.type == RuleType::Peer) {
			state.peers.emplace(rule.peer);
		} else if (!rule.text.isEmpty()) {
			patterns.push_back(WordPattern(rule));
		}
	}
	state.hasWords = !patterns.isEmpty();
	state.words = state.hasWords
		? QRegularExpression(
			patterns.join('|'),
			QRegularExpression::CaseInsensitiveOption
				| QRegularExpression::UseUnicodePropertiesOption)
		: QRegularExpression();
	if (state.hasWords && !state.words.isValid()) {
		state.hasWords = false;
	}
	++state.version;
	state.cache.clear();
}

void Load(State &state) {
	auto file = QFile(FilePath());
	if (!file.open(QIODevice::ReadOnly)) {
		return;
	}
	const auto object = QJsonDocument::fromJson(file.readAll()).object();
	state.enabled = object.value(u"enabled"_q).toBool(true);
	for (const auto value : object.value(u"rules"_q).toArray()) {
		const auto entry = value.toObject();
		auto rule = Rule();
		rule.enabled = entry.value(u"on"_q).toBool(true);
		if (entry.value(u"type"_q).toString() == u"peer"_q) {
			rule.type = RuleType::Peer;
			rule.peer = ParseId(entry.value(u"id"_q).toString());
			rule.text = entry.value(u"name"_q).toString();
			rule.username = entry.value(u"username"_q).toString();
			if (!rule.peer) {
				continue;
			}
		} else {
			rule.type = RuleType::Word;
			rule.text = entry.value(u"text"_q).toString().trimmed();
			rule.wholeWord = entry.value(u"whole"_q).toBool(false);
			if (rule.text.isEmpty()) {
				continue;
			}
		}
		state.rules.push_back(std::move(rule));
	}
}

void Save(const State &state) {
	auto rules = QJsonArray();
	for (const auto &rule : state.rules) {
		auto entry = QJsonObject();
		if (rule.type == RuleType::Peer) {
			entry.insert(u"type"_q, u"peer"_q);
			entry.insert(u"id"_q, DisplayId(rule.peer));
			entry.insert(u"name"_q, rule.text);
			entry.insert(u"username"_q, rule.username);
		} else {
			entry.insert(u"type"_q, u"word"_q);
			entry.insert(u"text"_q, rule.text);
			entry.insert(u"whole"_q, rule.wholeWord);
		}
		entry.insert(u"on"_q, rule.enabled);
		rules.push_back(entry);
	}
	auto object = QJsonObject();
	object.insert(u"enabled"_q, state.enabled.current());
	object.insert(u"rules"_q, rules);
	auto file = QFile(FilePath());
	if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
	}
}

[[nodiscard]] State &Instance() {
	static auto state = State();
	static const auto loaded = [] {
		Load(state);
		Rebuild(state);
		return true;
	}();
	(void)loaded;
	return state;
}

// Hidden messages have zero height, so every loaded message in every
// account is asked to recount its size after a change.
void RelayoutAll() {
	for (const auto &[index, account] : Core::App().domain().accounts()) {
		if (!account->sessionExists()) {
			continue;
		}
		auto &owner = account->session().data();
		const auto relayout = [&](not_null<PeerData*> peer) {
			const auto history = owner.historyLoaded(peer.get());
			if (!history) {
				return;
			}
			for (const auto &block : history->blocks) {
				for (const auto &view : block->messages) {
					owner.requestItemResize(view->data());
				}
			}
		};
		owner.enumerateGroups(relayout);
		owner.enumerateBroadcasts([&](not_null<ChannelData*> channel) {
			relayout(channel);
		});
	}
}

void Changed() {
	auto &state = Instance();
	Rebuild(state);
	Save(state);
	state.changes.fire({});
	RelayoutAll();
}

[[nodiscard]] QString PeerName(not_null<PeerData*> peer) {
	return peer->name();
}

[[nodiscard]] bool MatchesWords(
		State &state,
		not_null<const HistoryItem*> item) {
	const auto &text = item->originalText();
	if (text.text.isEmpty()) {
		return false;
	}
	const auto signature = int(text.text.size() * 31 + text.entities.size());
	const auto id = item->fullId();
	if (const auto i = state.cache.find(id); i != end(state.cache)) {
		if (i->second.signature == signature
			&& i->second.version == state.version) {
			return i->second.hidden;
		}
	}
	if (int(state.cache.size()) >= kMaxCached) {
		state.cache.clear();
	}
	const auto hidden = Normalize(text.text).contains(state.words);
	state.cache[id] = Cached{
		.signature = signature,
		.version = state.version,
		.hidden = hidden,
	};
	return hidden;
}

void AddSenderAction(
		not_null<Ui::PopupMenu*> menu,
		not_null<PeerData*> peer,
		const QString &label) {
	if (HasPeer(peer->id)) {
		return;
	}
	const auto weak = base::make_weak(&peer->session());
	const auto id = peer->id;
	menu->addAction(label.arg(peer->shortName()), [=] {
		if (const auto strong = weak.get()) {
			if (const auto found = strong->data().peerLoaded(id)) {
				AddPeer(found);
			}
		}
	}, &st::menuIconTagFilter);
}

} // namespace

bool Enabled() {
	return Instance().enabled.current();
}

rpl::producer<bool> EnabledValue() {
	return Instance().enabled.value();
}

void SetEnabled(bool enabled) {
	auto &state = Instance();
	if (state.enabled.current() == enabled) {
		return;
	}
	state.enabled = enabled;
	Changed();
}

const std::vector<Rule> &Rules() {
	return Instance().rules;
}

rpl::producer<> RulesChanges() {
	return Instance().changes.events();
}

bool HasPeer(PeerId peer) {
	const auto &rules = Instance().rules;
	return ranges::any_of(rules, [&](const Rule &rule) {
		return (rule.type == RuleType::Peer) && (rule.peer == peer);
	});
}

bool AddPeer(not_null<PeerData*> peer) {
	auto &state = Instance();
	for (auto &rule : state.rules) {
		if (rule.type == RuleType::Peer && rule.peer == peer->id) {
			// Already there: refresh who it is and switch it on.
			rule.text = PeerName(peer);
			rule.username = peer->username();
			rule.enabled = true;
			Changed();
			return false;
		}
	}
	state.rules.push_back(Rule{
		.type = RuleType::Peer,
		.peer = peer->id,
		.text = PeerName(peer),
		.username = peer->username(),
	});
	Changed();
	return true;
}

bool AddPeerId(PeerId peer) {
	if (!peer || HasPeer(peer)) {
		return false;
	}
	Instance().rules.push_back(Rule{
		.type = RuleType::Peer,
		.peer = peer,
	});
	Changed();
	return true;
}

bool AddWord(const QString &word, bool wholeWord) {
	const auto text = word.trimmed();
	if (text.isEmpty()) {
		return false;
	}
	auto &state = Instance();
	const auto normalized = Normalize(text);
	for (const auto &rule : state.rules) {
		if (rule.type == RuleType::Word
			&& rule.wholeWord == wholeWord
			&& Normalize(rule.text) == normalized) {
			return false;
		}
	}
	state.rules.push_back(Rule{
		.type = RuleType::Word,
		.text = text,
		.wholeWord = wholeWord,
	});
	Changed();
	return true;
}

void UpdatePeerInfo(not_null<PeerData*> peer) {
	auto &state = Instance();
	auto changed = false;
	for (auto &rule : state.rules) {
		if (rule.type != RuleType::Peer || rule.peer != peer->id) {
			continue;
		}
		const auto name = PeerName(peer);
		const auto username = peer->username();
		if (rule.text != name || rule.username != username) {
			rule.text = name;
			rule.username = username;
			changed = true;
		}
	}
	if (changed) {
		// Names are not used for matching, no need to recount messages.
		Save(state);
		state.changes.fire({});
	}
}

void SetRuleEnabled(int index, bool enabled) {
	auto &rules = Instance().rules;
	if (index < 0 || index >= int(rules.size())
		|| rules[index].enabled == enabled) {
		return;
	}
	rules[index].enabled = enabled;
	Changed();
}

void RemoveRule(int index) {
	auto &rules = Instance().rules;
	if (index < 0 || index >= int(rules.size())) {
		return;
	}
	rules.erase(begin(rules) + index);
	Changed();
}

QString DisplayId(PeerId peer) {
	if (const auto user = peerToUser(peer)) {
		return QString::number(user.bare);
	} else if (const auto channel = peerToChannel(peer)) {
		return u"-100"_q + QString::number(channel.bare);
	} else if (const auto chat = peerToChat(peer)) {
		return u"-"_q + QString::number(chat.bare);
	}
	return QString();
}

PeerId ParseId(const QString &text) {
	const auto value = text.trimmed();
	static const auto digits = QRegularExpression(u"^-?\\d{1,19}$"_q);
	if (!digits.match(value).hasMatch()) {
		return PeerId();
	}
	auto ok = false;
	if (value.startsWith(u"-100"_q) && value.size() > 4) {
		const auto bare = value.mid(4).toULongLong(&ok);
		return (ok && bare) ? peerFromChannel(ChannelId(bare)) : PeerId();
	} else if (value.startsWith('-')) {
		const auto bare = value.mid(1).toULongLong(&ok);
		return (ok && bare) ? peerFromChat(ChatId(bare)) : PeerId();
	}
	const auto bare = value.toULongLong(&ok);
	return (ok && bare) ? peerFromUser(UserId(bare)) : PeerId();
}

bool IsFilteredOut(
		not_null<const HistoryItem*> item,
		const HistoryItem *textItem) {
	auto &state = Instance();
	if (!state.enabled.current()
		|| (state.peers.empty() && !state.hasWords)) {
		return false;
	}
	const auto chat = item->history()->peer;
	if (chat->isUser()) {
		return false; // Private chats are never filtered.
	}
	if (!state.peers.empty()) {
		// The chat itself is not a "sender" here: a rule for a channel
		// hides its reposts in other chats, not the channel feed itself.
		const auto from = item->from()->id;
		if (from != chat->id && state.peers.contains(from)) {
			return true;
		}
		if (const auto original = item->originalSender()) {
			if (original->id != chat->id
				&& state.peers.contains(original->id)) {
				return true;
			}
		}
	}
	if (!state.hasWords) {
		return false;
	}
	if (MatchesWords(state, item)) {
		return true;
	}
	return textItem
		&& (textItem != item.get())
		&& MatchesWords(state, not_null<const HistoryItem*>(textItem));
}

void AddSenderActions(
		not_null<Ui::PopupMenu*> menu,
		not_null<HistoryItem*> item) {
	const auto chat = item->history()->peer;
	if (!item->isRegular() || item->out() || chat->isUser()) {
		return;
	}
	const auto from = item->from();
	if (from != chat) {
		AddSenderAction(menu, from, u"В фильтр постов: %1"_q);
	}
	if (const auto original = item->originalSender()) {
		if (original != from.get() && original != chat.get()) {
			AddSenderAction(
				menu,
				original,
				u"В фильтр постов: %1 (репост)"_q);
		}
	}
}

} // namespace BearGram::PostFilter
