/*
This file is part of BearGram, a personal build of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "beargram/beargram_deleted_log.h"

#include "base/unixtime.h"
#include "beargram/beargram_settings.h"
#include "core/application.h"
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

namespace BearGram {
namespace {

struct EditVersion {
	TimeId date = 0;
	QString text;
};

struct State {
	bool loaded = false;
	base::flat_set<QString> selected;  // "<self>:<peer>"
	base::flat_set<QString> except;    // "<self>:<peer>"
	base::flat_map<QString, std::vector<EditVersion>> edits; // "<self>:<peer>:<msg>"
	base::flat_set<QString> kept;      // "<self>:<peer>:<msg>"
};

[[nodiscard]] QString LogPath() {
	return cWorkingDir() + u"tdata/beargram_log.json"_q;
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
	item->history()->owner().requestItemViewRefresh(item);
	return true;
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
}

void ClearAll() {
	auto &state = Instance();
	const auto kept = base::take(state.kept);
	state.edits.clear();
	Save();
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
