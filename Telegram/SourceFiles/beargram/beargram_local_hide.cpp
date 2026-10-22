/*
This file is part of BearGram, a personal build of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "beargram/beargram_local_hide.h"

#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/view/history_view_element.h"
#include "main/main_session.h"
#include "settings.h"
#include "ui/widgets/popup_menu.h"
#include "styles/style_menu_icons.h"

#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

namespace BearGram {
namespace {

struct HiddenState {
	base::flat_set<FullMsgId> messages;
	base::flat_set<PeerId> senders;
};

[[nodiscard]] QString HiddenPath() {
	return cWorkingDir() + u"tdata/beargram_hidden.json"_q;
}

[[nodiscard]] PeerId ParsePeer(const QString &value) {
	return PeerId(PeerIdHelper(value.toULongLong()));
}

[[nodiscard]] HiddenState Load() {
	auto result = HiddenState();
	auto file = QFile(HiddenPath());
	if (!file.open(QIODevice::ReadOnly)) {
		return result;
	}
	const auto object = QJsonDocument::fromJson(file.readAll()).object();
	for (const auto &value : object.value(u"messages"_q).toArray()) {
		const auto parts = value.toString().split('_');
		if (parts.size() == 2) {
			result.messages.emplace(
				ParsePeer(parts[0]),
				MsgId(parts[1].toLongLong()));
		}
	}
	for (const auto &value : object.value(u"senders"_q).toArray()) {
		if (const auto peer = ParsePeer(value.toString())) {
			result.senders.emplace(peer);
		}
	}
	return result;
}

[[nodiscard]] HiddenState &State() {
	static auto state = Load();
	return state;
}

void Save() {
	const auto &state = State();
	auto messages = QJsonArray();
	for (const auto &id : state.messages) {
		messages.push_back(u"%1_%2"_q
			.arg(id.peer.value)
			.arg(id.msg.bare));
	}
	auto senders = QJsonArray();
	for (const auto &peer : state.senders) {
		senders.push_back(QString::number(peer.value));
	}
	auto object = QJsonObject();
	object.insert(u"messages"_q, messages);
	object.insert(u"senders"_q, senders);
	auto file = QFile(HiddenPath());
	if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
	}
}

void RelayoutSender(not_null<History*> history, PeerId sender) {
	auto &owner = history->owner();
	for (const auto &block : history->blocks) {
		for (const auto &view : block->messages) {
			const auto item = view->data();
			if (item->from()->id == sender) {
				owner.requestItemResize(item);
			}
		}
	}
}

} // namespace

bool IsHiddenLocally(not_null<const HistoryItem*> item) {
	const auto &state = State();
	if (!state.messages.empty() && state.messages.contains(item->fullId())) {
		return true;
	} else if (state.senders.empty() || item->out()) {
		return false;
	}
	const auto from = item->from()->id;
	return (from != item->history()->peer->id)
		&& state.senders.contains(from);
}

void AddLocalHideActions(
		not_null<Ui::PopupMenu*> menu,
		not_null<HistoryItem*> item) {
	if (!item->isRegular()) {
		return;
	}
	const auto session = &item->history()->session();
	const auto id = item->fullId();
	menu->addAction(u"Скрыть у себя"_q, [=] {
		State().messages.emplace(id);
		Save();
		if (const auto found = session->data().message(id)) {
			session->data().requestItemResize(found);
		}
	}, &st::menuIconStealth);

	const auto history = item->history();
	const auto from = item->from();
	if (item->out()
		|| from == history->peer
		|| history->peer->isUser()) {
		return;
	}
	const auto sender = from->id;
	const auto peerId = history->peer->id;
	menu->addAction(u"Скрыть все сообщения от %1"_q.arg(
		from->shortName()
	), [=] {
		State().senders.emplace(sender);
		Save();
		if (const auto loaded = session->data().historyLoaded(peerId)) {
			RelayoutSender(loaded, sender);
		}
	}, &st::menuIconStealth);
}

void AddHideSelectedAction(
		not_null<Ui::PopupMenu*> menu,
		not_null<Main::Session*> session,
		MessageIdsList ids,
		Fn<void()> clearSelection) {
	if (ids.empty()) {
		return;
	}
	const auto weak = base::make_weak(session);
	menu->addAction(u"Скрыть выбранные у себя (%1)"_q.arg(ids.size()), [=] {
		auto &state = State();
		for (const auto &id : ids) {
			state.messages.emplace(id);
		}
		Save();
		if (clearSelection) {
			clearSelection();
		}
		if (const auto strong = weak.get()) {
			for (const auto &id : ids) {
				if (const auto found = strong->data().message(id)) {
					strong->data().requestItemResize(found);
				}
			}
		}
	}, &st::menuIconStealth);
}

int HiddenMessagesCount() {
	return int(State().messages.size());
}

int HiddenSendersCount() {
	return int(State().senders.size());
}

void ShowAllHiddenMessages() {
	State().messages.clear();
	Save();
}

void ShowAllHiddenSenders() {
	State().senders.clear();
	Save();
}

} // namespace BearGram
