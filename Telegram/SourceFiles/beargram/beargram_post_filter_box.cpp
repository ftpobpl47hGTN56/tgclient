/*
This file is part of BearGram, a personal build of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "beargram/beargram_post_filter.h"

#include "apiwrap.h"
#include "base/flat_set.h"
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
#include "ui/effects/animations.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/userpic_view.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/padding_wrap.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_basic.h"
#include "styles/style_boxes.h"
#include "styles/style_info.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

#include <algorithm>
#include <tuple>
#include <utility>
#include <rpl/variable.h>

#include <QtCore/QDate>
#include <QtCore/QDateTime>
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

[[nodiscard]] QString CreatedDate(TimeId created) {
	return created
		? langDayOfMonth(QDateTime::fromSecsSinceEpoch(created).date())
		: QString();
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
	if (const auto date = CreatedDate(rule.created); !date.isEmpty()) {
		result += u" · добавлено "_q + date;
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

struct RuleKey {
	int type = 0;
	uint64 peer = 0;
	QString text;
	bool wholeWord = false;

	friend bool operator<(const RuleKey &a, const RuleKey &b) {
		return std::tie(a.type, a.peer, a.text, a.wholeWord)
			< std::tie(b.type, b.peer, b.text, b.wholeWord);
	}
};

[[nodiscard]] RuleKey FromRule(const Rule &rule) {
	return {
		.type = int(rule.type),
		.peer = rule.peer.value,
		.text = rule.text,
		.wholeWord = rule.wholeWord,
	};
}

struct SelectionUi {
	rpl::variable<bool> selecting = false;
	rpl::variable<QString> deleteText = u"Удалить"_q;
	base::flat_set<RuleKey> selected;
	std::vector<std::pair<not_null<Ui::Checkbox*>, RuleKey>> checks;
	Ui::SettingsButton *deleteButton = nullptr;
};

void RefreshSelectionUi(not_null<SelectionUi*> selection) {
	const auto count = int(selection->selected.size());
	selection->deleteText = count
		? (u"Удалить (%1)"_q).arg(count)
		: u"Удалить"_q;
	if (selection->deleteButton) {
		selection->deleteButton->setEnabled(count > 0);
	}
}

void SetRuleSelected(
		not_null<SelectionUi*> selection,
		const RuleKey &key,
		bool selected) {
	if (selected) {
		selection->selected.insert(key);
	} else {
		selection->selected.remove(key);
	}
	RefreshSelectionUi(selection);
}

[[nodiscard]] int ContentTopOffset(
		not_null<QWidget*> widget,
		not_null<QWidget*> ancestor) {
	auto result = 0;
	for (auto w = widget.get(); w && w != ancestor.get(); w = w->parentWidget()) {
		result += w->y();
	}
	return result;
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
		not_null<Ui::VerticalLayout*> contentRoot,
		not_null<Ui::VerticalLayout*> list,
		not_null<Main::Session*> session,
		not_null<SelectionUi*> selection,
		const Fn<void(int contentY)> &beforeRemove,
		int index,
		const Rule &rule) {
	const auto row = list->add(
		object_ptr<Ui::FixedHeightWidget>(list, kRowHeight));

	const auto title = Title(rule);
	const auto subtitle = Subtitle(rule);
	const auto enabled = rule.enabled;
	const auto isPeer = (rule.type == RuleType::Peer);
	const auto peerId = rule.peer;
	const auto key = FromRule(rule);

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
		beforeRemove(ContentTopOffset(row, contentRoot));
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

	const auto check = Ui::CreateChild<Ui::Checkbox>(
		row,
		QString(),
		selection->selected.contains(key),
		st::defaultCheckbox,
		st::defaultCheck);
	check->resizeToWidth(check->naturalWidth() + st::defaultCheckbox.width);
	// The whole row selects, the checkbox is just its indicator.
	check->setAttribute(Qt::WA_TransparentForMouseEvents);

	remove->show();
	edit->show();
	toggle->show();
	check->show();

	selection->checks.emplace_back(check, key);

	row->widthValue() | rpl::on_next([=](int width) {
		const auto right = st::boxRowPadding.right() / 2;
		remove->moveToRight(right, (kRowHeight - remove->height()) / 2);
		edit->moveToRight(
			right + remove->width(),
			(kRowHeight - edit->height()) / 2);
		toggle->moveToRight(
			right + remove->width() + edit->width(),
			(kRowHeight - toggle->height()) / 2);
		check->moveToRight(right, (kRowHeight - check->height()) / 2);
	}, row->lifetime());

	std::move(selection->selecting.value()) | rpl::on_next([=](bool on) {
		check->setVisible(on);
		check->setChecked(
			selection->selected.contains(key),
			Ui::Checkbox::NotifyAboutChange::DontNotify);
		toggle->setVisible(!on);
		remove->setVisible(!on);
		edit->setVisible(!on);
		row->setCursor(on ? Qt::PointingHandCursor : Qt::ArrowCursor);
	}, check->lifetime());

	row->events(
	) | rpl::filter([=](not_null<QEvent*> event) {
		return (event->type() == QEvent::MouseButtonRelease)
			&& selection->selecting.current();
	}) | rpl::on_next([=](not_null<QEvent*> event) {
		if (static_cast<QMouseEvent*>(event.get())->button()
				!= Qt::LeftButton) {
			return;
		}
		SetRuleSelected(
			selection,
			key,
			!selection->selected.contains(key));
		check->setChecked(
			selection->selected.contains(key),
			Ui::Checkbox::NotifyAboutChange::DontNotify);
	}, row->lifetime());

	// The list keeps its width when it is rebuilt (search, toggles), so
	// the layout doesn't resize new rows: give them the width right away,
	// otherwise the buttons are placed for a zero wide row.
	if (const auto width = list->width()) {
		row->resizeToWidth(width);
	}

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

enum class GroupType {
	Channel,
	User,
	Word,
};

constexpr auto kGroupToggleDuration = crl::time(200);

struct GroupToggles {
	bool open = true;
	float64 progress = 1.;
	Ui::Animations::Simple animation;
};

// A "Каналы и группы (3)" style header with a chevron pointing down when
// open and right when folded; the rules below fold with the toggle.
// The button lives in the outer list, not in the folding part: folding
// must not hide the chevron that unfolds the group back, and a rebuild
// must not recreate the button, that resets its folded state.
void AddGroupHeader(
		not_null<Ui::SettingsButton*> header,
		not_null<GroupToggles*> toggles,
		bool *persistedOpen,
		Fn<void()> toggled) {
	header->setClickedCallback([=] {
		toggles->open = !toggles->open;
		*persistedOpen = toggles->open;
		toggles->animation.start(
			[=](float64 value) {
				toggles->progress = value;
				header->update();
			},
			toggles->progress,
			toggles->open ? 1. : 0.,
			kGroupToggleDuration);
		toggled();
	});
	header->events(
	) | rpl::filter([=](not_null<QEvent*> event) {
		return event->type() == QEvent::Resize;
	}) | rpl::on_next([=] {
		header->update();
	}, header->lifetime());
	header->paintRequest() | rpl::on_next([=] {
		auto p = Painter(header);
		const auto center = header->height() / 2.;
		const auto size = float64(st::bearFilterGroupChevronSize);
		const auto rotate = -90. * (1. - toggles->progress);
		const auto x = header->width() - st::bearFilterGroupChevronRight;
		auto path = QPainterPath();
		path.moveTo(x - size, center - size);
		path.lineTo(x, center);
		path.lineTo(x + size, center - size);
		p.save();
		p.translate(x, center);
		p.rotate(rotate);
		p.translate(-x, -center);
		auto color = st::windowSubTextFg->c;
		color.setAlphaF(color.alphaF() * st::bearFilterGroupChevronOpacity);
		auto pen = QPen(color);
		pen.setWidthF(float64(st::bearFilterGroupChevronStroke));
		pen.setCapStyle(Qt::RoundCap);
		pen.setJoinStyle(Qt::RoundJoin);
		p.setPen(pen);
		p.setBrush(Qt::NoBrush);
		p.drawPath(path);
		p.restore();
	}, header->lifetime());
}

enum class SortMode {
	ByAdded,
	Alphabetical,
	Newest,
	Oldest,
};

[[nodiscard]] QString SortModeText(SortMode mode) {
	switch (mode) {
	case SortMode::Alphabetical:
		return u"Сортировка: по алфавиту"_q;
	case SortMode::Newest:
		return u"Сортировка: сначала новые"_q;
	case SortMode::Oldest:
		return u"Сортировка: сначала старые"_q;
	}
	return u"Сортировка: по добавлению"_q;
}

[[nodiscard]] SortMode NextSortMode(SortMode mode) {
	switch (mode) {
	case SortMode::ByAdded:
		return SortMode::Alphabetical;
	case SortMode::Alphabetical:
		return SortMode::Newest;
	case SortMode::Newest:
		return SortMode::Oldest;
	}
	return SortMode::ByAdded;
}

void SortRuleIndices(std::vector<int> &indices, SortMode mode) {
	const auto &rules = Rules();
	switch (mode) {
	case SortMode::Alphabetical:
		std::stable_sort(
			begin(indices),
			end(indices),
			[&](int a, int b) {
				return Title(rules[a]).localeAwareCompare(
					Title(rules[b])) < 0;
			});
		return;
	case SortMode::Newest:
		std::stable_sort(
			begin(indices),
			end(indices),
			[&](int a, int b) {
				return rules[a].created > rules[b].created;
			});
		return;
	case SortMode::Oldest:
		std::stable_sort(
			begin(indices),
			end(indices),
			[&](int a, int b) {
				const auto ca = rules[a].created;
				const auto cb = rules[b].created;
				return (ca && cb)
					? (ca < cb)
					: (ca && !cb); // dated rules first, undated last
			});
		return;
	case SortMode::ByAdded:
		return;
	}
}

[[nodiscard]] GroupType GroupOfRule(const Rule &rule) {
	return (rule.type == RuleType::Word)
		? GroupType::Word
		: (peerIsUser(rule.peer) ? GroupType::User : GroupType::Channel);
}

// A permanent group: created once per box, so its chevron state survives
// every rebuild of the rules inside it.

struct GroupSection {
	GroupType type = GroupType::Channel;
	QString title;
	bool *persistedOpen = nullptr;
	Fn<void()> persistOpen;
	not_null<Ui::SlideWrap<Ui::VerticalLayout>*> headerWrap;
	not_null<Ui::SettingsButton*> button;
	not_null<GroupToggles*> toggles;
	not_null<rpl::variable<QString>*> titleText;
	not_null<Ui::SlideWrap<Ui::VerticalLayout>*> wrap;
	not_null<Ui::VerticalLayout*> inner;
};

[[nodiscard]] GroupSection MakeGroupSection(
		not_null<Ui::VerticalLayout*> list,
		GroupType type,
		QString title,
		bool *persistedOpen,
		Fn<void()> persistOpen) {
	const auto headerWrap = list->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			list,
			object_ptr<Ui::VerticalLayout>(list)));
	const auto header = headerWrap->entity();
	const auto toggles = list->lifetime().make_state<GroupToggles>();
	const auto titleText = list->lifetime().make_state<rpl::variable<QString>>(
		title);
	const auto button = header->add(
		object_ptr<Ui::SettingsButton>(
			header,
			titleText->value(),
			st::bearFilterGroupHeader));
	const auto wrap = list->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			list,
			object_ptr<Ui::VerticalLayout>(list)));
	wrap->setDuration(kGroupToggleDuration);
	toggles->open = *persistedOpen;
	toggles->progress = toggles->open ? 1. : 0.;
	AddGroupHeader(
		button,
		toggles,
		persistedOpen,
		[=] {
			wrap->toggle(toggles->open, anim::type::normal);
			persistOpen();
		});
	return {
		.type = type,
		.title = std::move(title),
		.persistedOpen = persistedOpen,
		.persistOpen = std::move(persistOpen),
		.headerWrap = headerWrap,
		.button = button,
		.toggles = toggles,
		.titleText = titleText,
		.wrap = wrap,
		.inner = wrap->entity(),
	};
}

void FillGroupSection(
		not_null<Ui::GenericBox*> box,
		not_null<Ui::VerticalLayout*> contentRoot,
		not_null<Main::Session*> session,
		const GroupSection &section,
		not_null<SelectionUi*> selection,
		const Fn<void(int contentY)> &beforeRemove,
		SortMode sortMode,
		const QString &query,
		int &shownTotal) {
	const auto &rules = Rules();
	auto indices = std::vector<int>();
	for (auto i = 0; i != int(rules.size()); ++i) {
		const auto &rule = rules[i];
		if (GroupOfRule(rule) == section.type && MatchesQuery(rule, query)) {
			indices.push_back(i);
		}
	}
	SortRuleIndices(indices, sortMode);
	*section.titleText = section.title + u" (%1)"_q.arg(int(indices.size()));

	section.inner->clear();
	for (const auto index : indices) {
		AddRuleRow(
			box,
			contentRoot,
			section.inner,
			session,
			selection,
			beforeRemove,
			index,
			rules[index]);
	}

	const auto has = !indices.empty();
	const auto open = section.toggles->open || !query.isEmpty();
	section.headerWrap->toggle(has, anim::type::instant);
	section.wrap->toggle(has && open, anim::type::instant);
	if (has) {
		shownTotal += int(indices.size());
	}
}

void RebuildList(
		not_null<Ui::GenericBox*> box,
		not_null<Ui::VerticalLayout*> contentRoot,
		not_null<Ui::VerticalLayout*> list,
		not_null<Ui::FlatLabel*> empty,
		not_null<Main::Session*> session,
		const std::vector<GroupSection> &sections,
		not_null<SelectionUi*> selection,
		const Fn<void(int contentY)> &beforeRemove,
		SortMode sortMode,
		const QString &query) {
	selection->checks.clear();
	auto shown = 0;
	for (const auto &section : sections) {
		FillGroupSection(
			box,
			contentRoot,
			session,
			section,
			selection,
			beforeRemove,
			sortMode,
			query,
			shown);
	}
	const auto &rules = Rules();
	empty->setText(rules.empty()
		? u"Правил пока нет. Добавьте @username, ID или слово выше, "
			"или выделите текст в сообщении и нажмите правой кнопкой → "
			"«В фильтр постов»."_q
		: (shown ? QString() : u"Ничего не найдено."_q));
	empty->setVisible(!shown);
	list->resizeToWidth(list->width());
}

void ExportRules(not_null<Ui::GenericBox*> box, bool nicknames) {
	const auto weak = base::make_weak(box);
	const auto count = int(Rules().size());
	FileDialog::GetWritePath(
		box.get(),
		u"Экспорт фильтра постов"_q,
		u"JSON (*.json);;Текст (*.txt)"_q,
		QDir::homePath() + u"/beargram_post_filter_%1.json"_q.arg(count),
		[=](QString &&path) {
			const auto text = path.endsWith(u".txt"_q, Qt::CaseInsensitive);
			auto file = QFile(path);
			const auto ok = file.open(QIODevice::WriteOnly | QIODevice::Truncate)
				&& (file.write(text ? ExportText() : ExportJson(nicknames)) >= 0);
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
			if (imported.pins) {
				text += u". Пины: %1 акк."_q.arg(imported.pins);
			}
			if (imported.nicknames) {
				text += u". Никнеймов: %1"_q.arg(imported.nicknames);
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
			true,
			st::defaultBoxCheckbox),
		st::boxRowPadding + QMargins(0, st::boxLittleSkip, 0, 0));

	Ui::AddSkip(container);
	const auto withNicknames = container->lifetime().make_state<bool>(false);
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
	)->setClickedCallback([=] {
		ExportRules(box, *withNicknames);
	});
	const auto nicknamesCheck = container->add(
		object_ptr<Ui::Checkbox>(
			container,
			u"Включить никнеймы в экспорт"_q,
			false,
			st::defaultBoxCheckbox),
		st::boxRowPadding + QMargins(0, st::boxLittleSkip, 0, 0));
	nicknamesCheck->checkedChanges(
	) | rpl::on_next([=](bool on) {
		*withNicknames = on;
	}, container->lifetime());
	Settings::AddButtonWithIcon(
		container,
		rpl::single(u"Очистить все правила"_q),
		st::settingsAttentionButtonWithIcon,
		{ &st::menuIconDeleteAttention }
	)->setClickedCallback([=] { ClearRulesWithConfirm(box); });

	Ui::AddSkip(container);
	const auto rulesTitle = Ui::AddSubsectionTitle(
		container,
		rpl::single(u"Правила"_q));

	const auto keepScroll = container->lifetime().make_state<int>(-1);
	const auto sortMode = container->lifetime().make_state<rpl::variable<SortMode>>(
		SortMode::ByAdded);
	const auto sortText = container->lifetime().make_state<rpl::variable<QString>>(
		SortModeText(SortMode::ByAdded));
	const auto selectionUi = container->lifetime().make_state<SelectionUi>();
	const auto requestRebuild = container->lifetime().make_state<Fn<void()>>();

	const auto toolbarWrap = container->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			container,
			object_ptr<Ui::VerticalLayout>(container)));
	const auto toolbar = toolbarWrap->entity();
	const auto sortButton = toolbar->add(
		object_ptr<Ui::SettingsButton>(
			toolbar,
			sortText->value(),
			st::settingsButtonNoIcon));
	const auto selectButton = toolbar->add(
		object_ptr<Ui::SettingsButton>(
			toolbar,
			rpl::single(u"Выделить"_q),
			st::settingsButtonNoIcon));

	const auto selectionWrap = container->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			container,
			object_ptr<Ui::VerticalLayout>(container)));
	const auto selectionPanel = selectionWrap->entity();
	const auto selectAllButton = selectionPanel->add(
		object_ptr<Ui::SettingsButton>(
			selectionPanel,
			rpl::single(u"Выделить всё"_q),
			st::settingsButtonNoIcon));
	const auto deleteSelectedButton = selectionPanel->add(
		object_ptr<Ui::SettingsButton>(
			selectionPanel,
			selectionUi->deleteText.value(),
			st::settingsAttentionButton));
	deleteSelectedButton->setEnabled(false);
	selectionUi->deleteButton = deleteSelectedButton;
	const auto cancelSelectionButton = selectionPanel->add(
		object_ptr<Ui::SettingsButton>(
			selectionPanel,
			rpl::single(u"Отмена"_q),
			st::settingsButtonNoIcon));

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

	const auto groupsOpen = container->lifetime().make_state<FilterGroupsOpen>(
		GroupsOpen());
	const auto saveGroups = [=] {
		SetGroupsOpen(*groupsOpen);
	};
	const auto sections = container->lifetime().make_state<std::vector<GroupSection>>();
	sections->push_back(MakeGroupSection(
		list,
		GroupType::Channel,
		u"Каналы и группы"_q,
		&groupsOpen->channels,
		saveGroups));
	sections->push_back(MakeGroupSection(
		list,
		GroupType::User,
		u"Пользователи"_q,
		&groupsOpen->users,
		saveGroups));
	sections->push_back(MakeGroupSection(
		list,
		GroupType::Word,
		u"Слова"_q,
		&groupsOpen->words,
		saveGroups));

	const auto beforeRemove = [=](int contentY) {
		// The scroll is clamped to zero while the list is rebuilt, keep
		// the position: skip the height of the row removed above it.
		const auto top = box->scrollTop();
		*keepScroll = top - ((contentY < top) ? kRowHeight : 0);
	};
	const auto rebuild = [=] {
		if (*keepScroll < 0) {
			*keepScroll = box->scrollTop();
		}
		RebuildList(
			box,
			container,
			list,
			empty,
			session,
			*sections,
			selectionUi,
			beforeRemove,
			sortMode->current(),
			search->getLastText().trimmed());
		rulesTitle->setText(u"Правила (%1)"_q.arg(Rules().size()));
		if (const auto target = std::exchange(*keepScroll, -1); target > 0) {
			box->scrollToY(target);
		}
	};
	*requestRebuild = rebuild;
	rebuild();
	RulesChanges() | rpl::on_next(rebuild, list->lifetime());
	search->changes() | rpl::on_next(rebuild, search->lifetime());

	sortButton->setClickedCallback([=] {
		*sortMode = NextSortMode(sortMode->current());
		*sortText = SortModeText(sortMode->current());
		(*requestRebuild)();
	});
	selectButton->setClickedCallback([=] {
		selectionUi->selecting = true;
	});
	selectAllButton->setClickedCallback([=] {
		const auto query = search->getLastText().trimmed();
		const auto &rules = Rules();
		auto keys = base::flat_set<RuleKey>();
		for (auto i = 0; i != int(rules.size()); ++i) {
			const auto &rule = rules[i];
			if (!MatchesQuery(rule, query)) {
				continue;
			}
			for (const auto &section : *sections) {
				// Folded groups are invisible, not a part of "select all".
				if (GroupOfRule(rule) == section.type
					&& section.toggles->open) {
					keys.insert(FromRule(rule));
					break;
				}
			}
		}
		auto allSelected = !keys.empty();
		for (const auto &key : keys) {
			if (!selectionUi->selected.contains(key)) {
				allSelected = false;
				break;
			}
		}
		if (allSelected) {
			selectionUi->selected.clear();
		} else {
			selectionUi->selected = std::move(keys);
		}
		for (const auto &[check, key] : selectionUi->checks) {
			check->setChecked(
				selectionUi->selected.contains(key),
				Ui::Checkbox::NotifyAboutChange::DontNotify);
		}
		RefreshSelectionUi(selectionUi);
	});
	deleteSelectedButton->setClickedCallback([=] {
		auto indices = std::vector<int>();
		const auto &rules = Rules();
		for (auto i = 0; i != int(rules.size()); ++i) {
			if (selectionUi->selected.contains(FromRule(rules[i]))) {
				indices.push_back(i);
			}
		}
		if (indices.empty()) {
			return;
		}
		box->uiShow()->show(Ui::MakeConfirmBox({
			.text = u"Удалить выделенные правила (%1)?"_q.arg(indices.size()),
			.confirmed = [=](Fn<void()> close) {
				*keepScroll = box->scrollTop();
				RemoveRuleIndices(indices);
				selectionUi->selected.clear();
				selectionUi->selecting = false;
				RefreshSelectionUi(selectionUi);
				close();
			},
			.confirmText = u"Удалить"_q,
			.confirmStyle = &st::attentionBoxButton,
		}));
	});
	cancelSelectionButton->setClickedCallback([=] {
		selectionUi->selected.clear();
		selectionUi->selecting = false;
		for (const auto &[check, key] : selectionUi->checks) {
			check->setChecked(
				false,
				Ui::Checkbox::NotifyAboutChange::DontNotify);
		}
		RefreshSelectionUi(selectionUi);
	});

	selectionUi->selecting.value(
	) | rpl::on_next([=](bool on) {
		toolbarWrap->toggle(!on, anim::type::instant);
		selectionWrap->toggle(on, anim::type::instant);
	}, toolbarWrap->lifetime());

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
