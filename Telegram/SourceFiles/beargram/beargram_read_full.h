/*
This file is part of BearGram, a personal build of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class HistoryItem;

namespace Ui {
class PopupMenu;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace BearGram {

// "Читать полностью": the text of a long message in a separate window.
// A rich message (headings, lists, pictures) opens in the Instant View
// window, where its pictures are shown too.
void AddReadFullAction(
	not_null<Ui::PopupMenu*> menu,
	not_null<HistoryItem*> item,
	not_null<Window::SessionController*> controller);

} // namespace BearGram
