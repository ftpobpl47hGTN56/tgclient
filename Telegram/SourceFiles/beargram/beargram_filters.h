/*
This file is part of BearGram, a personal build of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class HistoryItem;

namespace BearGram {

[[nodiscard]] bool IsFilteredOut(
	not_null<const HistoryItem*> item,
	const HistoryItem *textItem);

} // namespace BearGram
