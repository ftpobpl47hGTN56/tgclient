/*
This file is part of BearGram, a personal build of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class PeerData;

namespace Ui {
class ChatTheme;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace BearGram {

// Local chat wallpapers: a picture for any chat (groups and channels too)
// that only this app shows. Nothing is sent to the server. Pictures are
// copied to tdata/beargram_wallpapers, the list is in
// tdata/beargram_wallpapers.json by "<self>:<peer>".

[[nodiscard]] bool HasLocalWallpaper(not_null<PeerData*> peer);
void ChooseLocalWallpaper(
	not_null<Window::SessionController*> controller,
	not_null<PeerData*> peer);
void ResetLocalWallpaper(not_null<PeerData*> peer);

// Emits the chat theme with the local wallpaper (fallback while it is
// being prepared), or nullptr when the chat has no local wallpaper.
// Fires again when the local wallpaper of this chat is changed.
[[nodiscard]] rpl::producer<std::shared_ptr<Ui::ChatTheme>> LocalWallpaperThemeValue(
	not_null<PeerData*> peer,
	std::shared_ptr<Ui::ChatTheme> fallback);

} // namespace BearGram
