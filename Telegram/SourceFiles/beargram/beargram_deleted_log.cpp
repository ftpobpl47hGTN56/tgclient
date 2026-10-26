/*
This file is part of BearGram, a personal build of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "beargram/beargram_deleted_log.h"

#include "api/api_text_entities.h"
#include "base/unixtime.h"
#include "beargram/beargram_settings.h"
#include "core/application.h"
#include "data/data_types.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "settings.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/text/text_utilities.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/vertical_list.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"

#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

#include <deque>
#include <limits>
#include <map>
#include <optional>
#include <tuple>

namespace BearGram {
namespace {

// How many raw messages of logged chats are kept in memory, so that a full
// copy can be saved when one of them is deleted.
constexpr auto kRawCacheLimit = 40000;

struct EditVersion {
	TimeId date = 0;
	QString text;
};

struct DeletedEntry {
	TimeId deletedAt = 0;
	int layer = 0;
	QByteArray raw; // Serialized MTPMessage.
};

using RawKey = std::tuple<uint64, uint64, int64>; // self, peer, msg

struct State {
	bool loaded = false;
	base::flat_set<QString> selected;  // "<self>:<peer>"
	base::flat_set<QString> except;    // "<self>:<peer>"
	base::flat_map<QString, std::vector<EditVersion>> edits; // "<self>:<peer>:<msg>"
	base::flat_set<QString> kept;      // "<self>:<peer>:<msg>"

	// "<self>:<peer>" -> msg id -> saved copy. Saved to disk.
	std::map<QString, std::map<int64, DeletedEntry>> deleted;

	// Raw messages of logged chats received in this session. Memory only.
	std::map<RawKey, QByteArray> raw;
	std::deque<RawKey> rawOrder;
};

[[nodiscard]] QString LogPath() {
	return cWorkingDir() + u"tdata/beargram_log.json"_q;
}

[[nodiscard]] QString DeletedPath() {
	return cWorkingDir() + u"tdata/beargram_deleted.json"_q;
}

[[nodiscard]] QByteArray SerializeMessage(const MTPMessage &message) {
	auto buffer = mtpBuffer();
	message.write(buffer);
	return QByteArray(
		reinterpret_cast<const char*>(buffer.constData()),
		buffer.size() * sizeof(mtpPrime));
}

[[nodiscard]] std::optional<MTPMessage> ParseMessage(
		const QByteArray &bytes) {
	if (bytes.isEmpty() || (bytes.size() % sizeof(mtpPrime))) {
		return std::nullopt;
	}
	auto buffer = mtpBuffer();
	buffer.resize(bytes.size() / sizeof(mtpPrime));
	memcpy(buffer.data(), bytes.constData(), bytes.size());
	const mtpPrime *from = buffer.constData();
	const auto end = from + buffer.size();
	auto result = MTPMessage();
	if (!result.read(from, end) || from != end) {
		return std::nullopt;
	}
	return result;
}

[[nodiscard]] RawKey MakeRawKey(not_null<PeerData*> peer, MsgId id) {
	return {
		uint64(peer->session().userId().bare),
		uint64(peer->id.value),
		int64(id.bare),
	};
}

// A copy for messages that were loaded before logging was turned on for the
// chat: text, sender and date only, media can't be restored from it.
[[nodiscard]] MTPMessage BuildFallbackMessage(
		not_null<HistoryItem*> item) {
	using Flag = MTPDmessage::Flag;
	const auto history = item->history();
	auto text = item->originalText();
	if (item->media()) {
		auto prefix = TextWithEntities{
			u"[вложение не сохранено]"_q,
		};
		if (!text.text.isEmpty()) {
			prefix.append(u"\n"_q);
		}
		text = prefix.append(std::move(text));
	}
	const auto entities = Api::EntitiesToMTP(
		&history->session(),
		text.entities,
		Api::ConvertOption::SkipLocal);
	auto flags = MTPDmessage::Flags(0) | Flag::f_from_id;
	if (item->out()) {
		flags |= Flag::f_out;
	}
	if (item->isPost()) {
		flags |= Flag::f_post;
	}
	if (!entities.v.isEmpty()) {
		flags |= Flag::f_entities;
	}
	return MTP_message(
		MTP_flags(flags),
		MTP_int(int(item->id.bare)),
		peerToMTP(item->from()->id),
		MTPint(), // from_boosts_applied
		MTPstring(), // from_rank
		peerToMTP(history->peer->id),
		MTPPeer(), // saved_peer_id
		MTPMessageFwdHeader(),
		MTPlong(), // via_bot_id
		MTPlong(), // via_business_bot_id
		MTPPeer(), // guestchat_via_from
		MTPMessageReplyHeader(),
		MTP_int(item->date()),
		MTP_string(text.text),
		MTPMessageMedia(),
		MTPReplyMarkup(),
		entities,
		MTPint(), // views
		MTPint(), // forwards
		MTPMessageReplies(),
		MTPint(), // edit_date
		MTPstring(), // post_author
		MTPlong(), // grouped_id
		MTPMessageReactions(),
		MTPVector<MTPRestrictionReason>(),
		MTPint(), // ttl_period
		MTPint(), // quick_reply_shortcut_id
		MTPlong(), // effect
		MTPFactCheck(),
		MTPint(), // report_delivery_until_date
		MTPlong(), // paid_message_stars
		MTPSuggestedPost(),
		MTPint(), // schedule_repeat_period
		MTPstring(), // summary_from_language
		MTPRichMessage());
}

void LoadDeleted(State &state) {
	auto file = QFile(DeletedPath());
	if (!file.open(QIODevice::ReadOnly)) {
		return;
	}
	const auto object = QJsonDocument::fromJson(file.readAll()).object();
	const auto peers = object.value(u"peers"_q).toObject();
	for (auto i = peers.begin(); i != peers.end(); ++i) {
		const auto peerKey = i.key();
		const auto messages = i.value().toObject();
		for (auto j = messages.begin(); j != messages.end(); ++j) {
			const auto entry = j.value().toObject();
			const auto raw = QByteArray::fromBase64(
				entry.value(u"raw"_q).toString().toLatin1());
			if (raw.isEmpty()) {
				continue;
			}
			const auto msgId = j.key().toLongLong();
			state.deleted[peerKey][msgId] = DeletedEntry{
				.deletedAt = TimeId(entry.value(u"at"_q).toInteger()),
				.layer = int(entry.value(u"layer"_q).toInt()),
				.raw = raw,
			};
			state.kept.emplace(peerKey + ':' + QString::number(msgId));
		}
	}
}

[[nodiscard]] QString PeerKey(not_null<PeerData*> peer) {
	return QString::number(peer->session().userId().bare)
		+ ':'
		+ QString::number(peer->id.value);
}

[[nodiscard]] QString ItemKey(not_null<const HistoryItem*> item) {
	return PeerKey(item->history()->peer)
		+ ':'
		+ QString::number(item->id.bare);
}

void Load(State &state) {
	state.loaded = true;
	LoadDeleted(state);
	auto file = QFile(LogPath());
	if (!file.open(QIODevice::ReadOnly)) {
		return;
	}
	const auto object = QJsonDocument::fromJson(file.readAll()).object();
	for (const auto &value : object.value(u"selected"_q).toArray()) {
		state.selected.emplace(value.toString());
	}
	for (const auto &value : object.value(u"except"_q).toArray()) {
		state.except.emplace(value.toString());
	}
	const auto edits = object.value(u"edits"_q).toObject();
	for (auto i = edits.begin(); i != edits.end(); ++i) {
		auto &list = state.edits[i.key()];
		for (const auto &value : i.value().toArray()) {
			const auto entry = value.toObject();
			list.push_back({
				.date = TimeId(entry.value(u"date"_q).toInteger()),
				.text = entry.value(u"text"_q).toString(),
			});
		}
	}
}

[[nodiscard]] State &Instance() {
	static auto state = State();
	if (!state.loaded) {
		Load(state);
	}
	return state;
}

void Save() {
	const auto &state = Instance();
	auto selected = QJsonArray();
	for (const auto &key : state.selected) {
		selected.push_back(key);
	}
	auto except = QJsonArray();
	for (const auto &key : state.except) {
		except.push_back(key);
	}
	auto edits = QJsonObject();
	for (const auto &[key, list] : state.edits) {
		auto array = QJsonArray();
		for (const auto &version : list) {
			auto entry = QJsonObject();
			entry.insert(u"date"_q, qint64(version.date));
			entry.insert(u"text"_q, version.text);
			array.push_back(entry);
		}
		edits.insert(key, array);
	}
	auto object = QJsonObject();
	object.insert(u"selected"_q, selected);
	object.insert(u"except"_q, except);
	object.insert(u"edits"_q, edits);
	auto file = QFile(LogPath());
	if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
	}
}

void SaveDeleted() {
	const auto &state = Instance();
	auto peers = QJsonObject();
	for (const auto &[peerKey, messages] : state.deleted) {
		auto list = QJsonObject();
		for (const auto &[msgId, entry] : messages) {
			auto value = QJsonObject();
			value.insert(u"at"_q, qint64(entry.deletedAt));
			value.insert(u"layer"_q, entry.layer);
			value.insert(
				u"raw"_q,
				QString::fromLatin1(entry.raw.toBase64()));
			list.insert(QString::number(msgId), value);
		}
		if (!list.isEmpty()) {
			peers.insert(peerKey, list);
		}
	}
	auto object = QJsonObject();
	object.insert(u"version"_q, 1);
	object.insert(u"peers"_q, peers);
	auto file = QFile(DeletedPath());
	if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
	}
}

// Saves a full copy of the message that is about to be kept as deleted.
void SaveDeletedCopy(not_null<HistoryItem*> item) {
	auto &state = Instance();
	const auto peer = item->history()->peer;
	const auto i = state.raw.find(MakeRawKey(peer, item->id));
	const auto raw = (i != end(state.raw))
		? i->second
		: SerializeMessage(BuildFallbackMessage(item));
	state.deleted[PeerKey(peer)][int64(item->id.bare)] = DeletedEntry{
		.deletedAt = base::unixtime::now(),
		.layer = int(MTP::details::kCurrentLayer),
		.raw = raw,
	};
	SaveDeleted();
}

} // namespace

LogMode ModeFor(not_null<PeerData*> peer) {
	const auto &settings = Current();
	const auto mode = peer->isUser()
		? settings.logPrivateMode
		: settings.logGroupsMode;
	return static_cast<LogMode>(std::clamp(mode, 0, 2));
}

bool IsTracked(not_null<PeerData*> peer) {
	const auto &state = Instance();
	switch (ModeFor(peer)) {
	case LogMode::Selected:
		return state.selected.contains(PeerKey(peer));
	case LogMode::AllExcept:
		return !state.except.contains(PeerKey(peer));
	case LogMode::Off:
		return false;
	}
	return false;
}

bool ToggleTracked(not_null<PeerData*> peer) {
	auto &state = Instance();
	const auto key = PeerKey(peer);
	switch (ModeFor(peer)) {
	case LogMode::Selected:
		if (!state.selected.remove(key)) {
			state.selected.emplace(key);
		}
		break;
	case LogMode::AllExcept:
		if (!state.except.remove(key)) {
			state.except.emplace(key);
		}
		break;
	case LogMode::Off:
		return false;
	}
	Save();
	return IsTracked(peer);
}

QString TrackMenuText(not_null<PeerData*> peer) {
	return IsTracked(peer)
		? u"Сохранять удалённые: вкл"_q
		: u"Сохранять удалённые: выкл"_q;
}

bool KeepDeleted(not_null<HistoryItem*> item) {
	if (!item->isRegular() || !IsTracked(item->history()->peer)) {
		return false;
	}
	auto &state = Instance();
	const auto key = ItemKey(item);
	if (state.kept.contains(key)) {
		return true;
	}
	state.kept.emplace(key);
	SaveDeletedCopy(item);
	item->history()->owner().requestItemViewRefresh(item);
	return true;
}

void RememberRaw(not_null<PeerData*> peer, const MTPMessage &message) {
	if (message.type() != mtpc_message || ModeFor(peer) == LogMode::Off) {
		return;
	}
	auto &state = Instance();
	const auto key = MakeRawKey(peer, IdFromMessage(message));
	const auto i = state.raw.find(key);
	if (i != end(state.raw)) {
		i->second = SerializeMessage(message);
		return;
	}
	state.raw.emplace(key, SerializeMessage(message));
	state.rawOrder.push_back(key);
	while (state.rawOrder.size() > kRawCacheLimit) {
		state.raw.erase(state.rawOrder.front());
		state.rawOrder.pop_front();
	}
}

void InjectDeleted(
		not_null<History*> history,
		QVector<MTPMessage> &slice,
		bool older) {
	auto &state = Instance();
	if (state.deleted.empty()) {
		return;
	}
	const auto peer = history->peer;
	const auto i = state.deleted.find(PeerKey(peer));
	if (i == end(state.deleted) || i->second.empty()) {
		return;
	}

	// The range of ids that this slice fills in the history.
	constexpr auto kMax = std::numeric_limits<int64>::max();
	auto sliceMin = kMax;
	auto sliceMax = int64(0);
	auto present = base::flat_set<int64>();
	for (const auto &message : slice) {
		const auto id = int64(IdFromMessage(message).bare);
		present.emplace(id);
		sliceMin = std::min(sliceMin, id);
		sliceMax = std::max(sliceMax, id);
	}
	const auto empty = slice.isEmpty();
	const auto historyEmpty = history->isEmpty();
	auto from = int64(0);
	auto till = int64(-1); // Both inclusive.
	if (older) {
		// An empty older slice means the beginning of the chat was reached.
		from = empty ? 0 : sliceMin;
		till = !historyEmpty
			? (int64(history->minMsgId().bare) - 1)
			: history->loadedAtBottom()
			? kMax
			: (empty ? -1 : sliceMax);
	} else {
		// An empty newer slice means the end of the chat was reached.
		from = !historyEmpty
			? (int64(history->maxMsgId().bare) + 1)
			: (empty ? kMax : sliceMin);
		till = empty ? kMax : sliceMax;
	}
	if (from > till) {
		return;
	}

	auto added = false;
	for (auto j = i->second.lower_bound(from)
		; j != end(i->second) && j->first <= till
		; ++j) {
		if (present.contains(j->first)
			|| history->owner().message(peer, MsgId(j->first))) {
			continue;
		}
		auto message = ParseMessage(j->second.raw);
		if (!message || IdFromMessage(*message).bare != j->first) {
			LOG(("BearGram: could not restore a deleted message %1 "
				"(saved with layer %2)."
				).arg(j->first
				).arg(j->second.layer));
			continue;
		}
		slice.push_back(std::move(*message));
		added = true;
	}
	if (added) {
		// Slices come sorted from the newest message to the oldest.
		std::stable_sort(slice.begin(), slice.end(), [](
				const MTPMessage &a,
				const MTPMessage &b) {
			return IdFromMessage(a) > IdFromMessage(b);
		});
	}
}

bool IsKeptDeleted(not_null<const HistoryItem*> item) {
	const auto &state = Instance();
	return !state.kept.empty() && state.kept.contains(ItemKey(item));
}

void RecordEdit(not_null<HistoryItem*> item, const QString &newText) {
	if (!item->isRegular() || !IsTracked(item->history()->peer)) {
		return;
	}
	const auto oldText = item->originalText().text;
	if (oldText.isEmpty() || oldText == newText) {
		return;
	}
	Instance().edits[ItemKey(item)].push_back({
		.date = base::unixtime::now(),
		.text = oldText,
	});
	Save();
}

void AddEditHistoryAction(
		not_null<Ui::PopupMenu*> menu,
		not_null<HistoryItem*> item,
		std::shared_ptr<Ui::Show> show) {
	const auto &state = Instance();
	const auto i = state.edits.find(ItemKey(item));
	if (!show || i == end(state.edits) || i->second.empty()) {
		return;
	}
	const auto versions = i->second;
	const auto now = item->originalText().text;
	menu->addAction(u"История правок"_q, [=] {
		show->show(Box([=](not_null<Ui::GenericBox*> box) {
			box->setTitle(rpl::single(u"История правок"_q));
			box->setWidth(st::boxWideWidth * 3 / 2);
			box->setMaxHeight(st::boxMaxListHeight * 3 / 2);
			auto text = TextWithEntities();
			const auto addVersion = [&](const QString &title, const QString &body) {
				if (!text.text.isEmpty()) {
					text.append(u"\n\n"_q);
				}
				text.append(Ui::Text::Bold(title)).append(u"\n"_q).append(body);
			};
			for (const auto &version : versions) {
				addVersion(
					u"До "_q + langDateTime(base::unixtime::parse(version.date)),
					version.text);
			}
			addVersion(u"Сейчас"_q, now);
			const auto label = box->addRow(object_ptr<Ui::FlatLabel>(
				box,
				st::boxLabel));
			label->setMarkedText(text);
			label->setSelectable(true);
			box->addButton(tr::lng_close(), [=] { box->closeBox(); });
		}));
	}, &st::menuIconEdit);
}

void ForgetPeer(not_null<PeerData*> peer) {
	auto &state = Instance();
	const auto prefix = PeerKey(peer) + ':';
	auto changed = false;
	for (auto i = begin(state.edits); i != end(state.edits);) {
		if (i->first.startsWith(prefix)) {
			i = state.edits.erase(i);
			changed = true;
		} else {
			++i;
		}
	}
	for (auto i = begin(state.kept); i != end(state.kept);) {
		if (i->startsWith(prefix)) {
			i = state.kept.erase(i);
		} else {
			++i;
		}
	}
	if (changed) {
		Save();
	}
	if (state.deleted.erase(PeerKey(peer))) {
		SaveDeleted();
	}
}

void ClearAll() {
	auto &state = Instance();
	const auto kept = base::take(state.kept);
	state.edits.clear();
	state.deleted.clear();
	Save();
	SaveDeleted();
	for (const auto &[index, account] : Core::App().domain().accounts()) {
		if (!account->sessionExists()) {
			continue;
		}
		auto &session = account->session();
		const auto self = QString::number(session.userId().bare) + ':';
		auto toDestroy = std::vector<not_null<HistoryItem*>>();
		for (const auto &key : kept) {
			if (!key.startsWith(self)) {
				continue;
			}
			const auto parts = key.split(':');
			if (parts.size() != 3) {
				continue;
			}
			const auto peerId = PeerId(parts[1].toULongLong());
			const auto msgId = MsgId(parts[2].toLongLong());
			if (const auto item = session.data().message(peerId, msgId)) {
				toDestroy.push_back(item);
			}
		}
		for (const auto &item : toDestroy) {
			item->destroy();
		}
	}
}

} // namespace BearGram
