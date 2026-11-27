/*
This file is part of BearGram, a personal build of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "data/data_peer_id.h"

#include <QtCore/QJsonObject>

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

// All nicknames as {user id: name}, for the settings export.
// An empty object means there are none.
[[nodiscard]] QJsonObject ExportNicknames();

// Replaces all nicknames with the given ones and saves.
void ImportNicknames(const QJsonObject &names);

} // namespace BearGram
