/*
This file is part of BearGram, a personal build of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "beargram/beargram_settings_box.h"

#include "beargram/beargram_settings.h"
#include "lang/lang_keys.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/vertical_list.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"

namespace BearGram {
namespace {

void AddLinesChoice(
		not_null<Ui::VerticalLayout*> container,
		const QString &title,
		std::initializer_list<int> options,
		int current,
		Fn<void(int)> save) {
	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(container, rpl::single(title));
	const auto group = std::make_shared<Ui::RadiobuttonGroup>(current);
	for (const auto value : options) {
		container->add(
			object_ptr<Ui::Radiobutton>(
				container,
				group,
				value,
				QString::number(value),
				st::settingsCheckbox),
			st::settingsSendTypePadding);
	}
	group->setChangedCallback(std::move(save));
}

void AddToggle(
		not_null<Ui::VerticalLayout*> container,
		const QString &title,
		bool current,
		Fn<void(bool)> save) {
	const auto button = container->add(object_ptr<Ui::SettingsButton>(
		container,
		rpl::single(title),
		st::settingsButtonNoIcon
	))->toggleOn(rpl::single(current));
	button->toggledChanges() | rpl::on_next([=](bool enabled) {
		save(enabled);
	}, button->lifetime());
}

} // namespace

void SettingsBox(not_null<Ui::GenericBox*> box) {
	box->setTitle(rpl::single(u"BearGram"_q));
	box->setWidth(st::boxWideWidth);

	const auto container = box->verticalLayout();
	const auto &current = Current();

	Ui::AddSubsectionTitle(container, rpl::single(u"Фильтры"_q));
	AddToggle(
		container,
		u"Скрывать сообщения заблокированных"_q,
		current.hideBlockedUsers,
		[](bool enabled) {
			Update([&](Settings &settings) {
				settings.hideBlockedUsers = enabled;
			});
		});
	AddToggle(
		container,
		u"Скрывать рекламные посты (erid, «Реклама»)"_q,
		current.hideAdPosts,
		[](bool enabled) {
			Update([&](Settings &settings) {
				settings.hideAdPosts = enabled;
			});
		});

	Ui::AddSkip(container);
	Ui::AddDivider(container);
	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(
		container,
		rpl::single(u"Длинные сообщения"_q));
	AddToggle(
		container,
		u"Сворачивать длинные сообщения"_q,
		current.collapseLongMessages,
		[](bool enabled) {
			Update([&](Settings &settings) {
				settings.collapseLongMessages = enabled;
			});
		});

	AddLinesChoice(
		container,
		u"Сворачивать, если строк больше"_q,
		{ 10, 15, 20, 30, 50 },
		current.collapseAfterLines,
		[](int value) {
			Update([&](Settings &settings) {
				settings.collapseAfterLines = value;
			});
		});
	AddLinesChoice(
		container,
		u"Показывать строк в свёрнутом виде"_q,
		{ 3, 6, 10 },
		current.collapsedPreviewLines,
		[](int value) {
			Update([&](Settings &settings) {
				settings.collapsedPreviewLines = value;
			});
		});

	Ui::AddSkip(container);
	Ui::AddDividerText(
		container,
		rpl::single(u"Изменения применятся к чатам после их повторного "
			"открытия. Свои сообщения никогда не скрываются."_q));

	box->addButton(tr::lng_box_ok(), [=] { box->closeBox(); });
}

} // namespace BearGram
