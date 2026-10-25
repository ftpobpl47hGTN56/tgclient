/*
This file is part of BearGram, a personal build of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "beargram/beargram_nicknames.h"

#include "data/data_user.h"
#include "lang/lang_keys.h"
#include "settings.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/vertical_list.h"
#include "styles/style_boxes.h"
#include "styles/style_edit_peer_members.h"
#include "styles/style_layers.h"

#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

namespace BearGram {
namespace {

[[nodiscard]] QString NicknamesPath() {
	return cWorkingDir() + u"tdata/beargram_nicknames.json"_q;
}

[[nodiscard]] base::flat_map<uint64, QString> Load() {
	auto result = base::flat_map<uint64, QString>();
	auto file = QFile(NicknamesPath());
	if (!file.open(QIODevice::ReadOnly)) {
		return result;
	}
	const auto object = QJsonDocument::fromJson(file.readAll()).object();
	for (auto i = object.begin(); i != object.end(); ++i) {
		const auto id = i.key().toULongLong();
		const auto name = i.value().toString().trimmed();
		if (id && !name.isEmpty()) {
			result.emplace(id, name);
		}
	}
	return result;
}

[[nodiscard]] base::flat_map<uint64, QString> &Names() {
	static auto names = Load();
	return names;
}

void Save() {
	auto object = QJsonObject();
	for (const auto &[id, name] : Names()) {
		object.insert(QString::number(id), name);
	}
	auto file = QFile(NicknamesPath());
	if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
	}
}

} // namespace

QString Nickname(UserId userId) {
	const auto &names = Names();
	if (names.empty()) {
		return QString();
	}
	const auto i = names.find(userId.bare);
	return (i != end(names)) ? i->second : QString();
}

void SetNickname(not_null<UserData*> user, const QString &name) {
	const auto value = name.trimmed();
	const auto id = peerToUser(user->id).bare;
	if (value.isEmpty()) {
		Names().remove(id);
	} else {
		Names()[id] = value;
	}
	Save();
	// Re-apply the name: UserData::setName() picks the local name up.
	user->setName(
		user->firstName,
		user->lastName,
		user->nameOrPhone,
		user->editableUsername());
}

void ShowNicknameBox(
		std::shared_ptr<Ui::Show> show,
		not_null<UserData*> user) {
	show->show(Box([=](not_null<Ui::GenericBox*> box) {
		const auto realName = user->firstName.isEmpty()
			? user->lastName
			: (user->lastName.isEmpty()
				? user->firstName
				: (user->firstName + ' ' + user->lastName));
		const auto current = Nickname(peerToUser(user->id));
		box->setTitle(rpl::single(u"Локальное имя"_q));
		box->addRow(
			object_ptr<Ui::FlatLabel>(
				box,
				u"Видно только Вам в BearGram и никуда не отправляется. "
				"Настоящее имя: "_q + realName,
				st::boxLabel),
			st::boxRowPadding);
		Ui::AddSkip(box->verticalLayout());
		const auto field = box->addRow(
			object_ptr<Ui::InputField>(
				box,
				st::defaultInputField,
				rpl::single(realName),
				current),
			st::addContactFieldMargin);
		box->setFocusCallback([=] {
			field->setFocusFast();
			field->selectAll();
		});
		const auto save = [=] {
			SetNickname(user, field->getLastText());
			box->closeBox();
		};
		field->submits() | rpl::on_next(save, field->lifetime());
		box->addButton(tr::lng_settings_save(), save);
		if (!current.isEmpty()) {
			box->addLeftButton(rpl::single(u"Сбросить"_q), [=] {
				SetNickname(user, QString());
				box->closeBox();
			});
		}
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}));
}

} // namespace BearGram
