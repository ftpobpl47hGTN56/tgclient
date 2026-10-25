/*
This file is part of BearGram, a personal build of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "beargram/beargram_long_messages.h"

#include "beargram/beargram_settings.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "ui/text/text.h"
#include "ui/click_handler.h"

namespace BearGram {
namespace {

constexpr auto kMaxCachedTexts = 4096;

struct LinesInfo {
	int width = 0;
	int height = 0;
	int lines = 0;
	std::vector<int> bottoms;
};

[[nodiscard]] base::flat_set<FullMsgId> &Expanded() {
	static auto result = base::flat_set<FullMsgId>();
	return result;
}

[[nodiscard]] const LinesInfo &CountLines(
		const Ui::Text::String &text,
		int width,
		int height,
		int keepBottoms) {
	static auto cache = base::flat_map<const void*, LinesInfo>();
	const auto key = static_cast<const void*>(&text);
	if (const auto i = cache.find(key); i != end(cache)) {
		const auto &info = i->second;
		if (info.width == width
			&& info.height == height
			&& int(info.bottoms.size()) >= std::min(keepBottoms, info.lines)) {
			return info;
		}
	}
	if (int(cache.size()) >= kMaxCachedTexts) {
		cache.clear();
	}
	const auto geometry = text.countLinesGeometry(width);
	auto info = LinesInfo{
		.width = width,
		.height = height,
		.lines = int(geometry.size()),
	};
	const auto keep = std::min(keepBottoms, info.lines);
	info.bottoms.reserve(keep);
	for (auto i = 0; i != keep; ++i) {
		info.bottoms.push_back(geometry[i].bottom);
	}
	return cache[key] = std::move(info);
}

} // namespace

CollapsedText ComputeCollapsedText(
		not_null<const HistoryItem*> item,
		const Ui::Text::String &text,
		int textWidth,
		int fullHeight,
		int toggleHeight) {
	const auto &settings = Current();
	if (!settings.collapseLongMessages
		|| textWidth <= 0
		|| fullHeight <= 0
		|| text.isEmpty()) {
		return {};
	}
	const auto lineHeight = text.style()
		? text.style()->font->height
		: 0;
	const auto threshold = settings.collapseAfterLines;
	if (lineHeight <= 0 || fullHeight <= threshold * lineHeight) {
		return {};
	}
	const auto preview = std::clamp(
		settings.collapsedPreviewLines,
		1,
		threshold);
	const auto &info = CountLines(text, textWidth, fullHeight, preview);
	if (info.lines <= threshold || int(info.bottoms.size()) < preview) {
		return {};
	}
	auto result = CollapsedText{
		.collapsible = true,
		.expanded = Expanded().contains(item->fullId()),
		.toggleHeight = toggleHeight,
	};
	result.visibleLines = result.expanded ? 0 : preview;
	result.toggleTop = result.expanded
		? fullHeight
		: info.bottoms[preview - 1];
	result.totalHeight = result.toggleTop + result.toggleHeight;
	return result;
}

CollapsedRich ComputeCollapsedRich(
		not_null<const HistoryItem*> item,
		int fullHeight,
		int lineHeight,
		int toggleHeight) {
	const auto &settings = Current();
	if (!settings.collapseLongMessages
		|| fullHeight <= 0
		|| lineHeight <= 0) {
		return {};
	}
	const auto threshold = settings.collapseAfterLines;
	if (fullHeight <= threshold * lineHeight) {
		return {};
	}
	const auto preview = std::clamp(
		settings.collapsedPreviewLines,
		1,
		threshold);
	auto result = CollapsedRich{
		.collapsible = true,
		.expanded = Expanded().contains(item->fullId()),
		.toggleHeight = toggleHeight,
	};
	result.visibleHeight = result.expanded
		? fullHeight
		: std::min(fullHeight, preview * lineHeight);
	result.toggleTop = result.visibleHeight;
	result.totalHeight = result.toggleTop + result.toggleHeight;
	return result;
}

QString ToggleText(bool expanded) {
	return expanded
		? u"▴ Свернуть"_q
		: u"▾ Развернуть"_q;
}

ClickHandlerPtr ToggleLink(not_null<const HistoryItem*> item) {
	static auto links = base::flat_map<FullMsgId, ClickHandlerPtr>();
	const auto id = item->fullId();
	if (const auto i = links.find(id); i != end(links)) {
		return i->second;
	}
	const auto weak = base::make_weak(&item->history()->session());
	auto result = std::make_shared<LambdaClickHandler>([=] {
		const auto session = weak.get();
		if (!session) {
			return;
		}
		auto &expanded = Expanded();
		if (!expanded.remove(id)) {
			expanded.emplace(id);
		}
		if (const auto found = session->data().message(id)) {
			session->data().requestItemResize(found);
		}
	});
	links.emplace(id, result);
	return result;
}

} // namespace BearGram
