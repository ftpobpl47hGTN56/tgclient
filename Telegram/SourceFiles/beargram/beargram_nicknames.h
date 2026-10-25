/*
This file is part of BearGram, a personal build of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "data/data_peer_id.h"

class UserData;

namespace Ui {
class Show;
} // namespace Ui

namespace BearGram {

// Local names of users: seen only in this app, never sent anywhere.
// Stored by user id in tdata/beargram_nicknames.json.

[[nodiscard]] QString Nickname(UserId userId);
void SetNickname(not_null<UserData*> user, const QString &name);
void ShowNicknameBox(
	std::shared_ptr<Ui::Show> show,
	not_null<UserData*> user);

} // namespace BearGram
