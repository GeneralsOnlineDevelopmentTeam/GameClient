#include "GameNetwork/GeneralsOnline/MatchKeys.h"
#include "GameNetwork/GeneralsOnline/NetBytes.h"

#include <openssl/evp.h>

#include <cstring>

MatchKeys::MatchKeys()
	: m_active(false)
	, m_matchID(0)
	, m_localUserID(-1)
	, m_privateKey(nullptr)
{
	m_publicKey.fill(0);
}

MatchKeys::~MatchKeys()
{
	End();
}

bool MatchKeys::BeginLobby(int64_t localUserID)
{
	if (m_privateKey != nullptr && m_localUserID == localUserID)
	{
		return true;
	}
	End();

	EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
	if (ctx == nullptr)
	{
		return false;
	}

	EVP_PKEY* key = nullptr;
	const bool generated = EVP_PKEY_keygen_init(ctx) == 1 && EVP_PKEY_keygen(ctx, &key) == 1;
	EVP_PKEY_CTX_free(ctx);
	if (!generated || key == nullptr)
	{
		return false;
	}

	size_t publicKeySize = PUBLIC_KEY_SIZE;
	if (EVP_PKEY_get_raw_public_key(key, m_publicKey.data(), &publicKeySize) != 1 || publicKeySize != PUBLIC_KEY_SIZE)
	{
		EVP_PKEY_free(key);
		return false;
	}

	m_privateKey = key;
	m_localUserID = localUserID;
	return true;
}

void MatchKeys::End()
{
	if (m_privateKey != nullptr)
	{
		EVP_PKEY_free(m_privateKey);
		m_privateKey = nullptr;
	}
	m_publicKey.fill(0);
	FreePeerKeyObjects();
	m_peerKeys.clear();
	m_peerHolds.clear();
	m_active = false;
	m_expectedPeers.clear();
	m_matchID = 0;
	m_localUserID = -1;
}

bool MatchKeys::BeginMatch(uint64_t matchID, const std::set<int64_t>& expectedPeers)
{
	if (m_privateKey == nullptr)
	{
		return false;
	}
	m_matchID = matchID;
	m_expectedPeers = expectedPeers;
	m_active = true;
	return true;
}

void MatchKeys::EndMatch()
{
	m_active = false;
	m_expectedPeers.clear();
	m_matchID = 0;
}

bool MatchKeys::IsReady() const
{
	if (!m_active)
	{
		return false;
	}
	for (int64_t userID : m_expectedPeers)
	{
		if (!HasPeerKey(userID))
		{
			return false;
		}
	}
	return true;
}

std::vector<int64_t> MatchKeys::GetPeerKeyUsers() const
{
	std::vector<int64_t> users;
	for (const auto& entry : m_peerKeys)
	{
		users.push_back(entry.first);
	}
	return users;
}

bool MatchKeys::PeerHoldsAll(int64_t peer, const std::set<int64_t>& needed) const
{
	auto holds = m_peerHolds.find(peer);
	if (holds == m_peerHolds.end())
	{
		return false;
	}
	for (int64_t userID : needed)
	{
		if (userID != peer && holds->second.count(userID) == 0)
		{
			return false;
		}
	}
	return true;
}

bool MatchKeys::DropPeer(int64_t userID)
{
	if (m_active && ExpectsPeer(userID))
	{
		return false;
	}
	FreePeerKeyObject(userID);
	m_peerHolds.erase(userID);
	return m_peerKeys.erase(userID) != 0;
}

void MatchKeys::FreePeerKeyObject(int64_t userID)
{
	auto parsed = m_peerKeyObjects.find(userID);
	if (parsed != m_peerKeyObjects.end())
	{
		EVP_PKEY_free(parsed->second);
		m_peerKeyObjects.erase(parsed);
	}
}

void MatchKeys::FreePeerKeyObjects()
{
	for (auto& entry : m_peerKeyObjects)
	{
		EVP_PKEY_free(entry.second);
	}
	m_peerKeyObjects.clear();
}

bool MatchKeys::Sign(const uint8_t* data, size_t len, uint8_t* signature) const
{
	if (!m_active || m_privateKey == nullptr)
	{
		return false;
	}

	EVP_MD_CTX* ctx = EVP_MD_CTX_new();
	if (ctx == nullptr)
	{
		return false;
	}

	size_t signatureSize = SIGNATURE_SIZE;
	const bool signed_ = EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, m_privateKey) == 1
		&& EVP_DigestSign(ctx, signature, &signatureSize, data, len) == 1 && signatureSize == SIGNATURE_SIZE;
	EVP_MD_CTX_free(ctx);
	return signed_;
}

