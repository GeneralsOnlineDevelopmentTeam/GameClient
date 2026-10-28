#pragma once
#include <cstdint>
#include <vector>

namespace LobbyConnectionPolicy
{
inline bool IsCurrentAttempt(int64_t expectedLobby, uint64_t expectedGeneration, const std::vector<int64_t>& expectedMembers,
    int64_t lobby, uint64_t generation, const std::vector<int64_t>& members)
{
    return expectedLobby >= 0 && expectedLobby == lobby && expectedGeneration == generation && expectedMembers == members;
}

inline bool CanRetry(bool manualLobby, bool inMatch, bool member, bool local, bool connected, bool connecting, int64_t sinceLastAttemptMs)
{
    return manualLobby && !inMatch && member && !local && !connected && !connecting && sinceLastAttemptMs >= 1000;
}
}
