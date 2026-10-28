/*
This file is part of BearGram, a personal build of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace BearGram {

// Checks GitHub Releases of the BearGram repository for a newer build.
//
// Cloud builds publish releases tagged "beargram-<version>-build<N>". The
// cloud build number is compiled in as BEARGRAM_BUILD_NUMBER, so a newer
// build of the same Telegram version is found too. A local build (number
// 0) only reports releases with a newer Telegram version.

struct AvailableUpdate {
	QString title; // "7.3.4" or "7.3.3, сборка 15".
	QString url; // The release page.

	friend inline bool operator==(
		const AvailableUpdate &,
		const AvailableUpdate &) = default;
};

// Starts the periodic checks (shortly after the start and then every few
// hours) when Settings::checkUpdates is on. Safe to call more than once.
void StartUpdateChecks();

// Checks right now, ignoring Settings::checkUpdates.
// done receives a text for a toast.
void CheckUpdatesNow(Fn<void(QString)> done);

// The newest release found, or nullptr when there is none.
[[nodiscard]] rpl::producer<const AvailableUpdate*> AvailableUpdateValue();

// The cloud build number, or 0 for a local build.
[[nodiscard]] int CurrentBuild();

// "7.3.3, сборка 12" or "7.3.3, локальная сборка".
[[nodiscard]] QString CurrentVersionText();

} // namespace BearGram
