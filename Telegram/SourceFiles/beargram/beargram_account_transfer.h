/*
This file is part of BearGram, a personal build of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Ui {
class GenericBox;
class Show;
class VerticalLayout;
} // namespace Ui

// Moving logged in accounts between desktop BearGram installations (another
// computer, a reinstall, a second copy) with a password protected file,
// without logging in again.
//
// A desktop session of an account is its MTP authorization: the user id,
// the main DC and the authorization keys (Account::serializeMtpAuthorization,
// the same data tdata keeps). Export writes it for every logged in account
// into one file encrypted with AES-256-GCM, the key derived from the
// password with PBKDF2. Import adds the accounts that are not logged in here
// yet as new accounts; chats are then loaded from the server.
//
// The same session must not run in two apps at once: Telegram may end it
// (AUTH_KEY_DUPLICATED). After moving, remove the old copy without logging
// out, because logging out ends the session for both.
namespace BearGram::AccountTransfer {

// "Перенос аккаунтов" rows for the BearGram settings box.
void AddSettingsSection(
	not_null<Ui::VerticalLayout*> container,
	std::shared_ptr<Ui::Show> show);

// Import for the welcome screen, where there is no settings yet.
void StartImport(std::shared_ptr<Ui::Show> show);

} // namespace BearGram::AccountTransfer
