/*
This file is part of BearGram, a personal build of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "beargram/beargram_counters.h"

#include "data/data_channel.h"
#include "data/data_session.h"
#include "dialogs/dialogs_key.h"
#include "history/history.h"
#include "main/main_session.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "styles/style_dialogs.h"
#include "styles/style_widgets.h"

namespace BearGram::Counters {
namespace {

constexpr auto kBarHeight = 28;
constexpr auto kSkip = 6;

struct Pinned {
	int channels = 0;
	int groups = 0;
	int chats = 0;

	[[nodiscard]] int total() const {
		return channels + groups + chats;
	}
	friend inline bool operator==(const Pinned &, const Pinned &) = default;
};

[[nodiscard]] Pinned CountPinned(not_null<Main::Session*> session) {
	auto result = Pinned();
	for (const auto &key : session->data().pinnedChatsOrder(nullptr)) {
		const auto history = key.history();
		if (!history) {
			continue;
		}
		const auto peer = history->peer;
		if (peer->isUser()) {
			++result.chats;
		} else if (const auto channel = peer->asChannel()
			; channel && !channel->isMegagroup()) {
			++result.channels;
		} else {
			++result.groups;
		}
	}
	return result;
}

} // namespace

int PillWidth(const style::font &font, int count) {
	return font->width(QString::number(count)) + font->height;
}

int PaintPill(
		QPainter &p,
		int left,
		int top,
		int outerWidth,
		const style::font &font,
		const QColor &color,
		int count) {
	const auto text = QString::number(count);
	const auto width = PillWidth(font, count);
	const auto height = font->height + 2;
	const auto rect = style::rtlrect(left, top, width, height, outerWidth);
	{
		auto hq = PainterHighQualityEnabler(p);
		auto fill = color;
		fill.setAlphaF(color.alphaF() * 0.12);
		auto pen = QPen(color);
		pen.setWidthF(style::ConvertScaleExact(1.2));
		p.setPen(pen);
		p.setBrush(fill);
		const auto radius = height / 2.;
		p.drawRoundedRect(QRectF(rect).marginsRemoved(
			{ 0.6, 0.6, 0.6, 0.6 }), radius, radius);
	}
	p.setPen(color);
	p.setFont(font);
	p.drawText(rect, style::al_center, text);
	return width;
}

void PaintSeparator(QPainter &p, int left, int right, int y) {
	if (right <= left) {
		return;
	}
	auto hq = PainterHighQualityEnabler(p);
	const auto line = style::ConvertScaleExact(1.5);
	auto pen = QPen(QColor(0xF5, 0xC5, 0x18, 0xD0));
	pen.setWidthF(line);
	pen.setCapStyle(Qt::RoundCap);
	p.setPen(pen);
	p.drawLine(QPointF(left + line, y), QPointF(right - line, y));
}

object_ptr<Ui::RpWidget> CreatePinnedBar(
		QWidget *parent,
		not_null<Main::Session*> session,
		rpl::producer<bool> shown) {
	auto result = object_ptr<Ui::RpWidget>(parent);
	const auto raw = result.data();
	const auto state = raw->lifetime().make_state<Pinned>();
	const auto visible = raw->lifetime().make_state<rpl::variable<bool>>(
		std::move(shown));
	const auto refresh = [=] {
		const auto now = CountPinned(session);
		const auto height = (now.total() && visible->current())
			? kBarHeight
			: 0;
		if (*state != now || raw->height() != height) {
			*state = now;
			raw->resize(raw->width(), height);
			raw->update();
		}
	};
	session->data().pinnedDialogsOrderUpdated(
	) | rpl::on_next(refresh, raw->lifetime());
	session->data().chatsListChanges(
	) | rpl::on_next(refresh, raw->lifetime());
	visible->changes() | rpl::on_next(refresh, raw->lifetime());
	refresh();

	raw->paintRequest() | rpl::on_next([=] {
		auto p = QPainter(raw);
		p.fillRect(raw->rect(), st::dialogsBg);
		if (!state->total()) {
			return;
		}
		const auto &font = st::normalFont;
		const auto &bold = st::semiboldFont;
		const auto top = (raw->height() - font->height - 2) / 2;
		const auto baseline = top + 1 + font->ascent;
		const auto label = st::windowSubTextFg->c;
		const auto accent = st::windowActiveTextFg->c;
		auto x = st::defaultDialogRow.padding.left() + kSkip;
		const auto text = [&](const QString &value) {
			p.setFont(font);
			p.setPen(label);
			p.drawText(x, baseline, value);
			x += font->width(value) + kSkip;
		};
		const auto pill = [&](int count) {
			x += PaintPill(p, x, top, raw->width(), bold, accent, count);
			x += kSkip * 2;
		};
		text(u"📌"_q);
		pill(state->total());
		const auto part = [&](const QString &name, int count) {
			if (count) {
				text(name);
				pill(count);
			}
		};
		part(u"каналы"_q, state->channels);
		part(u"группы"_q, state->groups);
		part(u"переписки"_q, state->chats);

		p.fillRect(
			0,
			raw->height() - st::lineWidth,
			raw->width(),
			st::lineWidth,
			st::shadowFg);
	}, raw->lifetime());
	return result;
}

} // namespace BearGram::Counters
