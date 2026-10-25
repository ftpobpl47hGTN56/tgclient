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
#include "ui/widgets/continuous_sliders.h"
#include "ui/widgets/labels.h"
#include "ui/vertical_list.h"
#include "ui/wrap/padding_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"
#include "styles/style_background_preview_box.h"
#include "styles/style_edit_peer_members.h"
#include "styles/style_settings_local_storage.h"

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

void AddIntSlider(
		not_null<Ui::VerticalLayout*> container,
		const QString &title,
		int min,
		int max,
		int current,
		Fn<void(int)> save) {
	const auto row = container->add(
		object_ptr<Ui::FixedHeightWidget>(
			container,
			st::localStorageLimitLabel.font->height),
		st::localStorageLimitLabelMargin);
	const auto name = Ui::CreateChild<Ui::LabelSimple>(
		row,
		st::localStorageLimitLabel,
		title);
	const auto label = Ui::CreateChild<Ui::LabelSimple>(
		row,
		st::localStorageLimitValue);
	label->setText(QString::number(current));
	rpl::combine(
		row->widthValue(),
		label->widthValue()
	) | rpl::on_next([=](int width, int) {
		name->moveToLeft(0, 0, width);
		label->moveToRight(0, 0, width);
	}, row->lifetime());
	const auto slider = container->add(
		object_ptr<Ui::MediaSlider>(container, st::localStorageLimitSlider),
		st::localStorageLimitMargin);
	slider->resize(st::localStorageLimitSlider.seekSize);
	slider->setPseudoDiscrete(
		max - min + 1,
		[=](int index) { return min + index; },
		current,
		[=](int value) { label->setText(QString::number(value)); },
		[=](int value) { save(value); });
}

} // namespace

void SettingsBox(not_null<Ui::GenericBox*> box) {
	box->setTitle(rpl::single(u"BearGram (сборка 12)"_q));
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
	AddToggle(
		container,
		u"Скрывать посты каналов с закрытыми комментариями"_q,
		current.hideNoCommentsPosts,
		[](bool enabled) {
			Update([&](Settings &settings) {
				settings.hideNoCommentsPosts = enabled;
			});
		});

	Ui::AddSkip(container);
	Ui::AddDividerText(
		container,
		rpl::single(u"Рекламные посты ищутся по маркировке (erid, «Реклама»), "
			"а реклама Telegram ниже — это официальные спонсорские сообщения."_q));
	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(container, rpl::single(u"Реклама Telegram"_q));
	AddToggle(
		container,
		u"Скрывать рекламу Telegram (Sponsored)"_q,
		current.hideSponsored,
		[](bool enabled) {
			Update([&](Settings &settings) {
				settings.hideSponsored = enabled;
			});
		});
	Ui::AddSkip(container);
	Ui::AddDividerText(
		container,
		rpl::single(u"Блоки рекламы внизу каналов, в ботах и в видео. "
			"Правила Telegram API (п. 3.3) запрещают сторонним приложениям "
			"мешать их показу; санкции направлены против приложения (api_id), "
			"а не аккаунта. Включайте на своё усмотрение."_q));

	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(container, rpl::single(u"Аккаунты"_q));
	AddIntSlider(
		container,
		u"Лимит аккаунтов"_q,
		1,
		kMaxAccountsLimit,
		current.maxAccounts,
		[](int value) {
			Update([&](Settings &settings) {
				settings.maxAccounts = value;
			});
		});
	Ui::AddDividerText(
		container,
		rpl::single(u"Сколько аккаунтов можно добавить без Telegram Premium. "
			"Уже добавленные аккаунты лимит не удаляет."_q));

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
