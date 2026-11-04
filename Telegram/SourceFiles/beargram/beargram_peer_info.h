/*
This file is part of BearGram, a personal build of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class ChannelData;

// Account and channel age under the ID in profiles.
//
// Telegram does not tell when an account was registered. User ids grow
// over time, so the month is estimated from the id by known (id, date)
// points: "≈ март 2021". The points come from the MIT licensed dataset of
// github.com/WizardLoop/CreationDate, made monotone (isotonic regression)
// because since 2021 ids are given out of several ranges at once. An
// estimate may be a few months off.
//
// A channel or supergroup is created with its message 1, the service
// "channel created" one, so its date is exact when that message exists.
namespace BearGram::PeerInfo {

[[nodiscard]] QString EstimatedRegistrationText(uint64 bareUserId);

// Empty until the first message is loaded, stays empty if it is gone.
[[nodiscard]] rpl::producer<QString> ChannelCreatedValue(
	not_null<ChannelData*> channel);

} // namespace BearGram::PeerInfo