bool MatchKeys::Verify(int64_t userID, const uint8_t* data, size_t len, const uint8_t* signature)
{
	if (!m_active)
	{
		return false;
	}

	EVP_PKEY* key = nullptr;
	auto parsed = m_peerKeyObjects.find(userID);
	if (parsed != m_peerKeyObjects.end())
	{
		key = parsed->second;
	}
	else
	{
		auto raw = m_peerKeys.find(userID);
		if (raw == m_peerKeys.end())
		{
			return false;
		}
		key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, raw->second.data(), PUBLIC_KEY_SIZE);
		if (key == nullptr)
		{
			return false;
		}
		m_peerKeyObjects[userID] = key;
	}

	EVP_MD_CTX* ctx = EVP_MD_CTX_new();
	if (ctx == nullptr)
	{
		return false;
	}

	const bool valid = EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, key) == 1
		&& EVP_DigestVerify(ctx, signature, SIGNATURE_SIZE, data, len) == 1;
	EVP_MD_CTX_free(ctx);
	return valid;
}

std::vector<uint8_t> MatchKeys::BuildAnnouncement() const
{
	std::vector<uint8_t> message(ANNOUNCEMENT_SIZE);
	message[0] = KIND_ANNOUNCEMENT;
	WriteU64(message.data() + 1, static_cast<uint64_t>(m_localUserID));
	memcpy(message.data() + 9, m_publicKey.data(), PUBLIC_KEY_SIZE);
	return message;
}

std::vector<uint8_t> MatchKeys::BuildStatus() const
{
	std::vector<uint8_t> message;
	message.push_back(KIND_STATUS);
	message.push_back(0);
	uint8_t count = 0;
	for (const auto& entry : m_peerKeys)
	{
		if (count >= MAX_STATUS_USERS)
		{
			break;
		}
		uint8_t id[8];
		WriteU64(id, static_cast<uint64_t>(entry.first));
		message.insert(message.end(), id, id + 8);
		++count;
	}
	message[1] = count;
	return message;
}

bool MatchKeys::OnAnnouncement(int64_t connectionUserID, const uint8_t* data, size_t len, const char*& reason, bool& changed)
{
	changed = false;
	if (data == nullptr || len != ANNOUNCEMENT_SIZE || data[0] != KIND_ANNOUNCEMENT)
	{
		reason = "malformed or another version";
		return false;
	}

	const int64_t announcedUserID = static_cast<int64_t>(ReadU64(data + 1));
	if (announcedUserID != connectionUserID)
	{
		reason = "it names another user than the connection it arrived on";
		return false;
	}
	if (m_privateKey == nullptr || connectionUserID == m_localUserID)
	{
		reason = "this client has no key of its own yet or the key is its own";
		return false;
	}
	if (m_active && !ExpectsPeer(connectionUserID))
	{
		reason = "the user is not in the match roster";
		return false;
	}

	std::array<uint8_t, PUBLIC_KEY_SIZE> key;
	memcpy(key.data(), data + 9, PUBLIC_KEY_SIZE);

	auto existing = m_peerKeys.find(connectionUserID);
	if (existing != m_peerKeys.end())
	{
		if (existing->second == key)
		{
			reason = nullptr;
			return true; // the same key again
		}
		if (m_active)
		{
			reason = "this user already announced a different key";
			return false;
		}
		// a client that came back into the lobby makes a new key, the connection it arrives on vouches for it
		FreePeerKeyObject(connectionUserID);
		m_peerHolds.erase(connectionUserID);
	}

	m_peerKeys[connectionUserID] = key;
	changed = true;
	reason = nullptr;
	return true;
}

bool MatchKeys::OnStatus(int64_t connectionUserID, const uint8_t* data, size_t len, const char*& reason, bool& changed)
{
	changed = false;
	if (data == nullptr || len < 2 || data[0] != KIND_STATUS)
	{
		reason = "malformed or another version";
		return false;
	}
	const size_t count = data[1];
	if (count > MAX_STATUS_USERS || len != 2 + count * 8)
	{
		reason = "malformed status";
		return false;
	}

	std::set<int64_t> holds;
	for (size_t i = 0; i < count; ++i)
	{
		holds.insert(static_cast<int64_t>(ReadU64(data + 2 + i * 8)));
	}
	auto existing = m_peerHolds.find(connectionUserID);
	changed = existing == m_peerHolds.end() || existing->second != holds;
	m_peerHolds[connectionUserID] = holds;
	reason = nullptr;
	return true;
}
