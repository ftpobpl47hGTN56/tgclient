/*
This file is part of BearGram, a personal build of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "beargram/beargram_peer_info.h"

#include "apiwrap.h"
#include "base/unixtime.h"
#include "data/data_channel.h"
#include "data/data_session.h"
#include "history/history_item.h"
#include "main/main_session.h"

namespace BearGram::PeerInfo {
namespace {

struct Point {
	int64 id = 0;
	int64 date = 0; // Unixtime.
};

// Monotone (id, date) points, see beargram_peer_info.h.
constexpr Point kPoints[] = {
	{ 0LL, 1376438400 },
	{ 2768409LL, 1383264000 },
	{ 7679610LL, 1388448000 },
	{ 11538514LL, 1391212800 },
	{ 23646077LL, 1393372800 },
	{ 44634663LL, 1399334400 },
	{ 54845238LL, 1411171200 },
	{ 63263518LL, 1414368000 },
	{ 101260938LL, 1425600000 },
	{ 103204956LL, 1432987200 },
	{ 117597704LL, 1438300800 },
	{ 127929227LL, 1442620800 },
	{ 143445125LL, 1448928000 },
	{ 152956184LL, 1450800000 },
	{ 171295414LL, 1457481600 },
	{ 181783990LL, 1460246400 },
	{ 222021233LL, 1465344000 },
	{ 278941742LL, 1473465600 },
	{ 285253072LL, 1476748800 },
	{ 294851037LL, 1479513600 },
	{ 297621225LL, 1481846400 },
	{ 328594461LL, 1485561600 },
	{ 337808429LL, 1487635200 },
	{ 369669043LL, 1490918400 },
	{ 400169472LL, 1501459200 },
	{ 805158066LL, 1563148800 },
	{ 1974255900LL, 1633996800 },
	{ 5027173742LL, 1638921600 },
	{ 5045293264LL, 1642032000 },
	{ 5058656439LL, 1645833600 },
	{ 5266729977LL, 1648454400 },
	{ 5308260177LL, 1650844800 },
	{ 5387226589LL, 1655694000 },
	{ 5474282362LL, 1658435657 },
	{ 5531378275LL, 1660240800 },
	{ 5578621357LL, 1664409600 },
	{ 5767004208LL, 1670630400 },
	{ 5832370132LL, 1674561600 },
	{ 5926866061LL, 1676851200 },
	{ 6136110037LL, 1682848800 },
	{ 6326011828LL, 1688688000 },
	{ 6462226143LL, 1698969600 },
	{ 6540611293LL, 1703937600 },
	{ 6694418617LL, 1705894892 },
	{ 6909688956LL, 1707652800 },
	{ 7002435197LL, 1712361600 },
	{ 7091188196LL, 1718179200 },
	{ 7248991615LL, 1721260800 },
	{ 7283525500LL, 1723564800 },
	{ 7375779833LL, 1725904800 },
	{ 7454492493LL, 1727827200 },
	{ 7692193285LL, 1736363520 },
	{ 7824631473LL, 1738419840 },
	{ 7866754510LL, 1749729600 },
	{ 8179125032LL, 1752019200 },
	{ 8284801005LL, 1757088000 },
	{ 8423113779LL, 1760025600 },
	{ 8480708838LL, 1762300800 },
	{ 8559682245LL, 1762819200 }
};

// After the last point ids keep growing at the speed of its last year.
constexpr auto kSlopeFrom = 51; // Index of the point a year before the last.

[[nodiscard]] QString MonthName(int month) {
	static const auto names = std::array<QString, 12>{
		u"январь"_q, u"февраль"_q, u"март"_q, u"апрель"_q,
		u"май"_q, u"июнь"_q, u"июль"_q, u"август"_q,
		u"сентябрь"_q, u"октябрь"_q, u"ноябрь"_q, u"декабрь"_q,
	};
	return names[std::clamp(month, 1, 12) - 1];
}

[[nodiscard]] QString MonthNameOf(int month) {
	static const auto names = std::array<QString, 12>{
		u"января"_q, u"февраля"_q, u"марта"_q, u"апреля"_q,
		u"мая"_q, u"июня"_q, u"июля"_q, u"августа"_q,
		u"сентября"_q, u"октября"_q, u"ноября"_q, u"декабря"_q,
	};
	return names[std::clamp(month, 1, 12) - 1];
}

[[nodiscard]] int64 EstimateDate(int64 id) {
	constexpr auto count = int(std::size(kPoints));
	for (auto i = 1; i != count; ++i) {
		const auto &a = kPoints[i - 1];
		const auto &b = kPoints[i];
		if (id <= b.id) {
			if (id <= a.id || b.id == a.id) {
				return a.date;
			}
			const auto part = (id - a.id) / double(b.id - a.id);
			return a.date + int64(part * (b.date - a.date));
		}
	}
	const auto &from = kPoints[kSlopeFrom];
	const auto &last = kPoints[count - 1];
	const auto perId = (last.date - from.date) / double(last.id - from.id);
	return last.date + int64((id - last.id) * perId);
}

[[nodiscard]] QString DateText(TimeId date) {
	const auto parsed = base::unixtime::parse(date).date();
	return QString::number(parsed.day())
		+ ' '
		+ MonthNameOf(parsed.month())
		+ ' '
		+ QString::number(parsed.year());
}

} // namespace

QString EstimatedRegistrationText(uint64 bareUserId) {
	if (!bareUserId) {
		return QString();
	}
	const auto date = std::min(
		EstimateDate(int64(bareUserId)),
		int64(base::unixtime::now()));
	const auto parsed = base::unixtime::parse(TimeId(date)).date();
	return u"≈ "_q
		+ MonthName(parsed.month())
		+ ' '
		+ QString::number(parsed.year())
		+ u" (оценка по ID)"_q;
}

rpl::producer<QString> ChannelCreatedValue(not_null<ChannelData*> channel) {
	return [=](auto consumer) {
		auto lifetime = rpl::lifetime();
		const auto session = &channel->session();
		const auto first = MsgId(1);
		const auto put = [=] {
			if (const auto item = session->data().message(channel, first)) {
				consumer.put_next(DateText(item->date()));
				return true;
			}
			return false;
		};
		consumer.put_next(QString());
		if (!put()) {
			const auto done = std::make_shared<bool>(false);
			lifetime.add([=] { *done = true; });
			session->api().requestMessageData(channel, first, [=] {
				if (!*done) {
					put();
				}
			});
		}
		return lifetime;
	};
}

} // namespace BearGram::PeerInfo
