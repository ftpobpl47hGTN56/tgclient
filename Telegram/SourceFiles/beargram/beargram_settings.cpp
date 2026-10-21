/*
This file is part of BearGram, a personal build of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "beargram/beargram_settings.h"

#include "settings.h"

#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

namespace BearGram {
namespace {

constexpr auto kMinLines = 3;
constexpr auto kMaxLines = 200;

[[nodiscard]] QString SettingsPath() {
	return cWorkingDir() + u"tdata/beargram.json"_q;
}

[[nodiscard]] Settings Load() {
	auto result = Settings();
	auto file = QFile(SettingsPath());
	if (!file.open(QIODevice::ReadOnly)) {
		return result;
	}
	const auto object = QJsonDocument::fromJson(file.readAll()).object();
	const auto readInt = [&](const QString &key, int fallback) {
		const auto value = object.value(key).toInt(fallback);
		return std::clamp(value, kMinLines, kMaxLines);
	};
	result.collapseLongMessages = object.value(
		u"collapseLongMessages"_q).toBool(result.collapseLongMessages);
	result.hideBlockedUsers = object.value(
		u"hideBlockedUsers"_q).toBool(result.hideBlockedUsers);
	result.hideAdPosts = object.value(
		u"hideAdPosts"_q).toBool(result.hideAdPosts);
	result.collapseAfterLines = readInt(
		u"collapseAfterLines"_q,
		result.collapseAfterLines);
	result.collapsedPreviewLines = readInt(
		u"collapsedPreviewLines"_q,
		result.collapsedPreviewLines);
	return result;
}

void Save(const Settings &settings) {
	auto object = QJsonObject();
	object.insert(u"collapseLongMessages"_q, settings.collapseLongMessages);
	object.insert(u"collapseAfterLines"_q, settings.collapseAfterLines);
	object.insert(u"hideBlockedUsers"_q, settings.hideBlockedUsers);
	object.insert(u"hideAdPosts"_q, settings.hideAdPosts);
	object.insert(
		u"collapsedPreviewLines"_q,
		settings.collapsedPreviewLines);
	auto file = QFile(SettingsPath());
	if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
	}
}

[[nodiscard]] Settings &Instance() {
	static auto instance = Load();
	return instance;
}

} // namespace

const Settings &Current() {
	return Instance();
}

void Update(Fn<void(Settings&)> change) {
	auto &settings = Instance();
	change(settings);
	settings.collapsedPreviewLines = std::min(
		settings.collapsedPreviewLines,
		settings.collapseAfterLines);
	Save(settings);
}

} // namespace BearGram
