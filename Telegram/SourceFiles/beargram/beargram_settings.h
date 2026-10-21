/*
This file is part of BearGram, a personal build of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace BearGram {

struct Settings {
	bool collapseLongMessages = true;
	int collapseAfterLines = 20;
	int collapsedPreviewLines = 6;
	bool hideBlockedUsers = true;
	bool hideAdPosts = true;
};

[[nodiscard]] const Settings &Current();
void Update(Fn<void(Settings&)> change);

} // namespace BearGram
