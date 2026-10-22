/*
This file is part of BearGram, a personal build of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "data/data_types.h"

class HistoryItem;

namespace Main {
class Session;
} // namespace Main

namespace Ui {
class PopupMenu;
} // namespace Ui

namespace BearGram {

[[nodiscard]] bool IsHiddenLocally(not_null<const HistoryItem*> item);

void AddLocalHideActions(
	not_null<Ui::PopupMenu*> menu,
	not_null<HistoryItem*> item);
void AddHideSelectedAction(
	not_null<Ui::PopupMenu*> menu,
	not_null<Main::Session*> session,
	MessageIdsList ids,
	Fn<void()> clearSelection);

} // namespace BearGram
