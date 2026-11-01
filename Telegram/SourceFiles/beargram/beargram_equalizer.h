/*
This file is part of BearGram, a personal build of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Ui {
class GenericBox;
} // namespace Ui

// A 10 band equalizer for music (the player at the top, AudioMsgId::Type
// ::Song). Voice messages, videos and calls are not affected.
//
// The bundled FFmpeg is built with the atempo filter only, so the bands are
// BearGram's own peaking filters (RBJ biquads) applied to the 16 bit PCM
// right before it goes to OpenAL, in Media::Player::Loaders::loadData.
// About three seconds of music are decoded ahead, so a change is heard
// after up to three seconds.
//
// The settings are kept in tdata/beargram_equalizer.json.
namespace BearGram::Equalizer {

inline constexpr auto kBands = 10;
inline constexpr auto kMinGain = -12; // dB
inline constexpr auto kMaxGain = 12; // dB

using Gains = std::array<int, kBands>;

// 31 Hz ... 16 kHz, one octave apart.
[[nodiscard]] const std::array<int, kBands> &Frequencies();

[[nodiscard]] bool Enabled();
[[nodiscard]] rpl::producer<bool> EnabledValue();
void SetEnabled(bool enabled);

[[nodiscard]] Gains CurrentGains();
void SetGains(const Gains &gains);

// Filters state for one audio stream, used on the audio loaders thread.
class Processor final {
public:
	// The stream jumped (a new track, a seek, a speed change).
	void reset();

	// Interleaved signed 16 bit samples, 1 or 2 channels, in place.
	void process(QByteArray &pcm, int channels, int frequency);

private:
	struct Biquad {
		double b0 = 1., b1 = 0., b2 = 0., a1 = 0., a2 = 0.;
	};
	struct History {
		double z1 = 0., z2 = 0.;
	};
	static constexpr auto kMaxChannels = 2;

	void refresh(int frequency);

	uint32 _version = 0;
	int _frequency = 0;
	bool _active = false;
	double _preamp = 1.;
	std::array<Biquad, kBands> _filters;
	std::array<bool, kBands> _used = {};
	std::array<std::array<History, kBands>, kMaxChannels> _history = {};

};

// "Эквалайзер" box, opened from the player.
void Box(not_null<Ui::GenericBox*> box);

} // namespace BearGram::Equalizer
