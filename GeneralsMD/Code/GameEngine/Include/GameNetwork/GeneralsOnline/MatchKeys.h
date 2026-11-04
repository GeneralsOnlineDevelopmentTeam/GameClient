#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <vector>

typedef struct evp_pkey_st EVP_PKEY;

// Ed25519 keys: each client makes a key pair when it joins or creates a lobby and announces the public key over each peer's own connection,
// a key counts for the user whose connection it came on. A match binds to the keys when it starts: the match id and the roster go into everything
// that is signed, so a signature is good for that match only. See ConnectionManager for what is signed.
//
// Channel messages, reliable:
//   announcement: u8 kind (2), i64 user id, 32 bytes public key
//   status:       u8 kind (3), u8 count, count x i64 user id of the peers whose key the sender holds
class MatchKeys
{
public:
	enum { PUBLIC_KEY_SIZE = 32, SIGNATURE_SIZE = 64, ANNOUNCEMENT_SIZE = 1 + 8 + PUBLIC_KEY_SIZE, MAX_STATUS_USERS = 16 };
	enum { KIND_ANNOUNCEMENT = 2, KIND_STATUS = 3 };

	MatchKeys();
	~MatchKeys();

	bool BeginLobby(int64_t localUserID);
	/// Drops the key pair and every peer key, call when the lobby is left.
	void End();
	bool HasLocalKey() const { return m_privateKey != nullptr; }
	int64_t GetLocalUserID() const { return m_localUserID; }

	bool BeginMatch(uint64_t matchID, const std::set<int64_t>& expectedPeers);
	void EndMatch();
	bool IsActive() const { return m_active; }

	std::vector<uint8_t> BuildAnnouncement() const;
	/// The keys held, the host reads it to see that every pair has exchanged keys.
	std::vector<uint8_t> BuildStatus() const;

	/// Takes an announcement that arrived on the connection of connectionUserID. False and a reason when rejected, changed when a key was added or replaced.
	bool OnAnnouncement(int64_t connectionUserID, const uint8_t* data, size_t len, const char*& reason, bool& changed);
	/// Takes a status that arrived on the connection of connectionUserID.
	bool OnStatus(int64_t connectionUserID, const uint8_t* data, size_t len, const char*& reason, bool& changed);

	/// Drops the key of a peer that left the lobby, never one of a bound match roster.
	bool DropPeer(int64_t userID);

	size_t GetPeerKeyCount() const { return m_peerKeys.size(); }
	size_t GetExpectedPeers() const { return m_expectedPeers.size(); }
	/// True when bound and a key is held for every peer of the roster.
	bool IsReady() const;
	bool ExpectsPeer(int64_t userID) const { return m_expectedPeers.count(userID) != 0; }
	bool HasPeerKey(int64_t userID) const { return m_peerKeys.find(userID) != m_peerKeys.end(); }
	std::vector<int64_t> GetPeerKeyUsers() const;
	/// True when peer reported holding the keys of every user in needed, peer itself excepted.
	bool PeerHoldsAll(int64_t peer, const std::set<int64_t>& needed) const;
	uint64_t GetMatchID() const { return m_matchID; }

	bool Sign(const uint8_t* data, size_t len, uint8_t* signature) const;
	/// Checks a signature of userID over data, false without a key of that user or when not bound to a match.
	bool Verify(int64_t userID, const uint8_t* data, size_t len, const uint8_t* signature);

private:
	void FreePeerKeyObject(int64_t userID);
	void FreePeerKeyObjects();

	bool m_active;
	uint64_t m_matchID;
	int64_t m_localUserID;
	std::set<int64_t> m_expectedPeers;
	EVP_PKEY* m_privateKey;
	std::array<uint8_t, PUBLIC_KEY_SIZE> m_publicKey;
	std::map<int64_t, std::array<uint8_t, PUBLIC_KEY_SIZE>> m_peerKeys;
	std::map<int64_t, std::set<int64_t>> m_peerHolds;	///< Per peer, the users whose key it said it holds.
	std::map<int64_t, EVP_PKEY*> m_peerKeyObjects;	///< The peer keys parsed for OpenSSL, made on first use.
};
