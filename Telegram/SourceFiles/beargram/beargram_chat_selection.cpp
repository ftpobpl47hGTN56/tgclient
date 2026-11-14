/*
This file is part of BearGram, a personal build of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "beargram/beargram_chat_selection.h"

#include "data/data_session.h"
#include "history/history.h"
#include "main/main_session.h"
#include "ui/painter.h"
#include "styles/style_widgets.h"

namespace BearGram::ChatSelection {
namespace {

struct State {
	base::weak_ptr<Main::Session> session;
	base::flat_set<PeerId> peers;
	rpl::event_stream<> changes;
};

[[nodiscard]] State &Instance() {
	static auto result = State();
	return result;
}

// Forgets a selection made in an account that is gone.
[[nodiscard]] Main::Session *Session() {
	auto &state = Instance();
	const auto session = state.session.get();
	if (!session && !state.peers.empty()) {
		state.peers.clear();
	}
	return session;
}

} // namespace

bool Contains(not_null<History*> history) {
	auto &state = Instance();
	return (Session() == &history->session())
		&& state.peers.contains(history->peer->id);
}

int Count() {
	return Session() ? int(Instance().peers.size()) : 0;
}

std::vector<not_null<History*>> List() {
	auto result = std::vector<not_null<History*>>();
	const auto session = Session();
	if (!session) {
		return result;
	}
	for (const auto &id : Instance().peers) {
		if (const auto history = session->data().historyLoaded(id)) {
			result.push_back(history);
		}
	}
	return result;
}

void Toggle(not_null<History*> history) {
	auto &state = Instance();
	if (Session() != &history->session()) {
		state.peers.clear();
		state.session = base::make_weak(&history->session());
	}
	const auto id = history->peer->id;
	if (!state.peers.remove(id)) {
		state.peers.emplace(id);
	}
	state.changes.fire({});
}

void Clear() {
	auto &state = Instance();
	if (!state.peers.empty()) {
		state.peers.clear();
		state.changes.fire({});
	}
}

rpl::producer<> Changes() {
	return Instance().changes.events();
}

void PaintMark(
		QPainter &p,
		int width,
		int height,
		QRect userpic,
		bool selected) {
	auto hq = PainterHighQualityEnabler(p);
	if (selected) {
		auto tint = st::windowBgActive->c;
		tint.setAlphaF(0.16);
		p.fillRect(0, 0, width, height, tint);
	}

	const auto size = std::max(userpic.width() * 2 / 5, 16);
	const auto stroke = std::max(size / 9., 1.5);
	const auto check = QRectF(
		userpic.x() + userpic.width() - size + stroke,
		userpic.y() + userpic.height() - size + stroke,
		size - stroke,
		size - stroke);
	if (!selected) {
		// An empty checkbox: a white circle with a gray ring.
		p.setPen(QPen(st::windowSubTextFg->c, stroke));
		p.setBrush(st::windowBg->c);
		p.drawEllipse(check);
		return;
	}
	p.setPen(QPen(st::windowBg->c, stroke));
	p.setBrush(st::windowBgActive->c);
	p.drawEllipse(check);

	auto pen = QPen(st::windowFgActive->c, stroke);
	pen.setCapStyle(Qt::RoundCap);
	pen.setJoinStyle(Qt::RoundJoin);
	p.setPen(pen);
	p.setBrush(Qt::NoBrush);
	const auto x = check.x();
	const auto y = check.y();
	const auto w = check.width();
	const auto h = check.height();
	p.drawPolyline(QPolygonF{
		QPointF(x + w * 0.28, y + h * 0.52),
		QPointF(x + w * 0.44, y + h * 0.68),
		QPointF(x + w * 0.73, y + h * 0.36),
	});
}

} // namespace BearGram::ChatSelection
