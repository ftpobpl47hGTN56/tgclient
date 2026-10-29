/*
This file is part of BearGram, a personal build of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "beargram/beargram_account_transfer.h"

#include "base/random.h"
#include "base/unixtime.h"
#include "core/application.h"
#include "core/file_utilities.h"
#include "data/data_user.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "mtproto/mtp_instance.h"
#include "mtproto/mtproto_dc_options.h"
#include "settings/settings_common.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/password_input.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

#include <openssl/evp.h>

namespace BearGram::AccountTransfer {
namespace {

// Different from the Android "BGACC1": an Android session (tgnet.dat) is a
// different format, so each app recognizes and refuses the other's file.
constexpr auto kMagic = std::string_view("BGDSK1");
constexpr auto kAndroidMagic = std::string_view("BGACC1");
constexpr auto kSaltSize = 16;
constexpr auto kIvSize = 12;
constexpr auto kTagSize = 16;
constexpr auto kKeySize = 32;
constexpr auto kIterations = 120000;
constexpr auto kMaxFileSize = 16 * 1024 * 1024;
constexpr auto kMinPasswordLength = 4;

struct ImportResult {
	QStringList added;
	QStringList skipped; // Already logged in here.
	QStringList noRoom; // The account limit is reached.
	QString error;
	bool wrongPassword = false;
};

[[nodiscard]] QByteArray MagicBytes(std::string_view magic) {
	return QByteArray(magic.data(), int(magic.size()));
}

[[nodiscard]] QByteArray DeriveKey(
		const QString &password,
		const QByteArray &salt) {
	const auto utf8 = password.toUtf8();
	auto result = QByteArray(kKeySize, Qt::Uninitialized);
	const auto ok = PKCS5_PBKDF2_HMAC(
		utf8.constData(),
		int(utf8.size()),
		reinterpret_cast<const unsigned char*>(salt.constData()),
		int(salt.size()),
		kIterations,
		EVP_sha256(),
		kKeySize,
		reinterpret_cast<unsigned char*>(result.data()));
	return (ok == 1) ? result : QByteArray();
}

// AES-256-GCM, the magic is the additional data: ciphertext + tag.
[[nodiscard]] QByteArray Seal(
		const QByteArray &key,
		const QByteArray &iv,
		const QByteArray &plain) {
	const auto aad = MagicBytes(kMagic);
	auto result = QByteArray(plain.size() + kTagSize, Qt::Uninitialized);
	const auto out = reinterpret_cast<unsigned char*>(result.data());
	const auto context = EVP_CIPHER_CTX_new();
	if (!context) {
		return {};
	}
	auto length = 0;
	auto total = 0;
	auto ok = (EVP_EncryptInit_ex(
			context,
			EVP_aes_256_gcm(),
			nullptr,
			nullptr,
			nullptr) == 1)
		&& (EVP_CIPHER_CTX_ctrl(
			context,
			EVP_CTRL_GCM_SET_IVLEN,
			int(iv.size()),
			nullptr) == 1)
		&& (EVP_EncryptInit_ex(
			context,
			nullptr,
			nullptr,
			reinterpret_cast<const unsigned char*>(key.constData()),
			reinterpret_cast<const unsigned char*>(iv.constData())) == 1)
		&& (EVP_EncryptUpdate(
			context,
			nullptr,
			&length,
			reinterpret_cast<const unsigned char*>(aad.constData()),
			int(aad.size())) == 1)
		&& (EVP_EncryptUpdate(
			context,
			out,
			&length,
			reinterpret_cast<const unsigned char*>(plain.constData()),
			int(plain.size())) == 1);
	total = length;
	ok = ok
		&& (EVP_EncryptFinal_ex(context, out + total, &length) == 1)
		&& (EVP_CIPHER_CTX_ctrl(
			context,
			EVP_CTRL_GCM_GET_TAG,
			kTagSize,
			out + plain.size()) == 1);
	EVP_CIPHER_CTX_free(context);
	return ok ? result : QByteArray();
}

[[nodiscard]] std::optional<QByteArray> Open(
		const QByteArray &key,
		const QByteArray &iv,
		const QByteArray &sealed) {
	if (sealed.size() < kTagSize) {
		return std::nullopt;
	}
	const auto aad = MagicBytes(kMagic);
	const auto plainSize = sealed.size() - kTagSize;
	auto result = QByteArray(plainSize, Qt::Uninitialized);
	auto tag = sealed.mid(plainSize);
	const auto out = reinterpret_cast<unsigned char*>(result.data());
	const auto context = EVP_CIPHER_CTX_new();
	if (!context) {
		return std::nullopt;
	}
	auto length = 0;
	auto total = 0;
	auto ok = (EVP_DecryptInit_ex(
			context,
			EVP_aes_256_gcm(),
			nullptr,
			nullptr,
			nullptr) == 1)
		&& (EVP_CIPHER_CTX_ctrl(
			context,
			EVP_CTRL_GCM_SET_IVLEN,
			int(iv.size()),
			nullptr) == 1)
		&& (EVP_DecryptInit_ex(
			context,
			nullptr,
			nullptr,
			reinterpret_cast<const unsigned char*>(key.constData()),
			reinterpret_cast<const unsigned char*>(iv.constData())) == 1)
		&& (EVP_DecryptUpdate(
			context,
			nullptr,
			&length,
			reinterpret_cast<const unsigned char*>(aad.constData()),
			int(aad.size())) == 1)
		&& (EVP_DecryptUpdate(
			context,
			out,
			&length,
			reinterpret_cast<const unsigned char*>(sealed.constData()),
			int(plainSize)) == 1);
	total = length;
	ok = ok
		&& (EVP_CIPHER_CTX_ctrl(
			context,
			EVP_CTRL_GCM_SET_TAG,
			kTagSize,
			tag.data()) == 1)
		&& (EVP_DecryptFinal_ex(context, out + total, &length) == 1);
	EVP_CIPHER_CTX_free(context);
	if (!ok) {
		return std::nullopt;
	}
	return result;
}

[[nodiscard]] QString AccountName(not_null<Main::Session*> session) {
	const auto user = session->user();
	const auto name = user->name();
	const auto username = user->username();
	return username.isEmpty()
		? name
		: (name + u" (@"_q + username + ')');
}

[[nodiscard]] std::vector<not_null<Main::Account*>> LoggedInAccounts() {
	auto result = std::vector<not_null<Main::Account*>>();
	for (const auto &account : Core::App().domain().orderedAccounts()) {
		if (account->sessionExists()) {
			result.push_back(account);
		}
	}
	return result;
}

[[nodiscard]] QByteArray Export(const QString &password, int *count) {
	auto accounts = QJsonArray();
	for (const auto &account : LoggedInAccounts()) {
		const auto session = &account->session();
		auto entry = QJsonObject();
		entry.insert(
			u"userId"_q,
			QString::number(session->userId().bare));
		entry.insert(u"name"_q, AccountName(session));
		entry.insert(
			u"test"_q,
			account->mtp().environment() != MTP::Environment::Production);
		entry.insert(
			u"mtp"_q,
			QString::fromLatin1(
				account->serializeMtpAuthorization().toBase64()));
		accounts.push_back(entry);
	}
	*count = accounts.size();
	if (accounts.isEmpty()) {
		return {};
	}
	auto root = QJsonObject();
	root.insert(u"format"_q, 1);
	root.insert(u"from"_q, u"BearGram Desktop"_q);
	root.insert(u"date"_q, qint64(base::unixtime::now()));
	root.insert(u"accounts"_q, accounts);
	const auto plain = QJsonDocument(root).toJson(QJsonDocument::Compact);

	auto salt = QByteArray(kSaltSize, Qt::Uninitialized);
	auto iv = QByteArray(kIvSize, Qt::Uninitialized);
	base::RandomFill(salt.data(), salt.size());
	base::RandomFill(iv.data(), iv.size());
	const auto key = DeriveKey(password, salt);
	const auto sealed = key.isEmpty() ? QByteArray() : Seal(key, iv, plain);
	if (sealed.isEmpty()) {
		return {};
	}
	return MagicBytes(kMagic) + salt + iv + sealed;
}

[[nodiscard]] ImportResult Import(
		const QByteArray &file,
		const QString &password) {
	auto result = ImportResult();
	const auto header = int(kMagic.size()) + kSaltSize + kIvSize;
	if (file.startsWith(MagicBytes(kAndroidMagic))) {
		result.error = u"Это файл аккаунтов BearGram для Android. "
			"На ПК подходит только файл, сохранённый в BearGram на ПК."_q;
		return result;
	} else if (!file.startsWith(MagicBytes(kMagic))
		|| file.size() <= header + kTagSize) {
		result.error = u"Это не файл аккаунтов BearGram."_q;
		return result;
	}
	const auto salt = file.mid(int(kMagic.size()), kSaltSize);
	const auto iv = file.mid(int(kMagic.size()) + kSaltSize, kIvSize);
	const auto key = DeriveKey(password, salt);
	const auto plain = key.isEmpty()
		? std::nullopt
		: Open(key, iv, file.mid(header));
	if (!plain) {
		result.wrongPassword = true;
		return result;
	}
	const auto root = QJsonDocument::fromJson(*plain).object();
	if (root.value(u"format"_q).toInt() != 1) {
		result.error = u"Файл сохранён более новой версией BearGram."_q;
		return result;
	}

	auto &domain = Core::App().domain();
	auto loggedIn = base::flat_set<uint64>();
	for (const auto &account : LoggedInAccounts()) {
		loggedIn.emplace(account->session().userId().bare);
	}
	auto firstAdded = (Main::Account*)nullptr;
	for (const auto value : root.value(u"accounts"_q).toArray()) {
		const auto entry = value.toObject();
		const auto userId = entry.value(u"userId"_q).toString().toULongLong();
		const auto name = entry.value(u"name"_q).toString();
		const auto authorization = QByteArray::fromBase64(
			entry.value(u"mtp"_q).toString().toLatin1());
		if (!userId || authorization.isEmpty()) {
			continue;
		} else if (loggedIn.contains(userId)) {
			result.skipped.push_back(name);
			continue;
		}
		// Accounts without a session (the welcome screen) are dropped when
		// another one is activated, so they don't take a place here.
		const auto used = ranges::count_if(
			domain.accounts(),
			[](const Main::Domain::AccountWithIndex &value) {
				return value.account->sessionExists();
			});
		if (used >= domain.maxAccounts()
			|| int(domain.accounts().size())
				>= int(Main::Domain::kPremiumMaxAccounts)) {
			result.noRoom.push_back(name);
			continue;
		}
		const auto environment = entry.value(u"test"_q).toBool()
			? MTP::Environment::Test
			: MTP::Environment::Production;
		const auto account = domain.addWithAuthorization(
			environment,
			authorization);
		if (!account->sessionExists()) {
			result.error = u"Не удалось прочитать сессию: "_q + name;
			continue;
		}
		loggedIn.emplace(userId);
		result.added.push_back(name);
		if (!firstAdded) {
			firstAdded = account;
		}
	}
	// On the welcome screen the active account has no session: switch to
	// an imported one, the empty account is removed then.
	if (firstAdded && !domain.active().sessionExists()) {
		domain.activate(firstAdded);
	}
	return result;
}

[[nodiscard]] QString ResultText(const ImportResult &result) {
	auto lines = QStringList();
	if (!result.added.isEmpty()) {
		lines.push_back(u"Добавлены: "_q + result.added.join(u", "_q));
	}
	if (!result.skipped.isEmpty()) {
		lines.push_back(u"Уже есть здесь: "_q + result.skipped.join(u", "_q));
	}
	if (!result.noRoom.isEmpty()) {
		lines.push_back(u"Не хватило места (лимит аккаунтов в Настройки → "
			"BearGram): "_q + result.noRoom.join(u", "_q));
	}
	if (!result.error.isEmpty()) {
		lines.push_back(result.error);
	}
	if (lines.isEmpty()) {
		lines.push_back(u"В файле нет аккаунтов."_q);
	}
	if (!result.added.isEmpty()) {
		lines.push_back(u"\nЧаты загрузятся с сервера. Старую копию BearGram "
			"удалите, не выходя из аккаунтов: одна сессия не должна работать "
			"в двух приложениях."_q);
	}
	return lines.join('\n');
}

[[nodiscard]] not_null<Ui::PasswordInput*> AddPasswordField(
		not_null<Ui::GenericBox*> box,
		const QString &placeholder) {
	box->addSkip(st::boxLittleSkip);
	return box->addRow(
		object_ptr<Ui::PasswordInput>(
			box,
			st::defaultInputField,
			rpl::single(placeholder)));
}

void ExportBox(not_null<Ui::GenericBox*> box) {
	box->setTitle(rpl::single(u"Экспорт аккаунтов"_q));
	box->setWidth(st::boxWideWidth);

	const auto accounts = LoggedInAccounts();
	auto names = QStringList();
	for (const auto &account : accounts) {
		names.push_back(AccountName(&account->session()));
	}
	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			(u"В файл попадут аккаунты (%1):\n"_q.arg(names.size())
				+ names.join('\n')
				+ u"\n\nС этим файлом и паролем можно войти в аккаунты "
				"без кода. Храните его как пароль и не отправляйте "
				"никому."_q),
			st::boxLabel),
		st::boxRowPadding);
	const auto password = AddPasswordField(box, u"Пароль для файла"_q);
	const auto repeat = AddPasswordField(box, u"Пароль ещё раз"_q);

	box->setFocusCallback([=] { password->setFocusFast(); });
	box->addButton(rpl::single(u"Сохранить"_q), [=] {
		const auto value = password->getLastText();
		if (value.size() < kMinPasswordLength) {
			password->showError();
			box->showToast(u"Пароль хотя бы из %1 символов."_q.arg(
				kMinPasswordLength));
			return;
		} else if (repeat->getLastText() != value) {
			repeat->showError();
			box->showToast(u"Пароли не совпадают."_q);
			return;
		}
		const auto weak = base::make_weak(box);
		FileDialog::GetWritePath(
			box.get(),
			u"Экспорт аккаунтов BearGram"_q,
			u"Аккаунты BearGram (*.bgaccounts);;Все файлы (*.*)"_q,
			QDir::homePath() + u"/beargram_accounts.bgaccounts"_q,
			[=](QString &&path) {
				auto count = 0;
				const auto data = Export(value, &count);
				auto file = QFile(path);
				const auto ok = !data.isEmpty()
					&& file.open(QIODevice::WriteOnly | QIODevice::Truncate)
					&& (file.write(data) == data.size());
				file.close();
				if (const auto strong = weak.get()) {
					if (ok) {
						strong->showToast(u"Сохранено аккаунтов: %1 → %2"_q
							.arg(count)
							.arg(QDir::toNativeSeparators(path)));
						strong->closeBox();
					} else {
						strong->showToast(count
							? u"Не удалось записать файл."_q
							: u"Нет аккаунтов для экспорта."_q);
					}
				}
			});
	});
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

void ImportPasswordBox(
		not_null<Ui::GenericBox*> box,
		QByteArray content) {
	box->setTitle(rpl::single(u"Импорт аккаунтов"_q));
	box->setWidth(st::boxWideWidth);
	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			u"Введите пароль, который задали при экспорте. Аккаунты, "
				"которые уже есть здесь, будут пропущены."_q,
			st::boxLabel),
		st::boxRowPadding);
	const auto password = AddPasswordField(box, u"Пароль файла"_q);
	box->setFocusCallback([=] { password->setFocusFast(); });
	box->addButton(rpl::single(u"Импортировать"_q), [=] {
		const auto result = Import(content, password->getLastText());
		if (result.wrongPassword) {
			password->showError();
			box->showToast(u"Неверный пароль."_q);
			return;
		}
		const auto show = box->uiShow();
		box->closeBox();
		show->show(Ui::MakeInformBox({
			.text = ResultText(result),
			.title = rpl::single(u"Импорт аккаунтов"_q),
		}));
	});
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

void ChooseFileAndImport(std::shared_ptr<Ui::Show> show) {
	FileDialog::GetOpenPath(
		show->toastParent().get(),
		u"Импорт аккаунтов BearGram"_q,
		u"Аккаунты BearGram (*.bgaccounts);;Все файлы (*.*)"_q,
		[=](FileDialog::OpenResult &&result) {
			auto content = result.remoteContent;
			if (content.isEmpty() && !result.paths.isEmpty()) {
				auto file = QFile(result.paths.front());
				if (file.size() > kMaxFileSize) {
					show->showToast(u"Файл слишком большой."_q);
					return;
				} else if (file.open(QIODevice::ReadOnly)) {
					content = file.readAll();
				}
			}
			if (content.isEmpty()) {
				show->showToast(u"Файл пустой или не читается."_q);
				return;
			}
			show->show(Box(ImportPasswordBox, std::move(content)));
		});
}

} // namespace

void StartImport(std::shared_ptr<Ui::Show> show) {
	ChooseFileAndImport(std::move(show));
}

void AddSettingsSection(
		not_null<Ui::VerticalLayout*> container,
		std::shared_ptr<Ui::Show> show) {
	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(container, rpl::single(u"Перенос аккаунтов"_q));
	const auto exportButton = container->add(
		object_ptr<Ui::SettingsButton>(
			container,
			rpl::single(u"Экспорт аккаунтов в файл"_q),
			st::settingsButtonNoIcon));
	exportButton->setClickedCallback([=] {
		if (LoggedInAccounts().empty()) {
			show->showToast(u"Нет аккаунтов для экспорта."_q);
			return;
		}
		show->show(Box(ExportBox));
	});
	const auto importButton = container->add(
		object_ptr<Ui::SettingsButton>(
			container,
			rpl::single(u"Импорт аккаунтов из файла"_q),
			st::settingsButtonNoIcon));
	importButton->setClickedCallback([=] {
		ChooseFileAndImport(show);
	});
	const auto reloadButton = container->add(
		object_ptr<Ui::SettingsButton>(
			container,
			rpl::single(u"Загрузить чаты заново"_q),
			st::settingsButtonNoIcon));
	reloadButton->setClickedCallback([=] {
		show->show(Ui::MakeConfirmBox({
			.text = u"BearGram перезапустится, и списки чатов всех "
				"аккаунтов загрузятся с сервера заново. Входить в аккаунты "
				"снова не нужно."_q,
			.confirmed = [] { Core::Restart(); },
			.confirmText = u"Перезапустить"_q,
			.title = rpl::single(u"Загрузить чаты заново"_q),
		}));
	});
	Ui::AddSkip(container);
	Ui::AddDividerText(
		container,
		rpl::single(u"Перенос в другой BearGram на ПК без входа по коду: "
			"файл с паролем содержит ключи авторизации аккаунтов. После "
			"переноса удалите старую копию, не выходя из аккаунтов. Файл "
			"с Android на ПК не подходит. На ПК чаты не хранятся на "
			"диске: «Загрузить чаты заново» перезапускает BearGram, и "
			"списки чатов приходят с сервера."_q));
}

} // namespace BearGram::AccountTransfer
