/*
This file is part of BearGram, a personal build of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "beargram/beargram_read_full.h"

#include "api/api_transcribes.h"
#include "apiwrap.h"
#include "base/call_delayed.h"
#include "core/application.h"
#include "beargram/beargram_long_messages.h"
#include "data/data_groups.h"
#include "data/data_session.h"
#include "iv/iv_instance.h"
#include "window/window_session_controller.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "lang/lang_keys.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"

namespace BearGram {
namespace {

// Shorter texts are easy to read in the bubble.
constexpr auto kMinLength = 300;

// See AddReadFullAction, the menu closes first.
constexpr auto kOpenDelay = crl::time(200);

// In an album the text is kept by one of the items (the last one for
// files), a click on another file gave an empty text.
[[nodiscard]] not_null<HistoryItem*> TextItem(not_null<HistoryItem*> item) {
	if (item->richPage() || !item->originalText().text.isEmpty()) {
		return item;
	} else if (const auto group = item->history()->owner().groups().find(
			item)) {
		for (const auto &other : group->items) {
			if (other->richPage() || !other->originalText().text.isEmpty()) {
				return other;
			}
		}
	}
	return item;
}

} // namespace

void AddReadFullAction(
		not_null<Ui::PopupMenu*> menu,
		not_null<HistoryItem*> clicked,
		not_null<Window::SessionController*> controller) {
	const auto item = TextItem(clicked);
	// A message with the "Развернуть" toggle is long whatever the length.
	const auto collapsible = IsCollapsible(clicked);
	if (item->richPage()) {
		// The text of a rich message has its pictures as small icons only,
		// headings and lists don't count in its length.
		if (!collapsible
			&& item->originalText().text.size() < kMinLength) {
			return;
		}
		const auto weak = base::make_weak(controller);
		const auto fullId = item->fullId();
		menu->addAction(u"Читать полностью"_q, [=] {
			// Opened right from the menu the window went under the main
			// one: closing the menu gives the focus back to the chat.
			// So it opens a moment later, when the menu is gone.
			base::call_delayed(kOpenDelay, [=] {
				const auto strong = weak.get();
				if (!strong) {
					return;
				} else if (const auto found
						= strong->session().data().message(fullId)) {
					Core::App().iv().showRichMessage(strong, found);
				}
			});
		}, &st::menuIconFile);
		return;
	}
	const auto show = controller->uiShow();
	auto copy = item->originalText();
	// The speech text of a voice or round message, when it is shown.
	const auto &transcribe = item->history()->session().api().transcribes(
		).entry(item);
	if (transcribe.shown && !transcribe.result.isEmpty()) {
		auto speech = TextWithEntities{ transcribe.result };
		if (!copy.text.isEmpty()) {
			speech.append(u"\n\n"_q).append(std::move(copy));
		}
		copy = std::move(speech);
	}
	if (copy.text.isEmpty()
		|| (!collapsible && copy.text.size() < kMinLength)) {
		return;
	}
	menu->addAction(u"Читать полностью"_q, [=] {
		show->show(Box([=](not_null<Ui::GenericBox*> box) {
			box->setTitle(rpl::single(u"Читать полностью"_q));
			box->setWidth(st::boxWideWidth * 3 / 2);
			box->setMaxHeight(st::boxMaxListHeight * 3 / 2);
			const auto label = box->addRow(object_ptr<Ui::FlatLabel>(
				box,
				st::boxLabel));
			label->setMarkedText(copy);
			label->setSelectable(true);
			box->addButton(tr::lng_close(), [=] { box->closeBox(); });
		}));
	}, &st::menuIconFile);
}

} // namespace BearGram
