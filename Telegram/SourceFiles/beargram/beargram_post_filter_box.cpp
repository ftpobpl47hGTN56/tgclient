/*
This file is part of BearGram, a personal build of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "beargram/beargram_post_filter.h"

#include "apiwrap.h"
#include "core/application.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "ui/empty_userpic.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/userpic_view.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/padding_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_basic.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

#include <QtCore/QRegularExpression>

namespace BearGram::PostFilter {
namespace {

constexpr auto kRowHeight = 56;
constexpr auto kUserpicSize = 40;
constexpr auto kUserpicTop = 8;
constexpr auto kTextSkip = 12;

// Rules are shared by all accounts, so the avatar and the current name are
// taken from whichever account knows this chat, the active one first.
[[nodiscard]] PeerData *FindPeer(
		not_null<Main::Session*> session,
		PeerId id) {
	if (const auto peer = session->data().peerLoaded(id)) {
		return peer;
	}
	for (const auto &[index, account] : Core::App().domain().accounts()) {
		if (account->sessionExists()) {
			if (const auto peer = account->session().data().peerLoaded(id)) {
				return peer;
			}
		}
	}
	return nullptr;
}

// Takes fresh names from the loaded chats, so a renamed person is shown
// under the new name and username (the rule itself is by id).
void RefreshNames(not_null<Main::Session*> session) {
	auto peers = std::vector<PeerId>();
	for (const auto &rule : Rules()) {
		if (rule.type == RuleType::Peer) {
			peers.push_back(rule.peer);
		}
	}
	for (const auto id : peers) {
		if (const auto peer = FindPeer(session, id)) {
			UpdatePeerInfo(peer);
		}
	}
}

[[nodiscard]] QString Title(const Rule &rule) {
	if (rule.type == RuleType::Word) {
		return rule.text;
	} else if (!rule.text.isEmpty()) {
		return rule.text;
	} else if (!rule.username.isEmpty()) {
		return '@' + rule.username;
	}
	return u"ID "_q + DisplayId(rule.peer);
}

[[nodiscard]] QString Subtitle(const Rule &rule) {
	auto result = QString();
	if (rule.type == RuleType::Word) {
		result = rule.wholeWord
			? u"слово целиком"_q
			: u"слово, любое вхождение"_q;
	} else {
		const auto kind = peerIsUser(rule.peer)
			? u"пользователь"_q
			: u"канал / группа"_q;
		result = rule.username.isEmpty()
			? (kind + u" · "_q + DisplayId(rule.peer))
			: ('@' + rule.username + u" · "_q + DisplayId(rule.peer));
	}
	return rule.enabled ? result : (result + u" · выключено"_q);
}

[[nodiscard]] bool MatchesQuery(const Rule &rule, const QString &query) {
	if (query.isEmpty()) {
		return true;
	}
	const auto check = [&](const QString &value) {
		return value.contains(query, Qt::CaseInsensitive);
	};
	return check(rule.text)
		|| (rule.type == RuleType::Peer
			&& (check(rule.username)
				|| check('@' + rule.username)
				|| check(DisplayId(rule.peer))));
}

void AddRuleRow(
		not_null<Ui::VerticalLayout*> list,
		not_null<Main::Session*> session,
		int index,
		const Rule &rule) {
	const auto row = list->add(
		object_ptr<Ui::FixedHeightWidget>(list, kRowHeight));

	const auto title = Title(rule);
	const auto subtitle = Subtitle(rule);
	const auto enabled = rule.enabled;
	const auto isPeer = (rule.type == RuleType::Peer);
	const auto peerId = rule.peer;

	struct State {
		Ui::PeerUserpicView view;
		std::unique_ptr<Ui::EmptyUserpic> empty;
	};
	const auto state = row->lifetime().make_state<State>();
	state->empty = std::make_unique<Ui::EmptyUserpic>(
		Ui::EmptyUserpic::UserpicColor(Ui::EmptyUserpic::ColorIndex(
			isPeer ? peerId.value : uint64(qHash(title)))),
		isPeer ? title : u"#"_q);

	const auto remove = Ui::CreateChild<Ui::IconButton>(
		row,
		st::boxTitleClose);
	remove->setAccessibleName(u"Удалить правило"_q);
	remove->setClickedCallback([=] {
		// The list is rebuilt from RulesChanges(), this row goes away.
		crl::on_main(remove, [=] { RemoveRule(index); });
	});

	const auto toggle = Ui::CreateChild<Ui::Checkbox>(
		row,
		QString(),
		enabled,
		st::defaultCheckbox,
		st::defaultToggle);
	toggle->resizeToWidth(toggle->naturalWidth() + st::defaultCheckbox.width);
	toggle->checkedChanges() | rpl::on_next([=](bool checked) {
		crl::on_main(toggle, [=] { SetRuleEnabled(index, checked); });
	}, toggle->lifetime());

	row->widthValue() | rpl::on_next([=](int width) {
		const auto right = st::boxRowPadding.right() / 2;
		remove->moveToRight(right, (kRowHeight - remove->height()) / 2);
		toggle->moveToRight(
			right + remove->width(),
			(kRowHeight - toggle->height()) / 2);
	}, row->lifetime());

	row->paintRequest() | rpl::on_next([=] {
		auto p = Painter(row);
		const auto left = st::boxRowPadding.left();
		const auto width = row->width();
		if (!enabled) {
			p.setOpacity(0.5);
		}
		const auto peer = isPeer ? FindPeer(session, peerId) : nullptr;
		if (peer) {
			peer->paintUserpicLeft(
				p,
				state->view,
				left,
				kUserpicTop,
				width,
				kUserpicSize);
		} else {
			state->empty->paintCircle(
				p,
				left,
				kUserpicTop,
				width,
				kUserpicSize);
		}
		const auto textLeft = left + kUserpicSize + kTextSkip;
		const auto textWidth = std::max(
			toggle->x() - textLeft - kTextSkip,
			1);
		p.setPen(st::windowFg);
		p.setFont(st::semiboldFont);
		p.drawTextLeft(
			textLeft,
			kUserpicTop + 1,
			width,
			st::semiboldFont->elided(title, textWidth));
		p.setPen(st::windowSubTextFg);
		p.setFont(st::normalFont);
		p.drawTextLeft(
			textLeft,
			kUserpicTop + kUserpicSize - st::normalFont->height - 1,
			width,
			st::normalFont->elided(subtitle, textWidth));
	}, row->lifetime());

	if (isPeer) {
		// Avatars arrive later than the box opens.
		session->downloaderTaskFinished(
		) | rpl::on_next([=] {
			row->update();
		}, row->lifetime());
	}
}

void RebuildList(
		not_null<Ui::VerticalLayout*> list,
		not_null<Ui::FlatLabel*> empty,
		not_null<Main::Session*> session,
		const QString &query) {
	list->clear();
	const auto &rules = Rules();
	auto shown = 0;
	for (auto i = 0; i != int(rules.size()); ++i) {
		if (MatchesQuery(rules[i], query)) {
			AddRuleRow(list, session, i, rules[i]);
			++shown;
		}
	}
	empty->setText(rules.empty()
		? u"Правил пока нет. Добавьте @username, ID или слово выше, "
			"или нажмите правой кнопкой на сообщение → "
			"«В фильтр постов»."_q
		: (shown ? QString() : u"Ничего не найдено."_q));
	empty->setVisible(!shown);
	list->resizeToWidth(list->width());
}

[[nodiscard]] QString ExtractUsername(const QString &text) {
	static const auto link = QRegularExpression(
		u"^(?:https?://)?(?:www\\.)?(?:t\\.me|telegram\\.me)/"
		"(?:s/)?@?([A-Za-z0-9_]{4,32})/?$"_q,
		QRegularExpression::CaseInsensitiveOption);
	static const auto mention = QRegularExpression(
		u"^@([A-Za-z0-9_]{4,32})$"_q);
	if (const auto m = link.match(text); m.hasMatch()) {
		return m.captured(1);
	} else if (const auto m = mention.match(text); m.hasMatch()) {
		return m.captured(1);
	}
	return QString();
}

void ResolveAndAdd(
		not_null<Ui::GenericBox*> box,
		not_null<Main::Session*> session,
		const QString &username) {
	const auto added = [=](not_null<PeerData*> peer) {
		const auto fresh = AddPeer(peer);
		box->showToast(fresh
			? (u"Добавлено: "_q + peer->name())
			: (u"Уже в фильтре: "_q + peer->name()));
	};
	if (const auto known = session->data().peerByUsername(username)) {
		added(known);
		return;
	}
	const auto weak = base::make_weak(box);
	session->api().request(MTPcontacts_ResolveUsername(
		MTP_flags(0),
		MTP_string(username),
		MTPstring()
	)).done([=](const MTPcontacts_ResolvedPeer &result) {
		const auto &data = result.data();
		session->data().processUsers(data.vusers());
		session->data().processChats(data.vchats());
		const auto peer = session->data().peerLoaded(
			peerFromMTP(data.vpeer()));
		if (!peer) {
			return;
		}
		if (weak.get()) {
			added(peer);
		} else {
			AddPeer(peer);
		}
	}).fail([=] {
		if (const auto strong = weak.get()) {
			strong->showToast(u"Не найдено: @"_q + username);
		}
	}).send();
}

} // namespace

void FilterBox(
		not_null<Ui::GenericBox*> box,
		not_null<Main::Session*> session) {
	box->setTitle(rpl::single(u"Фильтр постов"_q));
	box->setWidth(st::boxWideWidth);
	box->addTopButton(st::boxTitleClose, [=] { box->closeBox(); });

	RefreshNames(session);

	const auto container = box->verticalLayout();

	// Global switch: the same for every account, like the sound settings.
	const auto master = container->add(object_ptr<Ui::SettingsButton>(
		container,
		rpl::single(u"Фильтр включён"_q),
		st::settingsButtonNoIcon
	))->toggleOn(EnabledValue());
	master->toggledChanges() | rpl::on_next([=](bool enabled) {
		SetEnabled(enabled);
	}, master->lifetime());

	Ui::AddDividerText(
		container,
		rpl::single(u"Работает во всех аккаунтах. Скрывает посты "
			"в группах, каналах и комментариях. Отправитель ищется "
			"по ID, поэтому смена ника или имени фильтр не обходит."_q));

	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(
		container,
		rpl::single(u"Добавить тег"_q));
	const auto input = container->add(
		object_ptr<Ui::InputField>(
			container,
			st::defaultInputField,
			rpl::single(u"@username, ID или слово"_q),
			QString()),
		st::boxRowPadding);
	const auto whole = container->add(
		object_ptr<Ui::Checkbox>(
			container,
			u"Слово целиком (не скрывать «котлета» по слову «кот»)"_q,
			false,
			st::defaultBoxCheckbox),
		st::boxRowPadding + QMargins(0, st::boxLittleSkip, 0, 0));

	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(
		container,
		rpl::single(u"Правила"_q));
	const auto search = container->add(
		object_ptr<Ui::InputField>(
			container,
			st::defaultInputField,
			rpl::single(u"Поиск по тегам"_q),
			QString()),
		st::boxRowPadding);
	Ui::AddSkip(container);

	const auto empty = container->add(
		object_ptr<Ui::FlatLabel>(
			container,
			QString(),
			st::boxDividerLabel),
		st::boxRowPadding);
	const auto list = container->add(
		object_ptr<Ui::VerticalLayout>(container));

	const auto rebuild = [=] {
		RebuildList(list, empty, session, search->getLastText().trimmed());
	};
	rebuild();
	RulesChanges() | rpl::on_next(rebuild, list->lifetime());
	search->changes() | rpl::on_next(rebuild, search->lifetime());

	const auto submit = [=] {
		const auto text = input->getLastText().trimmed();
		if (text.isEmpty()) {
			input->showError();
			return;
		}
		input->clear();
		if (const auto username = ExtractUsername(text); !username.isEmpty()) {
			ResolveAndAdd(box, session, username);
		} else if (const auto id = ParseId(text)) {
			if (const auto peer = FindPeer(session, id)) {
				AddPeer(peer);
			} else if (!AddPeerId(id)) {
				box->showToast(u"Этот ID уже в фильтре."_q);
			}
		} else if (!AddWord(text, whole->checked())) {
			box->showToast(u"Такое слово уже в фильтре."_q);
		}
	};
	input->submits() | rpl::on_next(submit, input->lifetime());

	box->setFocusCallback([=] { input->setFocusFast(); });
	box->addButton(rpl::single(u"Добавить"_q), submit);
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}

} // namespace BearGram::PostFilter
