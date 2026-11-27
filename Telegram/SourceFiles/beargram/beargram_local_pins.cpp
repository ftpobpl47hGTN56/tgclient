/*
This file is part of BearGram, a personal build of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "beargram/beargram_local_pins.h"

#include "apiwrap.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_folder.h"
#include "data/data_histories.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "dialogs/dialogs_key.h"
#include "dialogs/dialogs_main_list.h"
#include "dialogs/dialogs_pinned_list.h"
#include "history/history.h"
#include "main/main_session.h"
#include "settings.h"

#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>

namespace BearGram::LocalPins {
namespace {

struct Account {
	// Local pin -> access hash (0 for a basic group).
	base::flat_map<PeerId, uint64> local;
	// Remembered order: "p<peer id>" or "f<folder id>", per folder id.
	base::flat_map<int, std::vector<QString>> order;
};

struct State {
	bool loaded = false;
	base::flat_map<uint64, Account> accounts;
	base::flat_set<Main::Session*> scheduled;
	base::flat_set<PeerId> requested;
	base::flat_map<PeerId, int> attempts;
	base::flat_set<Main::Session*> merged;
};

// Stop asking the server about a peer after this many failures.
constexpr auto kMaxPeerAttempts = 4;

[[nodiscard]] State &Instance() {
	static auto result = State();
	return result;
}

// The pins are kept in the BearGram settings file, under "pins".
[[nodiscard]] QString SettingsPath() {
	return cWorkingDir() + u"tdata/beargram.json"_q;
}

// Before: a separate file, read once if the settings have no pins yet.
[[nodiscard]] QString OldFilePath() {
	return cWorkingDir() + u"tdata/beargram_pins.json"_q;
}

[[nodiscard]] QJsonObject ReadObject(const QString &path) {
	auto file = QFile(path);
	return file.open(QIODevice::ReadOnly)
		? QJsonDocument::fromJson(file.readAll()).object()
		: QJsonObject();
}

void Parse(const QJsonObject &root, State &state) {
	for (auto i = root.begin(); i != root.end(); ++i) {
		const auto userId = i.key().toULongLong();
		if (!userId) {
			continue;
		}
		const auto object = i.value().toObject();
		auto &account = state.accounts[userId];
		for (const auto &value : object.value(u"local"_q).toArray()) {
			const auto entry = value.toObject();
			const auto id = entry.value(u"id"_q).toString().toULongLong();
			if (id) {
				account.local[PeerId(id)]
					= entry.value(u"hash"_q).toString().toULongLong();
			}
		}
		const auto orders = object.value(u"order"_q).toObject();
		for (auto j = orders.begin(); j != orders.end(); ++j) {
			auto &list = account.order[j.key().toInt()];
			for (const auto &value : j.value().toArray()) {
				list.push_back(value.toString());
			}
		}
	}
}

void Load(State &state) {
	state.loaded = true;
	auto root = ReadObject(SettingsPath()).value(u"pins"_q).toObject();
	if (root.isEmpty()) {
		root = ReadObject(OldFilePath());
	}
	Parse(root, state);
}

[[nodiscard]] QJsonObject PinsObject(const State &state) {
	auto root = QJsonObject();
	for (const auto &[userId, account] : state.accounts) {
		if (account.local.empty() && account.order.empty()) {
			continue;
		}
		auto local = QJsonArray();
		for (const auto &[id, hash] : account.local) {
			local.push_back(QJsonObject{
				{ u"id"_q, QString::number(id.value) },
				{ u"hash"_q, QString::number(hash) },
			});
		}
		auto orders = QJsonObject();
		for (const auto &[folderId, list] : account.order) {
			auto array = QJsonArray();
			for (const auto &name : list) {
				array.push_back(name);
			}
			orders.insert(QString::number(folderId), array);
		}
		root.insert(QString::number(userId), QJsonObject{
			{ u"local"_q, local },
			{ u"order"_q, orders },
		});
	}
	return root;
}

void Save(const State &state) {
	// The rest of the settings file stays as it is.
	auto settings = ReadObject(SettingsPath());
	settings.insert(u"pins"_q, PinsObject(state));
	auto file = QSaveFile(SettingsPath());
	if (file.open(QIODevice::WriteOnly)) {
		file.write(QJsonDocument(settings).toJson(QJsonDocument::Indented));
		if (file.commit()) {
			QFile::remove(OldFilePath());
		}
	}
}

[[nodiscard]] Account &AccountFor(not_null<Main::Session*> session) {
	auto &state = Instance();
	if (!state.loaded) {
		Load(state);
	}
	return state.accounts[session->userId().bare];
}

[[nodiscard]] Account *FindAccount(not_null<Main::Session*> session) {
	auto &state = Instance();
	if (!state.loaded) {
		Load(state);
	}
	const auto i = state.accounts.find(session->userId().bare);
	return (i != end(state.accounts)) ? &i->second : nullptr;
}

[[nodiscard]] int FolderIndex(Data::Folder *folder) {
	return folder ? folder->id() : 0;
}

[[nodiscard]] QString KeyName(const Dialogs::Key &key) {
	if (const auto history = key.history()) {
		return u"p"_q + QString::number(history->peer->id.value);
	} else if (const auto folder = key.folder()) {
		return u"f"_q + QString::number(folder->id());
	}
	return QString();
}

[[nodiscard]] uint64 AccessHash(not_null<PeerData*> peer) {
	if (const auto user = peer->asUser()) {
		return user->accessHash();
	} else if (const auto channel = peer->asChannel()) {
		return channel->accessHash();
	}
	return 0;
}

[[nodiscard]] bool CanBePinned(not_null<History*> history) {
	if (const auto channel = history->peer->asChannel()) {
		return channel->amIn();
	} else if (const auto chat = history->peer->asChat()) {
		return chat->amIn();
	}
	return true;
}

// Loads the dialogs of the local pins this session doesn't know yet.
// One peer per request: a single bad peer (left channel, stale access
// hash) must not fail the whole batch of pins.
void RequestPeers(
		not_null<Main::Session*> session,
		const std::vector<std::pair<PeerId, uint64>> &peers) {
	auto &state = Instance();
	auto &requested = state.requested;
	auto ids = std::vector<PeerId>();
	auto list = QVector<MTPInputDialogPeer>();
	for (const auto &[id, hash] : peers) {
		if (requested.contains(id)) {
			continue;
		}
		const auto input = [&]() -> std::optional<MTPInputPeer> {
			if (peerIsUser(id) && hash) {
				return MTP_inputPeerUser(
					MTP_long(peerToUser(id).bare),
					MTP_long(hash));
			} else if (peerIsChannel(id) && hash) {
				return MTP_inputPeerChannel(
					MTP_long(peerToChannel(id).bare),
					MTP_long(hash));
			} else if (peerIsChat(id)) {
				return MTP_inputPeerChat(MTP_long(peerToChat(id).bare));
			}
			return std::nullopt;
		}();
		if (input) {
			requested.emplace(id);
			ids.push_back(id);
			list.push_back(MTP_inputDialogPeer(*input));
		}
	}
	if (list.isEmpty()) {
		return;
	}
	const auto weak = base::make_weak(session);
	for (auto i = 0; i != list.size(); ++i) {
		const auto peerId = ids[i];
		const auto single = QVector<MTPInputDialogPeer>{ list[i] };
		session->api().request(MTPmessages_GetPeerDialogs(
			MTP_vector(single)
		)).done([=](const MTPmessages_PeerDialogs &result) {
			const auto strong = weak.get();
			if (!strong) {
				return;
			}
			auto &histories = strong->data().histories();
			histories.applyPeerDialogs(result);
			result.match([&](const MTPDmessages_peerDialogs &data) {
				for (const auto &dialog : data.vdialogs().v) {
					dialog.match([&](const MTPDdialog &data) {
						if (const auto id = peerFromMTP(data.vpeer())) {
							const auto history = strong->data().history(id);
							histories.dialogEntryApplied(history);
							history->updateChatListExistence();
						}
					}, [](const auto &) {});
				}
			});
			Instance().attempts.remove(peerId);
			ScheduleMerge(strong);
		}).fail([=](const MTP::Error &) {
			auto &state = Instance();
			const auto j = state.attempts.find(peerId);
			const auto next = (j == end(state.attempts)) ? 1 : j->second + 1;
			if (next >= kMaxPeerAttempts) {
				return;
			}
			state.attempts[peerId] = next;
			state.requested.remove(peerId);
		}).send();
	}
}

void Merge(not_null<Main::Session*> session, Data::Folder *folder) {
	const auto account = FindAccount(session);
	if (!account) {
		return;
	}
	const auto owner = &session->data();
	const auto list = owner->chatsList(folder)->pinned();
	const auto current = list->order();

	auto keys = current;
	auto unknown = std::vector<std::pair<PeerId, uint64>>();
	for (const auto &[id, hash] : account->local) {
		const auto peer = owner->peerLoaded(id);
		if (!peer || !peer->isMinimalLoaded()) {
			unknown.emplace_back(id, hash);
			continue;
		}
		const auto history = owner->history(peer->id);
		if (!history->folderKnown()) {
			unknown.emplace_back(id, AccessHash(peer));
			continue;
		} else if (history->folder() != folder
			|| !CanBePinned(history)
			|| ranges::contains(keys, Dialogs::Key(history))) {
			continue;
		}
		keys.push_back(history);
	}
	if (!unknown.empty()) {
		RequestPeers(session, unknown);
	}

	const auto i = account->order.find(FolderIndex(folder));
	auto desired = std::vector<Dialogs::Key>();
	desired.reserve(keys.size());
	if (i == end(account->order) || i->second.empty()) {
		desired = keys;
	} else {
		const auto &stored = i->second;
		// Pinned on another device: on top, as Telegram does.
		for (const auto &key : keys) {
			if (!ranges::contains(stored, KeyName(key))) {
				desired.push_back(key);
			}
		}
		for (const auto &name : stored) {
			const auto j = ranges::find(keys, name, KeyName);
			if (j != end(keys)) {
				desired.push_back(*j);
			}
		}
	}
	if (desired == current) {
		return;
	}
	list->clear();
	for (const auto &key : desired) {
		list->addPinned(key);
	}
	owner->notifyPinnedDialogsOrderUpdated();
}

} // namespace

bool IsLocal(not_null<History*> history) {
	const auto account = FindAccount(&history->session());
	return account && account->local.contains(history->peer->id);
}

bool IsLocalKey(const Dialogs::Key &key) {
	const auto history = key.history();
	return history && IsLocal(not_null<History*>(history));
}

void AddLocal(not_null<History*> history) {
	auto &account = AccountFor(&history->session());
	account.local[history->peer->id] = AccessHash(history->peer);
	Save(Instance());
}

void RemoveLocal(not_null<History*> history) {
	const auto account = FindAccount(&history->session());
	if (account && account->local.remove(history->peer->id)) {
		Save(Instance());
	}
}

void RememberOrder(not_null<Main::Session*> session, Data::Folder *folder) {
	auto &state = Instance();
	auto &account = AccountFor(session);
	auto names = std::vector<QString>();
	for (const auto &key : session->data().pinnedChatsOrder(folder)) {
		if (auto name = KeyName(key); !name.isEmpty()) {
			names.push_back(std::move(name));
		}
	}
	auto &stored = account.order[FolderIndex(folder)];
	// The list in the UI is still the short server one, the merge hasn't
	// restored the stored order yet: don't overwrite it with this.
	const auto restored = state.merged.contains(session.get());
	if (!restored && !stored.empty() && names.size() < stored.size()) {
		return;
	}
	if (stored != names) {
		stored = std::move(names);
		Save(Instance());
	}
}

void ScheduleMerge(not_null<Main::Session*> session) {
	auto &scheduled = Instance().scheduled;
	if (scheduled.contains(session.get())) {
		return;
	}
	scheduled.emplace(session.get());
	const auto weak = base::make_weak(session);
	const auto raw = session.get();
	crl::on_main([=] {
		Instance().scheduled.remove(raw);
		const auto strong = weak.get();
		if (!strong) {
			return;
		}
		Merge(strong, nullptr);
		if (const auto archive = strong->data().folderLoaded(
				Data::Folder::kId)) {
			Merge(strong, archive);
		}
		Instance().merged.emplace(raw);
	});
}

QJsonObject ExportPins() {
	auto &state = Instance();
	if (!state.loaded) {
		Load(state);
	}
	return PinsObject(state);
}

void ImportPins(const QJsonObject &pins) {
	auto &state = Instance();
	if (!state.loaded) {
		Load(state);
	}
	state.accounts.clear();
	state.requested.clear();
	state.attempts.clear();
	Parse(pins, state);
	Save(state);
}

} // namespace BearGram::LocalPins
