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
#include "ui/text/text_entity.h"
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
	base::flat_set<QString> usernames; // Lowercase, without '@'.
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
	state.usernames.clear();
	auto patterns = QStringList();
	for (const auto &rule : state.rules) {
		if (!rule.enabled) {
			continue;
		} else if (rule.type == RuleType::Peer) {
			state.peers.emplace(rule.peer);
			if (!rule.username.isEmpty()) {
				state.usernames.emplace(rule.username.toLower());
			}
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

[[nodiscard]] std::optional<Rule> RuleFromJson(const QJsonObject &entry) {
	auto rule = Rule();
	rule.enabled = entry.value(u"on"_q).toBool(true);
	if (entry.value(u"type"_q).toString() == u"peer"_q) {
		rule.type = RuleType::Peer;
		rule.peer = ParseId(entry.value(u"id"_q).toString());
		rule.text = entry.value(u"name"_q).toString();
		rule.username = entry.value(u"username"_q).toString();
		if (!rule.peer) {
			return std::nullopt;
		}
	} else {
		rule.type = RuleType::Word;
		rule.text = entry.value(u"text"_q).toString().trimmed();
		rule.wholeWord = entry.value(u"whole"_q).toBool(false);
		if (rule.text.isEmpty()) {
			return std::nullopt;
		}
	}
	return rule;
}

[[nodiscard]] QJsonObject RuleToJson(const Rule &rule) {
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
	return entry;
}

[[nodiscard]] QByteArray Serialize(const State &state) {
	auto rules = QJsonArray();
	for (const auto &rule : state.rules) {
		rules.push_back(RuleToJson(rule));
	}
	auto object = QJsonObject();
	object.insert(u"enabled"_q, state.enabled.current());
	object.insert(u"rules"_q, rules);
	return QJsonDocument(object).toJson(QJsonDocument::Indented);
}

void Load(State &state) {
	auto file = QFile(FilePath());
	if (!file.open(QIODevice::ReadOnly)) {
		return;
	}
	const auto object = QJsonDocument::fromJson(file.readAll()).object();
	state.enabled = object.value(u"enabled"_q).toBool(true);
	for (const auto value : object.value(u"rules"_q).toArray()) {
		if (auto rule = RuleFromJson(value.toObject())) {
			state.rules.push_back(std::move(*rule));
		}
	}
}

void Save(const State &state) {
	auto file = QFile(FilePath());
	if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		file.write(Serialize(state));
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

[[nodiscard]] bool SameRule(const Rule &a, const Rule &b) {
	if (a.type != b.type) {
		return false;
	} else if (a.type == RuleType::Peer) {
		return (a.peer == b.peer);
	}
	return (a.wholeWord == b.wholeWord)
		&& (Normalize(a.text) == Normalize(b.text));
}

[[nodiscard]] int FindRule(const State &state, const Rule &rule) {
	for (auto i = 0; i != int(state.rules.size()); ++i) {
		if (SameRule(state.rules[i], rule)) {
			return i;
		}
	}
	return -1;
}

// Adds without saving and recounting, so a batch costs one Changed().
bool Insert(State &state, Rule rule) {
	if (rule.type == RuleType::Word) {
		rule.text = rule.text.trimmed();
		if (rule.text.isEmpty()) {
			return false;
		}
	} else if (!rule.peer) {
		return false;
	}
	if (FindRule(state, rule) >= 0) {
		return false;
	}
	state.rules.push_back(std::move(rule));
	return true;
}

[[nodiscard]] bool HasMention(
		const State &state,
		const TextWithEntities &text) {
	if (!state.peers.empty()) {
		for (const auto &entity : text.entities) {
			if (entity.type() != EntityType::MentionName) {
				continue;
			}
			const auto fields = TextUtilities::MentionNameDataToFields(
				entity.data());
			if (fields.userId
				&& state.peers.contains(peerFromUser(UserId(fields.userId)))) {
				return true;
			}
		}
	}
	if (!state.usernames.empty() && text.text.contains('@')) {
		static const auto mention = QRegularExpression(
			u"@([A-Za-z0-9_]{4,32})"_q);
		auto i = mention.globalMatch(text.text);
		while (i.hasNext()) {
			const auto match = i.next();
			if (state.usernames.contains(match.captured(1).toLower())) {
				return true;
			}
		}
	}
	return false;
}

// Words and mentions of filtered people in the text, cached per message.
// Posts relayed by bots carry the author only as "@username" in the text,
// so a person rule matches such mentions too.
[[nodiscard]] bool MatchesText(
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
	const auto hidden = (state.hasWords
			&& Normalize(text.text).contains(state.words))
		|| HasMention(state, text);
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
	if (!Insert(Instance(), Rule{ .type = RuleType::Peer, .peer = peer })) {
		return false;
	}
	Changed();
	return true;
}

bool AddWord(const QString &word, bool wholeWord) {
	const auto rule = Rule{
		.type = RuleType::Word,
		.text = word,
		.wholeWord = wholeWord,
	};
	if (!Insert(Instance(), rule)) {
		return false;
	}
	Changed();
	return true;
}

bool AddRule(Rule rule) {
	if (!Insert(Instance(), std::move(rule))) {
		return false;
	}
	Changed();
	return true;
}

bool ReplaceRule(int index, Rule rule) {
	auto &state = Instance();
	if (index < 0 || index >= int(state.rules.size())) {
		return false;
	} else if (rule.type == RuleType::Word) {
		rule.text = rule.text.trimmed();
		if (rule.text.isEmpty()) {
			return false;
		}
	} else if (!rule.peer) {
		return false;
	}
	rule.enabled = state.rules[index].enabled;
	const auto existing = FindRule(state, rule);
	if (existing >= 0 && existing != index) {
		// Became the same as another rule: keep just that one.
		state.rules.erase(begin(state.rules) + index);
	} else {
		state.rules[index] = std::move(rule);
	}
	Changed();
	return true;
}

void ClearRules() {
	auto &state = Instance();
	if (state.rules.empty()) {
		return;
	}
	state.rules.clear();
	Changed();
}

QByteArray ExportJson() {
	return Serialize(Instance());
}

QByteArray ExportText() {
	auto lines = QStringList{
		u"// BearGram: фильтр постов. Одно правило в строке:"_q,
		u"// число - ID (1099189294, -100... для каналов), @username,"_q,
		u"// \"слово\" в кавычках - слово целиком, иначе любое вхождение."_q,
	};
	for (const auto &rule : Instance().rules) {
		if (rule.type == RuleType::Peer) {
			lines.push_back(DisplayId(rule.peer));
		} else if (rule.wholeWord) {
			lines.push_back('"' + rule.text + '"');
		} else {
			lines.push_back(rule.text);
		}
	}
	return lines.join('\n').toUtf8() + '\n';
}

ImportResult Import(const QByteArray &content) {
	auto result = ImportResult();
	auto &state = Instance();
	auto error = QJsonParseError();
	const auto document = QJsonDocument::fromJson(content, &error);
	if (error.error == QJsonParseError::NoError && document.isObject()) {
		const auto rules = document.object().value(u"rules"_q).toArray();
		for (const auto value : rules) {
			if (auto rule = RuleFromJson(value.toObject())) {
				if (Insert(state, std::move(*rule))) {
					++result.added;
				} else {
					++result.skipped;
				}
			}
		}
	} else {
		static const auto username = QRegularExpression(
			u"^@([A-Za-z0-9_]{4,32})$"_q);
		for (auto line : QString::fromUtf8(content).split('\n')) {
			line = line.trimmed();
			if (line.isEmpty() || line.startsWith(u"//"_q)) {
				continue;
			} else if (const auto m = username.match(line); m.hasMatch()) {
				result.usernames.push_back(m.captured(1));
				continue;
			}
			auto rule = Rule();
			if (const auto id = ParseId(line)) {
				rule = Rule{ .type = RuleType::Peer, .peer = id };
			} else if (line.size() > 2
				&& line.startsWith('"')
				&& line.endsWith('"')) {
				rule = Rule{
					.type = RuleType::Word,
					.text = line.mid(1, line.size() - 2),
					.wholeWord = true,
				};
			} else {
				rule = Rule{ .type = RuleType::Word, .text = line };
			}
			if (Insert(state, std::move(rule))) {
				++result.added;
			} else {
				++result.skipped;
			}
		}
	}
	if (result.added) {
		Changed();
	}
	return result;
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
	if (MatchesText(state, item)) {
		return true;
	}
	return textItem
		&& (textItem != item.get())
		&& MatchesText(state, not_null<const HistoryItem*>(textItem));
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

void AddSelectedTextAction(
		not_null<Ui::PopupMenu*> menu,
		const QString &selected) {
	constexpr auto kMaxLength = 128;
	constexpr auto kShownLength = 24;
	const auto text = selected.simplified();
	if (text.isEmpty() || text.size() > kMaxLength) {
		return;
	}
	const auto id = ParseId(text);
	const auto rule = id
		? Rule{ .type = RuleType::Peer, .peer = id }
		: Rule{ .type = RuleType::Word, .text = text };
	if (FindRule(Instance(), rule) >= 0) {
		return;
	}
	const auto shown = (text.size() > kShownLength)
		? (text.left(kShownLength - 1) + QChar(0x2026))
		: text;
	menu->addAction(
		(id
			? u"В фильтр постов: ID %1"_q
			: u"В фильтр постов: «%1»"_q).arg(shown),
		[=] {
			if (id) {
				AddPeerId(id);
			} else {
				AddWord(text, false);
			}
		},
		&st::menuIconTagFilter);
}

} // namespace BearGram::PostFilter
