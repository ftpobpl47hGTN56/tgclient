/*
This file is part of BearGram, a personal build of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "beargram/beargram_wallpapers.h"

#include "core/file_utilities.h"
#include "data/data_peer.h"
#include "data/data_cloud_themes.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "settings.h"
#include "ui/chat/chat_theme.h"
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

[[nodiscard]] base::flat_map<QString, QString> Load() {
	auto result = base::flat_map<QString, QString>();
	auto file = QFile(ListPath());
	if (!file.open(QIODevice::ReadOnly)) {
		return result;
	}
	const auto object = QJsonDocument::fromJson(file.readAll()).object();
	for (auto i = object.begin(); i != object.end(); ++i) {
		result.emplace(i.key(), i.value().toString());
	}
	return result;
}

[[nodiscard]] base::flat_map<QString, QString> &List() {
	static auto list = Load();
	return list;
}

void Save() {
	auto object = QJsonObject();
	for (const auto &[key, path] : List()) {
		object.insert(key, path);
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

[[nodiscard]] QString PathFor(not_null<PeerData*> peer) {
	const auto &list = List();
	const auto i = list.find(PeerKey(peer));
	if (i == end(list) || !QFileInfo::exists(i->second)) {
		return QString();
	}
	return i->second;
}

void RemoveFile(const QString &key) {
	const auto i = List().find(key);
	if (i != end(List())) {
		QFile::remove(i->second);
	}
}

struct CachedTheme {
	QString path;
	bool dark = false;
	std::weak_ptr<Ui::ChatTheme> theme;
};

[[nodiscard]] std::vector<CachedTheme> &Cache() {
	static auto cache = std::vector<CachedTheme>();
	return cache;
}

[[nodiscard]] rpl::producer<std::shared_ptr<Ui::ChatTheme>> ThemeForPath(
		QString path,
		bool dark,
		std::shared_ptr<Ui::ChatTheme> fallback) {
	return [=](auto consumer) {
		auto lifetime = rpl::lifetime();
		for (const auto &cached : Cache()) {
			if (cached.path == path && cached.dark == dark) {
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
		auto descriptor = Ui::ChatThemeDescriptor{
			.key = Ui::ChatThemeKey{
				(uint64(qHash(path)) << 1) | 1ULL,
				dark,
			},
			.preparePalette = Window::Theme::PrepareCurrentPaletteCallback(),
			.backgroundData = {
				.key = u"beargram:"_q + path,
				.path = path,
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
			path,
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
				cache.push_back({ path, dark, result });
				consumer.put_next_copy(result);
			});
		});
		return lifetime;
	};
}

} // namespace

bool HasLocalWallpaper(not_null<PeerData*> peer) {
	return !PathFor(peer).isEmpty();
}

void ChooseLocalWallpaper(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer) {
	const auto callback = [=](FileDialog::OpenResult &&result) {
		if (result.paths.isEmpty() && result.remoteContent.isEmpty()) {
			return;
		}
		QDir().mkpath(FilesDir());
		const auto key = PeerKey(peer);
		auto target = FilesDir()
			+ QString(key).replace(':', '_')
			+ '_'
			+ QString::number(crl::now())
			+ u".img"_q;
		auto ok = false;
		if (!result.paths.isEmpty()) {
			ok = QFile::copy(result.paths.front(), target);
		} else {
			auto file = QFile(target);
			ok = file.open(QIODevice::WriteOnly)
				&& (file.write(result.remoteContent) == result.remoteContent.size());
		}
		if (!ok) {
			controller->showToast(u"Не удалось скопировать картинку"_q);
			return;
		}
		RemoveFile(key);
		List()[key] = target;
		Save();
		Changes().fire_copy(key);
	};
	FileDialog::GetOpenPath(
		QPointer<QWidget>(controller->widget().get()),
		tr::lng_choose_image(tr::now),
		FileDialog::ImagesFilter(),
		crl::guard(controller, callback));
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
		const auto path = PathFor(peer);
		if (path.isEmpty()) {
			return rpl::single(std::shared_ptr<Ui::ChatTheme>());
		}
		return ThemeForPath(path, dark, fallback);
	}) | rpl::flatten_latest();
}

} // namespace BearGram
