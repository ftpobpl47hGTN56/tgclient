/*
This file is part of BearGram, a personal build of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "beargram/beargram_post_filter.h"

#include "apiwrap.h"
#include "core/application.h"
#include "core/file_utilities.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "settings/settings_common.h"
#include "ui/boxes/confirm_box.h"
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
#include "styles/style_info.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
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

[[nodiscard]] Rule PeerRule(not_null<PeerData*> peer) {
	return Rule{
		.type = RuleType::Peer,
		.peer = peer->id,
		.text = peer->name(),
		.username = peer->username(),
	};
}

// "@username" asks the API for the id, then calls done with the chat.
void ResolveUsername(
		not_null<Ui::GenericBox*> box,
		not_null<Main::Session*> session,
		const QString &username,
		Fn<void(not_null<PeerData*>)> done) {
	if (const auto known = session->data().peerByUsername(username)) {
		done(known);
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
		if (const auto peer = session->data().peerLoaded(
				peerFromMTP(data.vpeer()))) {
			done(peer);
		}
	}).fail([=] {
		if (const auto strong = weak.get()) {
			strong->showToast(u"Не найдено: @"_q + username);
		}
	}).send();
}

// Turns what was typed into a rule: @username or a t.me link, an id, or a
// word. Calls done right away, or after the API answers for a username.
void ParseInput(
		not_null<Ui::GenericBox*> box,
		not_null<Main::Session*> session,
		const QString &text,
		bool wholeWord,
		Fn<void(Rule)> done) {
	if (const auto username = ExtractUsername(text); !username.isEmpty()) {
		ResolveUsername(box, session, username, [=](not_null<PeerData*> peer) {
			done(PeerRule(peer));
		});
	} else if (const auto id = ParseId(text)) {
		const auto peer = FindPeer(session, id);
		done(peer ? PeerRule(peer) : Rule{ .type = RuleType::Peer, .peer = id });
	} else {
		done(Rule{
			.type = RuleType::Word,
			.text = text,
			.wholeWord = wholeWord,
		});
	}
}

[[nodiscard]] bool SameRuleIdentity(const Rule &a, const Rule &b) {
	return (a.type == b.type)
		&& (a.peer == b.peer)
		&& (a.text == b.text)
		&& (a.wholeWord == b.wholeWord);
}

void EditRuleBox(
		not_null<Ui::GenericBox*> box,
		not_null<Main::Session*> session,
		Rule original) {
	box->setTitle(rpl::single(u"Изменить правило"_q));
	box->setWidth(st::boxWideWidth);

	const auto isPeer = (original.type == RuleType::Peer);
	const auto value = !isPeer
		? original.text
		: original.username.isEmpty()
		? DisplayId(original.peer)
		: ('@' + original.username);

	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			u"Можно вписать другое слово, @username или ID. "
			"Вкл/выкл правила сохранится."_q,
			st::boxLabel),
		st::boxRowPadding);
	Ui::AddSkip(box->verticalLayout());
	const auto field = box->addRow(
		object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			rpl::single(u"@username, ID или слово"_q),
			value),
		st::boxRowPadding);
	const auto whole = box->addRow(
		object_ptr<Ui::Checkbox>(
			box,
			u"Слово целиком"_q,
			original.wholeWord,
			st::defaultBoxCheckbox),
		st::boxRowPadding + QMargins(0, st::boxLittleSkip, 0, 0));

	box->setFocusCallback([=] {
		field->setFocusFast();
		field->selectAll();
	});
	const auto save = [=] {
		const auto text = field->getLastText().trimmed();
		if (text.isEmpty()) {
			field->showError();
			return;
		}
		const auto weak = base::make_weak(box);
		ParseInput(box, session, text, whole->checked(), [=](Rule rule) {
			// Find the rule again: the list may have changed meanwhile.
			const auto &rules = Rules();
			for (auto i = 0; i != int(rules.size()); ++i) {
				if (SameRuleIdentity(rules[i], original)) {
					ReplaceRule(i, std::move(rule));
					break;
				}
			}
			if (const auto strong = weak.get()) {
				strong->closeBox();
			}
		});
	};
	field->submits() | rpl::on_next(save, field->lifetime());
	box->addButton(tr::lng_settings_save(), save);
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

void AddRuleRow(
		not_null<Ui::GenericBox*> box,
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
		st::bearFilterRowRemove);
	remove->setAccessibleName(u"Удалить правило"_q);
	remove->setClickedCallback([=] {
		// The list is rebuilt from RulesChanges(), this row goes away.
		crl::on_main(remove, [=] { RemoveRule(index); });
	});

	const auto edit = Ui::CreateChild<Ui::IconButton>(
		row,
		st::bearFilterRowEdit);
	edit->setAccessibleName(u"Изменить правило"_q);
	const auto copy = rule;
	edit->setClickedCallback([=] {
		box->uiShow()->show(Box(EditRuleBox, session, copy));
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

	// The row is added to an already shown list after a rebuild, and
	// children created after their parent was shown stay hidden in Qt.
	remove->show();
	edit->show();
	toggle->show();

	row->widthValue() | rpl::on_next([=](int width) {
		const auto right = st::boxRowPadding.right() / 2;
		remove->moveToRight(right, (kRowHeight - remove->height()) / 2);
		edit->moveToRight(
			right + remove->width(),
			(kRowHeight - edit->height()) / 2);
		toggle->moveToRight(
			right + remove->width() + edit->width(),
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
		not_null<Ui::GenericBox*> box,
		not_null<Ui::VerticalLayout*> list,
		not_null<Ui::FlatLabel*> empty,
		not_null<Main::Session*> session,
		const QString &query) {
	list->clear();
	const auto &rules = Rules();
	auto shown = 0;
	for (auto i = 0; i != int(rules.size()); ++i) {
		if (MatchesQuery(rules[i], query)) {
			AddRuleRow(box, list, session, i, rules[i]);
			++shown;
		}
	}
	empty->setText(rules.empty()
		? u"Правил пока нет. Добавьте @username, ID или слово выше, "
			"или выделите текст в сообщении и нажмите правой кнопкой → "
			"«В фильтр постов»."_q
		: (shown ? QString() : u"Ничего не найдено."_q));
	empty->setVisible(!shown);
	list->resizeToWidth(list->width());
}

void ExportRules(not_null<Ui::GenericBox*> box) {
	const auto weak = base::make_weak(box);
	FileDialog::GetWritePath(
		box.get(),
		u"Экспорт фильтра постов"_q,
		u"JSON (*.json);;Текст (*.txt)"_q,
		QDir::homePath() + u"/beargram_post_filter.json"_q,
		[=](QString &&path) {
			const auto text = path.endsWith(u".txt"_q, Qt::CaseInsensitive);
			auto file = QFile(path);
			const auto ok = file.open(QIODevice::WriteOnly | QIODevice::Truncate)
				&& (file.write(text ? ExportText() : ExportJson()) >= 0);
			if (const auto strong = weak.get()) {
				strong->showToast(ok
					? (u"Сохранено: "_q + QDir::toNativeSeparators(path))
					: u"Не удалось записать файл."_q);
			}
		});
}

void ImportRules(
		not_null<Ui::GenericBox*> box,
		not_null<Main::Session*> session) {
	const auto weak = base::make_weak(box);
	FileDialog::GetOpenPath(
		box.get(),
		u"Импорт фильтра постов"_q,
		u"Правила (*.json *.txt);;Все файлы (*.*)"_q,
		[=](FileDialog::OpenResult &&result) {
			auto content = result.remoteContent;
			if (content.isEmpty() && !result.paths.isEmpty()) {
				auto file = QFile(result.paths.front());
				if (file.open(QIODevice::ReadOnly)) {
					content = file.readAll();
				}
			}
			const auto strong = weak.get();
			if (!strong) {
				return;
			} else if (content.isEmpty()) {
				strong->showToast(u"Файл пустой или не читается."_q);
				return;
			}
			const auto imported = Import(content);
			auto text = u"Добавлено правил: %1"_q.arg(imported.added);
			if (imported.skipped) {
				text += u", уже были: %1"_q.arg(imported.skipped);
			}
			if (!imported.usernames.isEmpty()) {
				text += u". Ищу @username: %1"_q.arg(
					imported.usernames.size());
			}
			strong->showToast(text);
			for (const auto &username : imported.usernames) {
				ResolveUsername(
					strong,
					session,
					username,
					[](not_null<PeerData*> peer) { AddRule(PeerRule(peer)); });
			}
		});
}

void ClearRulesWithConfirm(not_null<Ui::GenericBox*> box) {
	if (Rules().empty()) {
		box->showToast(u"Правил и так нет."_q);
		return;
	}
	box->uiShow()->show(Ui::MakeConfirmBox({
		.text = u"Удалить все правила фильтра (%1)? "
			"Перед этим можно сделать экспорт."_q.arg(Rules().size()),
		.confirmed = [=](Fn<void()> close) {
			ClearRules();
			close();
		},
		.confirmText = u"Удалить всё"_q,
		.confirmStyle = &st::attentionBoxButton,
	}));
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
			"в группах, каналах и комментариях: от человека (по ID, "
			"смена ника не поможет), с упоминанием его @username, "
			"и со словами из списка."_q));

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
			u"Слово целиком (не скрывать «котлета» по «кот»)"_q,
			false,
			st::defaultBoxCheckbox),
		st::boxRowPadding + QMargins(0, st::boxLittleSkip, 0, 0));

	Ui::AddSkip(container);
	Settings::AddButtonWithIcon(
		container,
		rpl::single(u"Импорт из файла (JSON или TXT)"_q),
		st::settingsButton,
		{ &st::menuIconImportTheme }
	)->setClickedCallback([=] { ImportRules(box, session); });
	Settings::AddButtonWithIcon(
		container,
		rpl::single(u"Экспорт в файл"_q),
		st::settingsButton,
		{ &st::menuIconExport }
	)->setClickedCallback([=] { ExportRules(box); });
	Settings::AddButtonWithIcon(
		container,
		rpl::single(u"Очистить все правила"_q),
		st::settingsAttentionButtonWithIcon,
		{ &st::menuIconDeleteAttention }
	)->setClickedCallback([=] { ClearRulesWithConfirm(box); });

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
		RebuildList(
			box,
			list,
			empty,
			session,
			search->getLastText().trimmed());
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
		const auto weak = base::make_weak(box);
		ParseInput(box, session, text, whole->checked(), [=](Rule rule) {
			const auto title = Title(rule);
			const auto added = AddRule(std::move(rule));
			if (const auto strong = weak.get()) {
				strong->showToast(added
					? (u"Добавлено: "_q + title)
					: (u"Уже в фильтре: "_q + title));
			}
		});
	};
	input->submits() | rpl::on_next(submit, input->lifetime());

	box->setFocusCallback([=] { input->setFocusFast(); });
	box->addButton(rpl::single(u"Добавить"_q), submit);
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}

} // namespace BearGram::PostFilter
