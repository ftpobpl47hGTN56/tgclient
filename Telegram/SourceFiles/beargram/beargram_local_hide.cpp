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
#include "ui/widgets/popup_menu.h"
#include "styles/style_menu_icons.h"

namespace BearGram {
namespace {

struct HiddenState {
	base::flat_set<FullMsgId> messages;
	base::flat_set<PeerId> senders;
};

[[nodiscard]] HiddenState &State() {
	static auto state = HiddenState();
	return state;
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
	menu->addAction(u"Удалить у себя"_q, [=] {
		State().messages.emplace(id);
		if (const auto found = session->data().message(id)) {
			session->data().requestItemResize(found);
		}
	}, &st::menuIconDelete);

	const auto history = item->history();
	const auto from = item->from();
	if (item->out()
		|| from == history->peer
		|| history->peer->isUser()) {
		return;
	}
	const auto sender = from->id;
	const auto peerId = history->peer->id;
	menu->addAction(u"Удалить у себя всё от %1"_q.arg(
		from->shortName()
	), [=] {
		State().senders.emplace(sender);
		if (const auto loaded = session->data().historyLoaded(peerId)) {
			RelayoutSender(loaded, sender);
		}
	}, &st::menuIconDelete);
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
	menu->addAction(u"Удалить выбранные у себя (%1)"_q.arg(ids.size()), [=] {
		auto &state = State();
		for (const auto &id : ids) {
			state.messages.emplace(id);
		}
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
	}, &st::menuIconDelete);
}

} // namespace BearGram
