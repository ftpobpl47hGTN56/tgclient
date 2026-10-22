/*
This file is part of BearGram, a personal build of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "beargram/beargram_filters.h"

#include "api/api_blocked_peers.h"
#include "apiwrap.h"
#include "beargram/beargram_local_hide.h"
#include "beargram/beargram_settings.h"
#include "data/data_channel.h"
#include "data/data_peer.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"

#include <QtCore/QRegularExpression>

namespace BearGram {
namespace {

constexpr auto kMaxCachedAds = 8192;

struct BlockedList {
	base::flat_set<PeerId> ids;
	bool requested = false;
};

struct AdCheck {
	int signature = 0;
	bool ad = false;
};

[[nodiscard]] BlockedList &BlockedFor(not_null<Main::Session*> session) {
	static auto lists = base::flat_map<PeerId, BlockedList>();
	return lists[session->userPeerId()];
}

void LoadBlocked(
		not_null<Main::Session*> session,
		int offset) {
	const auto weak = base::make_weak(session);
	session->api().blockedPeers().request(offset, [=](
			Api::BlockedPeers::Slice slice) {
		const auto strong = weak.get();
		if (!strong) {
			return;
		}
		auto &list = BlockedFor(strong);
		for (const auto &item : slice.list) {
			list.ids.emplace(item.id);
		}
		const auto loaded = offset + int(slice.list.size());
		if (!slice.list.isEmpty() && loaded < slice.total) {
			LoadBlocked(strong, loaded);
		}
	});
}

[[nodiscard]] bool IsBlockedSender(not_null<const HistoryItem*> item) {
	const auto from = item->from();
	if (from->isBlocked()) {
		return true;
	} else if (from->blockStatus() != PeerData::BlockStatus::Unknown) {
		return false;
	}
	const auto session = &item->history()->session();
	auto &list = BlockedFor(session);
	if (!list.requested) {
		list.requested = true;
		LoadBlocked(session, 0);
	}
	return list.ids.contains(from->id);
}

[[nodiscard]] bool HasAdMarking(const TextWithEntities &text) {
	static const auto erid = QRegularExpression(
		u"(^|[^\\w])erid\\s*[:=]?\\s*\\w{6,}"_q,
		QRegularExpression::CaseInsensitiveOption
			| QRegularExpression::UseUnicodePropertiesOption);
	static const auto hashtag = QRegularExpression(
		u"(^|\\s)#(реклама|промо)(\\s|$)"_q,
		QRegularExpression::CaseInsensitiveOption
			| QRegularExpression::UseUnicodePropertiesOption);
	static const auto legal = QRegularExpression(
		u"(^|\\n)\\s*Реклама[.,:\\s][^\\n]*ИНН"_q,
		QRegularExpression::CaseInsensitiveOption
			| QRegularExpression::UseUnicodePropertiesOption);
	static const auto footer = QRegularExpression(
		u"(^|\\n)\\s*#?Реклама\\.?\\s*$"_q,
		QRegularExpression::CaseInsensitiveOption
			| QRegularExpression::UseUnicodePropertiesOption);
	if (text.text.contains(erid)
		|| text.text.contains(hashtag)
		|| text.text.contains(legal)
		|| text.text.contains(footer)) {
		return true;
	}
	for (const auto &entity : text.entities) {
		if (entity.type() == EntityType::CustomUrl
			&& entity.data().contains(u"erid="_q, Qt::CaseInsensitive)) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] bool IsAdPost(not_null<const HistoryItem*> textItem) {
	static auto cache = base::flat_map<FullMsgId, AdCheck>();
	const auto &text = textItem->originalText();
	const auto signature = int(text.text.size() * 31 + text.entities.size());
	const auto id = textItem->fullId();
	if (const auto i = cache.find(id); i != end(cache)) {
		if (i->second.signature == signature) {
			return i->second.ad;
		}
	}
	if (int(cache.size()) >= kMaxCachedAds) {
		cache.clear();
	}
	const auto ad = HasAdMarking(text);
	cache[id] = AdCheck{ .signature = signature, .ad = ad };
	return ad;
}

[[nodiscard]] bool HasClosedComments(not_null<const HistoryItem*> item) {
	const auto channel = item->history()->peer->asBroadcast();
	if (!channel
		|| !channel->discussionLinkKnown()
		|| !channel->discussionLink()
		|| item->isPinned()) {
		return false;
	}
	return !item->repliesAreComments();
}

} // namespace

bool IsFilteredOut(
		not_null<const HistoryItem*> item,
		const HistoryItem *textItem) {
	if (IsHiddenLocally(item)
		|| (textItem && textItem != item && IsHiddenLocally(textItem))) {
		return true;
	} else if (item->out() || !item->isRegular()) {
		return false;
	}
	const auto &settings = Current();
	if (settings.hideBlockedUsers
		&& !item->history()->peer->isUser()
		&& IsBlockedSender(item)) {
		return true;
	}
	if (settings.hideNoCommentsPosts && HasClosedComments(item)) {
		return true;
	}
	const auto source = textItem ? textItem : item.get();
	return settings.hideAdPosts && IsAdPost(source);
}

} // namespace BearGram
