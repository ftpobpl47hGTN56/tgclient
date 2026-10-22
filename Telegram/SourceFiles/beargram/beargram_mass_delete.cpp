/*
This file is part of BearGram, a personal build of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "beargram/beargram_mass_delete.h"

#include "apiwrap.h"
#include "data/data_channel.h"
#include "data/data_histories.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_types.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/show.h"
#include "ui/widgets/popup_menu.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"

namespace BearGram {
namespace {

constexpr auto kSearchPerPage = 100;
constexpr auto kDeletePerRequest = 100;

struct OwnMessagesState {
	std::vector<MsgId> ids;
	MsgId offsetId = 0;
};

void DeleteCollected(
		not_null<History*> history,
		std::shared_ptr<Ui::Show> show,
		const std::vector<MsgId> &ids) {
	if (ids.empty()) {
		show->showToast(u"Ваших сообщений в этом чате не найдено."_q);
		return;
	}
	auto &histories = history->owner().histories();
	auto chunk = QVector<MTPint>();
	chunk.reserve(kDeletePerRequest);
	for (const auto id : ids) {
		chunk.push_back(MTP_int(id.bare));
		if (chunk.size() == kDeletePerRequest) {
			histories.deleteMessages(history, chunk, true);
			chunk.clear();
		}
	}
	if (!chunk.isEmpty()) {
		histories.deleteMessages(history, chunk, true);
	}
	show->showToast(u"Удаляю сообщений: %1"_q.arg(ids.size()));
}

void CollectOwnMessages(
		not_null<History*> history,
		std::shared_ptr<Ui::Show> show,
		std::shared_ptr<OwnMessagesState> state) {
	using Flag = MTPmessages_Search::Flag;
	const auto weak = base::make_weak(history);
	history->session().api().request(MTPmessages_Search(
		MTP_flags(Flag::f_from_id),
		history->peer->input(),
		MTP_string(),
		MTP_inputPeerSelf(),
		MTP_inputPeerEmpty(), // saved_peer_id
		MTPVector<MTPReaction>(), // saved_reaction
		MTP_int(0), // top_msg_id
		MTP_inputMessagesFilterEmpty(),
		MTP_int(0), // min_date
		MTP_int(0), // max_date
		MTP_int(state->offsetId.bare),
		MTP_int(0), // add_offset
		MTP_int(kSearchPerPage),
		MTP_int(0), // max_id
		MTP_int(0), // min_id
		MTP_long(0) // hash
	)).done([=](const MTPmessages_Messages &result) {
		const auto strong = weak.get();
		if (!strong) {
			return;
		}
		auto page = std::vector<MsgId>();
		const auto collect = [&](const QVector<MTPMessage> &list) {
			for (const auto &message : list) {
				if (const auto id = IdFromMessage(message)) {
					page.push_back(id);
				}
			}
		};
		result.match([&](const MTPDmessages_messages &data) {
			collect(data.vmessages().v);
		}, [&](const MTPDmessages_messagesSlice &data) {
			collect(data.vmessages().v);
		}, [&](const MTPDmessages_channelMessages &data) {
			collect(data.vmessages().v);
		}, [](const MTPDmessages_messagesNotModified &) {
		});
		const auto progress = int(state->ids.size());
		for (const auto id : page) {
			if (state->offsetId == 0 || id < state->offsetId) {
				state->ids.push_back(id);
			}
		}
		if (int(state->ids.size()) == progress) {
			DeleteCollected(strong, show, state->ids);
			return;
		}
		state->offsetId = state->ids.back();
		CollectOwnMessages(strong, show, state);
	}).fail([=] {
		show->showToast(u"Не удалось получить список сообщений."_q);
	}).send();
}

void ConfirmAndRun(
		std::shared_ptr<Ui::Show> show,
		const QString &text,
		Fn<void()> action) {
	show->show(Ui::MakeConfirmBox({
		.text = text,
		.confirmed = [=](Fn<void()> close) {
			close();
			action();
		},
		.confirmText = u"Удалить"_q,
		.confirmStyle = &st::attentionBoxButton,
	}));
}

} // namespace

void AddMassDeleteActions(
		not_null<Ui::PopupMenu*> menu,
		std::shared_ptr<Ui::Show> show,
		not_null<HistoryItem*> item) {
	const auto history = item->history();
	const auto peer = history->peer;
	if (!peer->isChat() && !peer->isMegagroup()) {
		return;
	}
	const auto from = item->from();
	if (from->isSelf()) {
		menu->addAction(u"Удалить все мои сообщения здесь"_q, [=] {
			ConfirmAndRun(
				show,
				u"Удалить ВСЕ Ваши сообщения в «%1»? "
				"Их нельзя будет восстановить."_q.arg(peer->name()),
				[=] {
					show->showToast(u"Ищу Ваши сообщения..."_q);
					CollectOwnMessages(
						history,
						show,
						std::make_shared<OwnMessagesState>());
				});
		}, &st::menuIconDelete);
		return;
	}
	const auto channel = peer->asMegagroup();
	if (!channel || !channel->canDeleteMessages()) {
		return;
	}
	menu->addAction(u"Удалить все сообщения от %1"_q.arg(
		from->shortName()
	), [=] {
		ConfirmAndRun(
			show,
			u"Удалить ВСЕ сообщения от «%1» в этой группе? "
			"Их нельзя будет восстановить."_q.arg(from->name()),
			[=] {
				channel->session().api().deleteAllFromParticipant(
					channel,
					from);
				show->showToast(u"Сообщения удаляются."_q);
			});
	}, &st::menuIconDelete);
}

} // namespace BearGram
