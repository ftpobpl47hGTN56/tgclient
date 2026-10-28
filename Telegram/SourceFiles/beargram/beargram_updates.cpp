/*
This file is part of BearGram, a personal build of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "beargram/beargram_updates.h"

#include "base/timer.h"
#include "beargram/beargram_settings.h"
#include "core/version.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QPointer>
#include <QtCore/QRegularExpression>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

namespace BearGram {
namespace {

constexpr auto kFirstCheckDelay = crl::time(20 * 1000);
constexpr auto kCheckInterval = crl::time(6 * 60 * 60 * 1000);
constexpr auto kRequestTimeout = 30 * 1000;
constexpr auto kMaxResponseSize = 1024 * 1024;

const auto kLatestReleaseUrl = u"https://api.github.com/repos/"
	"ftpobpl47hGTN56/tgclient/releases/latest"_q;
const auto kReleasesPageUrl = u"https://github.com/"
	"ftpobpl47hGTN56/tgclient/releases"_q;

struct Release {
	int version = 0; // In the AppVersion format: 7003003.
	int build = 0;
	QString versionText;
	QString url;
};

struct State {
	std::unique_ptr<QNetworkAccessManager> manager;
	base::Timer timer;
	QPointer<QNetworkReply> reply;
	std::vector<Fn<void(QString)>> waiting;
	rpl::variable<std::optional<AvailableUpdate>> available;
	bool started = false;
};

// Never destroyed: Qt objects must not outlive QApplication in a static.
[[nodiscard]] State &Instance() {
	static const auto instance = new State();
	return *instance;
}

[[nodiscard]] std::optional<Release> ParseRelease(const QByteArray &data) {
	const auto object = QJsonDocument::fromJson(data).object();
	if (object.value(u"draft"_q).toBool()
		|| object.value(u"prerelease"_q).toBool()) {
		return std::nullopt;
	}
	static const auto regex = QRegularExpression(
		u"^beargram-(\\d+)\\.(\\d+)\\.(\\d+)-build(\\d+)$"_q);
	const auto tag = object.value(u"tag_name"_q).toString();
	const auto match = regex.match(tag);
	if (!match.hasMatch()) {
		return std::nullopt;
	}
	const auto major = match.captured(1).toInt();
	const auto minor = match.captured(2).toInt();
	const auto patch = match.captured(3).toInt();
	auto url = object.value(u"html_url"_q).toString();
	if (!url.startsWith(u"https://github.com/"_q)) {
		url = kReleasesPageUrl;
	}
	return Release{
		.version = major * 1000000 + minor * 1000 + patch,
		.build = match.captured(4).toInt(),
		.versionText = u"%1.%2.%3"_q.arg(major).arg(minor).arg(patch),
		.url = url,
	};
}

[[nodiscard]] std::optional<AvailableUpdate> ToUpdate(const Release &release) {
	const auto current = int(AppVersion);
	const auto build = CurrentBuild();
	if (release.version > current) {
		return AvailableUpdate{
			.title = release.versionText,
			.url = release.url,
		};
	} else if (release.version == current
		&& build > 0
		&& release.build > build) {
		return AvailableUpdate{
			.title = u"%1, сборка %2"_q.arg(release.versionText).arg(
				release.build),
			.url = release.url,
		};
	}
	return std::nullopt;
}

void Finish(const QString &result) {
	auto &state = Instance();
	for (const auto &callback : base::take(state.waiting)) {
		if (callback) {
			callback(result);
		}
	}
}

void HandleResponse(QNetworkReply *reply) {
	auto &state = Instance();
	const auto error = reply->error();
	const auto data = (error == QNetworkReply::NoError)
		? reply->read(kMaxResponseSize)
		: QByteArray();
	reply->deleteLater();
	state.reply = nullptr;

	if (error != QNetworkReply::NoError) {
		LOG(("BearGram: update check failed, error %1.").arg(int(error)));
		Finish(u"Не удалось проверить обновления. Проверьте интернет."_q);
		return;
	}
	const auto release = ParseRelease(data);
	if (!release) {
		LOG(("BearGram: update check got no usable release."));
		Finish(u"На GitHub пока нет сборок BearGram."_q);
		return;
	}
	const auto update = ToUpdate(*release);
	state.available = update;
	Finish(update
		? (u"Доступна BearGram "_q + update->title + u"."_q)
		: (u"У вас последняя версия BearGram ("_q
			+ CurrentVersionText()
			+ u")."_q));
}

void Request(Fn<void(QString)> done) {
	auto &state = Instance();
	state.waiting.push_back(std::move(done));
	if (state.reply) {
		return;
	}
	if (!state.manager) {
		state.manager = std::make_unique<QNetworkAccessManager>();
	}
	auto request = QNetworkRequest(QUrl(kLatestReleaseUrl));
	request.setRawHeader("Accept", "application/vnd.github+json");
	request.setRawHeader("User-Agent", "BearGram");
	request.setTransferTimeout(kRequestTimeout);
	const auto reply = state.manager->get(request);
	state.reply = reply;
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		HandleResponse(reply);
	});
}

void CheckOnTimer() {
	auto &state = Instance();
	state.timer.callOnce(kCheckInterval);
	if (Current().checkUpdates) {
		Request(nullptr);
	}
}

} // namespace

int CurrentBuild() {
#ifdef BEARGRAM_BUILD_NUMBER
	return BEARGRAM_BUILD_NUMBER;
#else // BEARGRAM_BUILD_NUMBER
	return 0;
#endif // BEARGRAM_BUILD_NUMBER
}

QString CurrentVersionText() {
	const auto version = QString::fromLatin1(AppVersionStr);
	const auto build = CurrentBuild();
	return build
		? u"%1, сборка %2"_q.arg(version).arg(build)
		: (version + u", локальная сборка"_q);
}

void StartUpdateChecks() {
	auto &state = Instance();
	if (state.started) {
		return;
	}
	state.started = true;
	state.timer.setCallback(CheckOnTimer);
	state.timer.callOnce(kFirstCheckDelay);
}

void CheckUpdatesNow(Fn<void(QString)> done) {
	Request(std::move(done));
}

rpl::producer<const AvailableUpdate*> AvailableUpdateValue() {
	return Instance().available.value(
	) | rpl::map([](const std::optional<AvailableUpdate> &update) {
		// The pointer is used right away by the subscriber.
		return update ? &*Instance().available.current() : nullptr;
	});
}

} // namespace BearGram
