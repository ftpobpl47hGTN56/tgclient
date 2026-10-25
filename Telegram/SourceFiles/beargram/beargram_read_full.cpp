/*
This file is part of BearGram, a personal build of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "beargram/beargram_read_full.h"

#include "history/history_item.h"
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

} // namespace

void AddReadFullAction(
		not_null<Ui::PopupMenu*> menu,
		not_null<HistoryItem*> item,
		std::shared_ptr<Ui::Show> show) {
	const auto &text = item->originalText();
	if (!show || text.text.size() < kMinLength) {
		return;
	}
	const auto copy = text;
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
