/*
This file is part of BearGram, a personal build of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class History;
class QPainter;

namespace Main {
class Session;
} // namespace Main

// Several chats selected in the chats list ("Выделить" in the chat menu),
// the menu of a selected chat then acts on all of them: pin, mute, read,
// archive, leave, delete. One account at a time, nothing is saved.
namespace BearGram::ChatSelection {

[[nodiscard]] bool Contains(not_null<History*> history);
[[nodiscard]] int Count();
[[nodiscard]] std::vector<not_null<History*>> List();

void Toggle(not_null<History*> history);
void Clear();

// Fired on every change, the chats list repaints the marks.
[[nodiscard]] rpl::producer<> Changes();

// The mark of a row while chats are selected: a selected row gets a tint
// and a check on the userpic, the others an empty circle to click. The
// painter is at the top left corner of the row.
void PaintMark(
	QPainter &p,
	int width,
	int height,
	QRect userpic,
	bool selected);

} // namespace BearGram::ChatSelection
