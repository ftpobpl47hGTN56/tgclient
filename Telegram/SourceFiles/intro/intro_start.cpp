/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "intro/intro_start.h"

#include "beargram/beargram_account_transfer.h"
#include "intro/intro_widget.h"
#include "window/window_controller.h"
#include "styles/style_intro.h"

#include "lang/lang_keys.h"
#include "intro/intro_qr.h"
#include "intro/intro_phone.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "main/main_account.h"
#include "main/main_app_config.h"

namespace Intro {
namespace details {

StartWidget::StartWidget(
	QWidget *parent,
	not_null<Main::Account*> account,
	not_null<Data*> data)
: Step(parent, account, data, true) {
	setMouseTracking(true);
	setTitleText(rpl::single(u"BearGram"_q));
	setDescriptionText(tr::lng_intro_about());
	setupBearGramImport();
	show();
}

void StartWidget::setupBearGramImport() {
	// BearGram: accounts exported from another BearGram, without a code.
	const auto import = Ui::CreateChild<Ui::LinkButton>(
		this,
		u"Импорт аккаунтов из файла"_q);
	import->show();
	import->setClickedCallback([=] {
		BearGram::AccountTransfer::StartImport(
			getData()->controller->uiShow());
	});
	rpl::combine(
		sizeValue(),
		import->widthValue()
	) | rpl::on_next([=](QSize size, int width) {
		import->moveToLeft(
			(size.width() - width) / 2,
			contentTop() + st::introStepFieldTop);
	}, import->lifetime());
}

void StartWidget::submit() {
	account().destroyStaleAuthorizationKeys();
	goNext<QrWidget>();
}

rpl::producer<QString> StartWidget::nextButtonText() const {
	return tr::lng_start_msgs();
}

rpl::producer<> StartWidget::nextButtonFocusRequests() const {
	return _nextButtonFocusRequests.events();
}

void StartWidget::activate() {
	Step::activate();
	setInnerFocus();
}

void StartWidget::setInnerFocus() {
	_nextButtonFocusRequests.fire({});
}

} // namespace details
} // namespace Intro
