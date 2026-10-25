/*
This file is part of BearGram, a personal build of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class HistoryItem;

namespace Ui {
class PopupMenu;
class Show;
} // namespace Ui

namespace BearGram {

// "Читать полностью": the text of a long message in a separate window.
void AddReadFullAction(
	not_null<Ui::PopupMenu*> menu,
	not_null<HistoryItem*> item,
	std::shared_ptr<Ui::Show> show);

} // namespace BearGram
