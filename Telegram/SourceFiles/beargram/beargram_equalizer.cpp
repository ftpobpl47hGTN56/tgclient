/*
This file is part of BearGram, a personal build of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "beargram/beargram_equalizer.h"

#include "settings.h"
#include "settings/settings_common.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/continuous_sliders.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/vertical_list.h"
#include "ui/wrap/padding_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_layers.h"
#include "styles/style_media_player.h" // mediaPlayerTime
#include "styles/style_settings.h"
#include "styles/style_background_preview_box.h" // localStorageLimitMargin
#include "styles/style_edit_peer_members.h" // localStorageLimitSlider
#include "styles/style_settings_local_storage.h"
#include "styles/style_widgets.h"

#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QMutex>

#include <atomic>
#include <cmath>
#include <cstdint>

namespace BearGram::Equalizer {
namespace {

// Bandwidth of one octave: the neighbour bands meet at about -3 dB.
constexpr auto kQuality = 1.41;
constexpr auto kPi = 3.14159265358979323846;

// A boost would clip loud music: the whole signal is lowered by half of
// the biggest boost, the rest is clamped.
constexpr auto kHeadroomPart = 0.5;

struct Preset {
	QString name;
	Gains gains;
};

[[nodiscard]] const std::vector<Preset> &Presets() {
	static const auto result = std::vector<Preset>{
		{ u"Ровно"_q, { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 } },
		{ u"Бас"_q, { 6, 5, 4, 2, 0, 0, 0, 0, 0, 0 } },
		{ u"Бас и верха"_q, { 5, 4, 2, 0, -1, 0, 1, 3, 4, 5 } },
		{ u"Электроника"_q, { 5, 4, 1, 0, -2, 0, 1, 3, 4, 5 } },
		{ u"Рок"_q, { 4, 3, 1, -1, -2, -1, 1, 3, 4, 4 } },
		{ u"Поп"_q, { -1, 1, 3, 4, 3, 1, -1, -1, 0, 0 } },
		{ u"Голос"_q, { -3, -2, 0, 2, 4, 4, 3, 1, 0, -1 } },
		{ u"Меньше баса"_q, { -6, -5, -4, -2, 0, 0, 0, 0, 0, 0 } },
		{ u"Верха"_q, { 0, 0, 0, 0, 0, 1, 2, 4, 5, 6 } },
	};
	return result;
}

struct State {
	QMutex mutex;
	bool enabled = false;
	Gains gains = {};
	std::atomic<uint32> version = 1;
};

[[nodiscard]] QString FilePath() {
	return cWorkingDir() + u"tdata/beargram_equalizer.json"_q;
}

void Load(State &state) {
	auto file = QFile(FilePath());
	if (!file.open(QIODevice::ReadOnly)) {
		return;
	}
	const auto object = QJsonDocument::fromJson(file.readAll()).object();
	state.enabled = object.value(u"enabled"_q).toBool(false);
	const auto gains = object.value(u"gains"_q).toArray();
	for (auto i = 0; i != kBands && i < gains.size(); ++i) {
		state.gains[i] = std::clamp(gains[i].toInt(0), kMinGain, kMaxGain);
	}
}

// Called with the mutex locked.
void Save(const State &state) {
	auto gains = QJsonArray();
	for (const auto gain : state.gains) {
		gains.push_back(gain);
	}
	auto object = QJsonObject();
	object.insert(u"enabled"_q, state.enabled);
	object.insert(u"gains"_q, gains);
	auto file = QFile(FilePath());
	if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
	}
}

// Never destroyed: the audio thread may use it while the app exits.
[[nodiscard]] State &Instance() {
	static const auto instance = [] {
		const auto result = new State();
		Load(*result);
		return result;
	}();
	return *instance;
}

// Main thread only.
[[nodiscard]] rpl::variable<bool> &EnabledVariable() {
	static auto result = rpl::variable<bool>(Enabled());
	return result;
}

[[nodiscard]] QString FrequencyText(int frequency) {
	return (frequency >= 1000)
		? (QString::number(frequency / 1000) + u" кГц"_q)
		: (QString::number(frequency) + u" Гц"_q);
}

[[nodiscard]] QString GainText(int gain) {
	return (gain > 0)
		? (u"+"_q + QString::number(gain) + u" дБ"_q)
		: (QString::number(gain) + u" дБ"_q);
}

// What each band changes, under its frequency.
[[nodiscard]] const std::array<QString, kBands> &BandHints() {
	static const auto result = std::array<QString, kBands>{
		u"суббас"_q,
		u"бас"_q,
		u"удар"_q,
		u"тепло"_q,
		u"середина"_q,
		u"голос"_q,
		u"атака"_q,
		u"чёткость"_q,
		u"яркость"_q,
		u"воздух"_q,
	};
	return result;
}

[[nodiscard]] QString PresetName(const Gains &gains) {
	for (const auto &preset : Presets()) {
		if (preset.gains == gains) {
			return preset.name;
		}
	}
	return u"Свой"_q;
}

[[nodiscard]] float64 SliderValue(int gain) {
	return (gain - kMinGain) / float64(kMaxGain - kMinGain);
}

} // namespace

const std::array<int, kBands> &Frequencies() {
	static const auto result = std::array<int, kBands>{
		31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000,
	};
	return result;
}

bool Enabled() {
	auto &state = Instance();
	QMutexLocker lock(&state.mutex);
	return state.enabled;
}

rpl::producer<bool> EnabledValue() {
	return EnabledVariable().value();
}

void SetEnabled(bool enabled) {
	auto &state = Instance();
	{
		QMutexLocker lock(&state.mutex);
		if (state.enabled == enabled) {
			return;
		}
		state.enabled = enabled;
		++state.version;
		Save(state);
	}
	EnabledVariable() = enabled;
}

Gains CurrentGains() {
	auto &state = Instance();
	QMutexLocker lock(&state.mutex);
	return state.gains;
}

void SetGains(const Gains &gains) {
	auto &state = Instance();
	QMutexLocker lock(&state.mutex);
	auto clamped = gains;
	for (auto &gain : clamped) {
		gain = std::clamp(gain, kMinGain, kMaxGain);
	}
	if (state.gains == clamped) {
		return;
	}
	state.gains = clamped;
	++state.version;
	Save(state);
}

void Processor::reset() {
	_history = {};
}

void Processor::refresh(int frequency) {
	auto &state = Instance();
	auto enabled = false;
	auto gains = Gains();
	{
		QMutexLocker lock(&state.mutex);
		_version = state.version.load();
		enabled = state.enabled;
		gains = state.gains;
	}
	_frequency = frequency;
	_active = false;
	auto maxBoost = 0;
	const auto &frequencies = Frequencies();
	for (auto i = 0; i != kBands; ++i) {
		const auto center = double(frequencies[i]);
		_used[i] = enabled
			&& (gains[i] != 0)
			&& (center < frequency * 0.45);
		if (!_used[i]) {
			continue;
		}
		_active = true;
		maxBoost = std::max(maxBoost, gains[i]);

		// Peaking EQ from the Audio EQ Cookbook by Robert Bristow-Johnson.
		const auto a = std::pow(10., gains[i] / 40.);
		const auto w0 = 2. * kPi * center / frequency;
		const auto alpha = std::sin(w0) / (2. * kQuality);
		const auto cosw0 = std::cos(w0);
		const auto a0 = 1. + alpha / a;
		auto &filter = _filters[i];
		filter.b0 = (1. + alpha * a) / a0;
		filter.b1 = (-2. * cosw0) / a0;
		filter.b2 = (1. - alpha * a) / a0;
		filter.a1 = (-2. * cosw0) / a0;
		filter.a2 = (1. - alpha / a) / a0;
	}
	_preamp = std::pow(10., -(maxBoost * kHeadroomPart) / 20.);
	if (!_active) {
		reset();
	}
}

void Processor::process(QByteArray &pcm, int channels, int frequency) {
	if (channels < 1 || channels > kMaxChannels || frequency <= 0) {
		return;
	}
	if (_version != Instance().version.load() || _frequency != frequency) {
		refresh(frequency);
	}
	if (!_active || pcm.isEmpty()) {
		return;
	}
	const auto samples = reinterpret_cast<std::int16_t*>(pcm.data());
	const auto count = int(pcm.size() / sizeof(std::int16_t));
	for (auto i = 0; i != count; ++i) {
		auto &history = _history[i % channels];
		auto x = double(samples[i]);
		for (auto band = 0; band != kBands; ++band) {
			if (!_used[band]) {
				continue;
			}
			const auto &f = _filters[band];
			auto &h = history[band];
			// Transposed direct form II.
			const auto y = f.b0 * x + h.z1;
			h.z1 = f.b1 * x - f.a1 * y + h.z2;
			h.z2 = f.b2 * x - f.a2 * y;
			x = y;
		}
		x *= _preamp;
		samples[i] = std::int16_t(std::clamp(x, -32768., 32767.));
	}
}

void Box(not_null<Ui::GenericBox*> box) {
	box->setTitle(rpl::single(u"Эквалайзер"_q));
	box->setWidth(st::boxRowPadding.left()
		+ style::ConvertScale(50 + 56 * kBands)
		+ st::boxRowPadding.right());

	const auto container = box->verticalLayout();

	const auto enabled = container->add(object_ptr<Ui::SettingsButton>(
		container,
		rpl::single(u"Включить эквалайзер"_q),
		st::settingsButtonNoIcon
	))->toggleOn(rpl::single(Enabled()));
	enabled->toggledChanges() | rpl::on_next([=](bool value) {
		SetEnabled(value);
	}, enabled->lifetime());

	struct BoxState {
		rpl::variable<QString> presetName;
		std::vector<Ui::MediaSlider*> sliders;
		std::vector<Ui::LabelSimple*> values;
		Gains gains = {};
		base::unique_qptr<Ui::PopupMenu> menu;
	};
	const auto state = box->lifetime().make_state<BoxState>();
	state->gains = CurrentGains();
	state->presetName = PresetName(state->gains);

	const auto applyGains = [=](const Gains &gains) {
		state->gains = gains;
		SetGains(gains);
		state->presetName = PresetName(gains);
		for (auto i = 0; i != kBands; ++i) {
			state->sliders[i]->setValue(SliderValue(gains[i]));
			state->values[i]->setText(GainText(gains[i]));
		}
	};

	const auto preset = ::Settings::AddButtonWithLabel(
		container,
		rpl::single(u"Пресет"_q),
		state->presetName.value(),
		st::settingsButtonNoIcon);
	preset->setClickedCallback([=] {
		state->menu = base::make_unique_q<Ui::PopupMenu>(
			preset,
			st::defaultPopupMenu);
		for (const auto &item : Presets()) {
			const auto gains = item.gains;
			state->menu->addAction(item.name, [=] {
				applyGains(gains);
			});
		}
		state->menu->popup(QCursor::pos());
	});

	Ui::AddSkip(container);

	// Classic graphic equalizer: a vertical slider per band, side by side.
	const auto columnWidth = style::ConvertScale(56);
	const auto scaleWidth = style::ConvertScale(50);
	const auto sliderHeight = style::ConvertScale(150);
	const auto skip = style::ConvertScale(6);
	const auto valueHeight = st::localStorageLimitValue.font->height;
	const auto nameHeight = st::localStorageLimitLabel.font->height;
	const auto hintHeight = st::mediaPlayerTime.font->height;
	const auto sliderTop = valueHeight + skip;
	const auto nameTop = sliderTop + sliderHeight + skip;
	const auto hintTop = nameTop + nameHeight;
	const auto panel = container->add(
		object_ptr<Ui::FixedHeightWidget>(
			container,
			hintTop + hintHeight + skip),
		QMargins(
			st::boxRowPadding.left(),
			0,
			st::boxRowPadding.right(),
			0));

	// The dB scale on the left: +12, 0 and -12 at the slider marks.
	const auto seek = st::localStorageLimitSlider.seekSize.height();
	const auto scaleAt = [=](float64 value) {
		return sliderTop
			+ seek / 2
			+ int(base::SafeRound((1. - value) * (sliderHeight - seek)));
	};
	for (const auto gain : { kMaxGain, 0, kMinGain }) {
		const auto label = Ui::CreateChild<Ui::LabelSimple>(
			panel,
			st::mediaPlayerTime,
			GainText(gain));
		label->moveToLeft(
			0,
			scaleAt(SliderValue(gain)) - hintHeight / 2);
	}

	const auto &frequencies = Frequencies();
	const auto &hints = BandHints();
	for (auto i = 0; i != kBands; ++i) {
		const auto value = Ui::CreateChild<Ui::LabelSimple>(
			panel,
			st::localStorageLimitValue,
			GainText(state->gains[i]));
		const auto name = Ui::CreateChild<Ui::LabelSimple>(
			panel,
			st::localStorageLimitLabel,
			FrequencyText(frequencies[i]));
		const auto hint = Ui::CreateChild<Ui::LabelSimple>(
			panel,
			st::mediaPlayerTime,
			hints[i]);
		const auto slider = Ui::CreateChild<Ui::MediaSlider>(
			panel,
			st::localStorageLimitSlider);
		slider->setDirection(Ui::ContinuousSlider::Direction::Vertical);
		slider->setAlwaysDisplayMarker(true);
		slider->setValue(SliderValue(state->gains[i]));
		const auto steps = kMaxGain - kMinGain;
		const auto gainFor = [=](float64 progress) {
			return kMinGain + int(base::SafeRound(progress * steps));
		};
		slider->setAdjustCallback([=](float64 progress) {
			return base::SafeRound(progress * steps) / float64(steps);
		});
		slider->setChangeProgressCallback([=](float64 progress) {
			value->setText(GainText(gainFor(progress)));
		});
		slider->setChangeFinishedCallback([=](float64 progress) {
			auto gains = state->gains;
			gains[i] = gainFor(progress);
			state->gains = gains;
			SetGains(gains);
			state->presetName = PresetName(gains);
		});
		state->sliders.push_back(slider);
		state->values.push_back(value);

		// Centered in its column; labels change width with their text.
		const auto center = [=](not_null<Ui::RpWidget*> widget, int top) {
			const auto left = scaleWidth + i * columnWidth;
			widget->moveToLeft(
				left + (columnWidth - widget->width()) / 2,
				top);
		};
		value->widthValue() | rpl::on_next([=] {
			center(value, 0);
		}, value->lifetime());
		center(name, nameTop);
		center(hint, hintTop);
		slider->resize(seek, sliderHeight);
		center(slider, sliderTop);
	}

	Ui::AddSkip(container);
	Ui::AddDividerText(
		container,
		rpl::single(u"Только для музыки в плеере сверху: голосовые, видео "
			"и звонки не меняются. Изменения слышны через пару секунд — "
			"столько музыки плеер загружает заранее. Громкие усиления "
			"немного понижают общую громкость, чтобы звук не хрипел."_q));

	box->addButton(rpl::single(u"Готово"_q), [=] { box->closeBox(); });
	box->addLeftButton(rpl::single(u"Сбросить"_q), [=] {
		applyGains(Gains{});
	});
}

} // namespace BearGram::Equalizer
