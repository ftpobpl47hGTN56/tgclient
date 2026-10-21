/*
This file is part of BearGram, a personal build of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "ui/click_handler.h"

class HistoryItem;

namespace Ui::Text {
class String;
} // namespace Ui::Text

namespace BearGram {

struct CollapsedText {
	bool collapsible = false;
	bool expanded = false;
	int visibleLines = 0;
	int toggleTop = 0;
	int toggleHeight = 0;
	int totalHeight = 0;
};

[[nodiscard]] CollapsedText ComputeCollapsedText(
	not_null<const HistoryItem*> item,
	const Ui::Text::String &text,
	int textWidth,
	int fullHeight,
	int toggleHeight);

[[nodiscard]] QString ToggleText(bool expanded);
[[nodiscard]] ClickHandlerPtr ToggleLink(not_null<const HistoryItem*> item);

} // namespace BearGram
