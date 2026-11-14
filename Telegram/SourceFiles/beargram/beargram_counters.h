/*
This file is part of BearGram, a personal build of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/object_ptr.h"

class QPainter;

namespace Main {
class Session;
} // namespace Main

namespace Ui {
class RpWidget;
} // namespace Ui

// Counters with a number in a rounded frame: pinned chats above the chats
// list, stickers in each pack, emoji in each section, saved GIFs.
namespace BearGram::Counters {

// The width of a counter for the number, the font of the number.
[[nodiscard]] int PillWidth(const style::font &font, int count);

// A counter, left / top of its frame, the frame is font->height + 2 high.
// Returns the width.
int PaintPill(
	QPainter &p,
	int left,
	int top,
	int outerWidth,
	const style::font &font,
	const QColor &color,
	int count);

// "Закреплено: каналы ⓹ группы ⓸ переписки ⓷" above the chats list.
// Its height is 0 while nothing is pinned in the main list or not shown
// (another folder, search, a forum).
[[nodiscard]] object_ptr<Ui::RpWidget> CreatePinnedBar(
	QWidget *parent,
	not_null<Main::Session*> session,
	rpl::producer<bool> shown);

} // namespace BearGram::Counters
