#pragma once
#include <algorithm>

namespace MeshConnectionTimeoutPolicy
{
constexpr int InitialTimeoutMs = 10000;
constexpr int ReconnectTimeoutMs = 30000;
inline int ClampReconnectTimeout(int value) { return std::clamp(value, InitialTimeoutMs, 60000); }
struct Timeouts { int initialMs; int connectedMs; };
inline Timeouts ForAttempt(bool retry, int reconnectTimeoutMs)
{
    const int extended = ClampReconnectTimeout(reconnectTimeoutMs);
    return {retry ? extended : InitialTimeoutMs, extended};
}
}
