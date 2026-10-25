/*
This file is part of BearGram, a personal build of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "beargram/beargram_wallpapers.h"

#include "boxes/background_preview_box.h"
#include "core/file_utilities.h"
#include "data/data_peer.h"
#include "data/data_cloud_themes.h"
#include "data/data_wall_paper.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "mainwindow.h" // MainWindow is a QWidget, needed for the dialog parent.
#include "settings.h"
#include "ui/chat/chat_theme.h"
#include "ui/image/image.h"
#include "ui/image/image_prepare.h"
#include "window/themes/window_theme.h"
#include "window/themes/window_themes_chat.h"
#include "window/themes/window_themes_embedded.h"
#include "window/window_session_controller.h"

#include <QtCore/QDir>
#include <QtCore/QHash>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

namespace BearGram {
namespace {

// One local wallpaper of one chat.
struct Entry {
	QString path;
	bool blurred = false;
	int dimming = 0; // 0..100, applied in the dark mode, like Telegram does.
};

[[nodiscard]] QString ListPath() {
	return cWorkingDir() + u"tdata/beargram_wallpapers.json"_q;
}

[[nodiscard]] QString FilesDir() {
	return cWorkingDir() + u"tdata/beargram_wallpapers/"_q;
}

[[nodiscard]] QString PeerKey(not_null<PeerData*> peer) {
	return QString::number(peer->session().userId().bare)
		+ ':'
		+ QString::number(peer->id.value);
}

[[nodiscard]] base::flat_map<QString, Entry> Load() {
	auto result = base::flat_map<QString, Entry>();
	auto file = QFile(ListPath());
	if (!file.open(QIODevice::ReadOnly)) {
		return result;
	}
	const auto object = QJsonDocument::fromJson(file.readAll()).object();
	for (auto i = object.begin(); i != object.end(); ++i) {
		const auto value = i.value();
		if (value.isString()) {
			// Build 13 stored only the path.
			result.emplace(i.key(), Entry{ .path = value.toString() });
		} else if (value.isObject()) {
			const auto entry = value.toObject();
			result.emplace(i.key(), Entry{
				.path = entry.value(u"path"_q).toString(),
				.blurred = entry.value(u"blurred"_q).toBool(),
				.dimming = std::clamp(
					entry.value(u"dimming"_q).toInt(),
					0,
					100),
			});
		}
	}
	return result;
}

[[nodiscard]] base::flat_map<QString, Entry> &List() {
	static auto list = Load();
	return list;
}

void Save() {
	auto object = QJsonObject();
	for (const auto &[key, entry] : List()) {
		auto value = QJsonObject();
		value.insert(u"path"_q, entry.path);
		value.insert(u"blurred"_q, entry.blurred);
		value.insert(u"dimming"_q, entry.dimming);
		object.insert(key, value);
	}
	auto file = QFile(ListPath());
	if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
	}
}

[[nodiscard]] rpl::event_stream<QString> &Changes() {
	static auto stream = rpl::event_stream<QString>();
	return stream;
}

[[nodiscard]] std::optional<Entry> EntryFor(not_null<PeerData*> peer) {
	const auto &list = List();
	const auto i = list.find(PeerKey(peer));
	if (i == end(list) || !QFileInfo::exists(i->second.path)) {
		return std::nullopt;
	}
	return i->second;
}

void RemoveFile(const QString &key) {
	const auto i = List().find(key);
	if (i != end(List())) {
		QFile::remove(i->second.path);
	}
}

struct CachedTheme {
	QString path;
	bool blurred = false;
	int dimming = 0;
	bool dark = false;
	std::weak_ptr<Ui::ChatTheme> theme;
};

[[nodiscard]] std::vector<CachedTheme> &Cache() {
	static auto cache = std::vector<CachedTheme>();
	return cache;
}

[[nodiscard]] rpl::producer<std::shared_ptr<Ui::ChatTheme>> ThemeForEntry(
		Entry entry,
		bool dark,
		std::shared_ptr<Ui::ChatTheme> fallback) {
	return [=](auto consumer) {
		auto lifetime = rpl::lifetime();
		for (const auto &cached : Cache()) {
			if (cached.path == entry.path
				&& cached.blurred == entry.blurred
				&& cached.dimming == entry.dimming
				&& cached.dark == dark) {
				if (auto strong = cached.theme.lock()) {
					consumer.put_next(std::move(strong));
					return lifetime;
				}
			}
		}
		consumer.put_next_copy(fallback);
		const auto type = dark
			? Data::CloudThemeType::Dark
			: Data::CloudThemeType::Light;

		// The key has to differ for each set of settings, otherwise
		// the chat keeps the theme prepared for the old settings.
		const auto settings = QString::number(entry.blurred ? 1 : 0)
			+ ':'
			+ QString::number(entry.dimming);
		const auto hash = uint64(qHash(entry.path + '|' + settings));
		auto descriptor = Ui::ChatThemeDescriptor{
			.key = Ui::ChatThemeKey{ (hash << 1) | 1ULL, dark },
			.preparePalette = Window::Theme::PrepareCurrentPaletteCallback(),
			.backgroundData = {
				.key = u"beargram:"_q + entry.path + '|' + settings,
				.path = entry.path,
				.darkModeDimming = entry.dimming,
				.isBlurred = entry.blurred,
				.forDarkMode = dark,
			},
			.bubblesData = Window::Theme::PrepareBubblesData(
				Data::CloudTheme(),
				type),
			.basedOnDark = dark,
		};
		const auto guard = lifetime.make_state<base::has_weak_ptr>();
		crl::async([
			descriptor = std::move(descriptor),
			weak = base::make_weak(guard),
			consumer,
			entry,
			dark
		]() mutable {
			auto result = std::make_shared<Ui::ChatTheme>(
				std::move(descriptor));
			crl::on_main(weak, [=]() mutable {
				result->finishCreateOnMain();
				auto &cache = Cache();
				cache.erase(ranges::remove_if(cache, [](const CachedTheme &c) {
					return c.theme.expired();
				}), end(cache));
				cache.push_back({
					entry.path,
					entry.blurred,
					entry.dimming,
					dark,
					result,
				});
				consumer.put_next_copy(result);
			});
		});
		return lifetime;
	};
}

// Shows the official wallpaper preview (blur, dimming, light/dark switch)
// in the local mode. Nothing is sent to the server, the chosen settings
// come back to the done callback.
void ShowPreview(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer,
		QImage image,
		bool blurred,
		int dimming,
		Fn<void(bool blurred, int dimming)> done) {
	auto paper = Data::CustomWallPaper();
	paper.setLocalImageAsThumbnail(std::make_shared<Image>(
		std::move(image)));
	paper = paper.withBlurred(blurred).withPatternIntensity(dimming);
	controller->show(Box<BackgroundPreviewBox>(
		controller,
		paper,
		BackgroundPreviewArgs{
			.forPeer = peer,
			.localApply = std::move(done),
		}));
}

} // namespace

bool HasLocalWallpaper(not_null<PeerData*> peer) {
	return EntryFor(peer).has_value();
}

void ChooseLocalWallpaper(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer) {
	const auto callback = [=](FileDialog::OpenResult &&result) {
		if (result.paths.isEmpty() && result.remoteContent.isEmpty()) {
			return;
		}
		const auto source = result.paths.isEmpty()
			? QString()
			: result.paths.front();
		const auto content = result.remoteContent;
		auto image = Images::Read({
			.path = source,
			.content = content,
			.forceOpaque = true,
		}).image;
		if (image.isNull() || image.width() <= 0 || image.height() <= 0) {
			controller->showToast(u"Не удалось открыть картинку"_q);
			return;
		}

		// The settings of the current wallpaper are the starting point.
		const auto current = EntryFor(peer);
		const auto done = [=](bool blurred, int dimming) {
			QDir().mkpath(FilesDir());
			const auto key = PeerKey(peer);
			const auto target = FilesDir()
				+ QString(key).replace(':', '_')
				+ '_'
				+ QString::number(crl::now())
				+ u".img"_q;
			auto ok = false;
			if (!source.isEmpty()) {
				ok = QFile::copy(source, target);
			} else {
				auto file = QFile(target);
				ok = file.open(QIODevice::WriteOnly)
					&& (file.write(content) == content.size());
			}
			if (!ok) {
				controller->showToast(u"Не удалось скопировать картинку"_q);
				return;
			}
			RemoveFile(key);
			List()[key] = Entry{
				.path = target,
				.blurred = blurred,
				.dimming = dimming,
			};
			Save();
			Changes().fire_copy(key);
		};
		ShowPreview(
			controller,
			peer,
			std::move(image),
			current ? current->blurred : false,
			current ? current->dimming : 0,
			crl::guard(controller, done));
	};
	FileDialog::GetOpenPath(
		QPointer<QWidget>(controller->widget().get()),
		tr::lng_choose_image(tr::now),
		FileDialog::ImagesFilter(),
		crl::guard(controller, callback));
}

void ConfigureLocalWallpaper(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer) {
	const auto current = EntryFor(peer);
	if (!current) {
		return;
	}
	auto image = Images::Read({
		.path = current->path,
		.forceOpaque = true,
	}).image;
	if (image.isNull() || image.width() <= 0 || image.height() <= 0) {
		controller->showToast(u"Не удалось открыть картинку"_q);
		return;
	}
	const auto key = PeerKey(peer);
	const auto path = current->path;
	const auto done = [=](bool blurred, int dimming) {
		const auto i = List().find(key);
		if (i == end(List()) || i->second.path != path) {
			return; // Changed while the preview was open.
		}
		i->second.blurred = blurred;
		i->second.dimming = dimming;
		Save();
		Changes().fire_copy(key);
	};
	ShowPreview(
		controller,
		peer,
		std::move(image),
		current->blurred,
		current->dimming,
		crl::guard(controller, done));
}

void ResetLocalWallpaper(not_null<PeerData*> peer) {
	const auto key = PeerKey(peer);
	RemoveFile(key);
	List().remove(key);
	Save();
	Changes().fire_copy(key);
}

rpl::producer<std::shared_ptr<Ui::ChatTheme>> LocalWallpaperThemeValue(
		not_null<PeerData*> peer,
		std::shared_ptr<Ui::ChatTheme> fallback) {
	const auto key = PeerKey(peer);
	return rpl::single(
		key
	) | rpl::then(Changes().events(
	) | rpl::filter([=](const QString &changed) {
		return changed == key;
	})) | rpl::map([=](const QString &) {
		return Window::Theme::IsThemeDarkValue(
		) | rpl::distinct_until_changed();
	}) | rpl::flatten_latest(
	) | rpl::map([=](bool dark)
	-> rpl::producer<std::shared_ptr<Ui::ChatTheme>> {
		const auto entry = EntryFor(peer);
		if (!entry) {
			return rpl::single(std::shared_ptr<Ui::ChatTheme>());
		}
		return ThemeForEntry(*entry, dark, fallback);
	}) | rpl::flatten_latest();
}

} // namespace BearGram
