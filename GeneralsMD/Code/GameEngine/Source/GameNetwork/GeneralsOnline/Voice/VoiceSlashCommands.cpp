// VoiceSlashCommands.cpp
//
// See VoiceSlashCommands.h for the role of this file.
//
// This is the single source of truth for /voice chat commands. Both the
// in-game chat (InGameChat.cpp) and the Generals Online lobby chat
// (WOLGameSetupMenu.cpp) delegate here so the command set stays in sync.

#include "PreRTS.h"

#include "GameNetwork/GeneralsOnline/Voice/VoiceSlashCommands.h"

#ifdef ENABLE_VOICE_CHAT

#include "GameNetwork/GeneralsOnline/Voice/VoiceManager.h"
#include "GameNetwork/GeneralsOnline/OnlineServices_Init.h"
#include "GameNetwork/GeneralsOnline/OnlineServices_LobbyInterface.h"
#include "GameNetwork/GeneralsOnline/GeneralsOnline_Settings.h"
#include "GameNetwork/GeneralsOnline/NetworkMesh.h"

#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

namespace
{
    // ------------------------------------------------------------
    // Small helpers
    // ------------------------------------------------------------
    void Emit(const std::function<void(const UnicodeString&)>& out,
              const wchar_t* msg)
    {
        out(UnicodeString(msg));
    }

    void EmitFmt(const std::function<void(const UnicodeString&)>& out,
                 const wchar_t* fmt, ...)
    {
        UnicodeString s;
        va_list ap;
        va_start(ap, fmt);
        s.format_va(fmt, ap);
        va_end(ap);
        out(s);
    }

    // Convert a narrow (UTF-8 / ASCII) std::string into UnicodeString for
    // chat output.
    UnicodeString WideFromNarrow(const std::string& narrow)
    {
        std::wstring w;
        w.reserve(narrow.size());
        for (char c : narrow) w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
        return UnicodeString(w.c_str());
    }

    bool ParseInt64(const char* s, int64_t& out)
    {
        if (s == nullptr || *s == 0) return false;
        char* end = nullptr;
        long long v = std::strtoll(s, &end, 10);
        if (end == s) return false;
        out = static_cast<int64_t>(v);
        return true;
    }

    bool ParseFloat(const char* s, float& out)
    {
        if (s == nullptr || *s == 0) return false;
        char* end = nullptr;
        double v = std::strtod(s, &end);
        if (end == s) return false;
        out = static_cast<float>(v);
        return true;
    }

    bool ParseInt(const char* s, int& out)
    {
        if (s == nullptr || *s == 0) return false;
        char* end = nullptr;
        long v = std::strtol(s, &end, 10);
        if (end == s) return false;
        out = static_cast<int>(v);
        return true;
    }

    // Look up a display name for a peer user id, if the lobby knows it.
    // Falls back to an empty string.
    std::string LookupPeerName(int64_t userID)
    {
        NGMP_OnlineServices_LobbyInterface* pLobby =
            NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();
        if (pLobby == nullptr) return {};
        LobbyMemberEntry e = pLobby->GetRoomMemberFromID(userID);
        if (e.user_id == -1) return {};
        return e.display_name;
    }

    // Case-insensitive ASCII comparison. Display names in NGMP are UTF-8
    // but for the /voice command we only need rough equivalence; real
    // collisions are resolved through the ambiguous-match error path
    // below, which forces the user to disambiguate via userID.
    bool IEqualsAscii(const std::string& a, const std::string& b)
    {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
        {
            unsigned char ca = static_cast<unsigned char>(a[i]);
            unsigned char cb = static_cast<unsigned char>(b[i]);
            if (ca >= 'A' && ca <= 'Z') ca = static_cast<unsigned char>(ca + ('a' - 'A'));
            if (cb >= 'A' && cb <= 'Z') cb = static_cast<unsigned char>(cb + ('a' - 'A'));
            if (ca != cb) return false;
        }
        return true;
    }

    // Resolve an arg string to a single peer user id.
    //
    //  - If the arg parses as a positive integer, we treat it as a
    //    userID directly. The caller gets that id back regardless of
    //    whether that user is currently in our lobby: this lets the
    //    user unmute a saved id for someone who already left.
    //
    //  - Otherwise we scan the CURRENT lobby's member list for a
    //    display-name match (case-insensitive ASCII). Exactly one match
    //    -> success. Zero matches -> NotFound. More than one match ->
    //    Ambiguous. The caller is expected to report the error.
    //
    // IMPORTANT: Name-based lookup only ever resolves within the lobby
    // we can see RIGHT NOW. Two different NGMP accounts that happen to
    // share a display name therefore cannot be muted by accident, as
    // long as they are not both in the current lobby - the second
    // account will simply never match until the user adds it by id.
    enum class PeerResolveResult { Ok, NotFound, Ambiguous, LobbyUnavailable };

    PeerResolveResult ResolvePeerArg(const char* arg, int64_t& outID,
                                     std::string& outName,
                                     int& outMatchCount)
    {
        outID = 0;
        outName.clear();
        outMatchCount = 0;
        if (arg == nullptr || *arg == 0) return PeerResolveResult::NotFound;

        // Numeric path: accept any positive 64-bit int as a raw userID.
        int64_t parsed = 0;
        if (ParseInt64(arg, parsed) && parsed > 0)
        {
            outID = parsed;
            outName = LookupPeerName(parsed); // may be empty
            outMatchCount = 1;
            return PeerResolveResult::Ok;
        }

        // Name path: must consult the current lobby.
        NGMP_OnlineServices_LobbyInterface* pLobby =
            NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();
        if (pLobby == nullptr) return PeerResolveResult::LobbyUnavailable;

        const std::string target(arg);
        std::vector<LobbyMemberEntry>& members = pLobby->GetMembersListForCurrentRoom();
        int64_t firstID = 0;
        std::string firstName;
        for (const LobbyMemberEntry& m : members)
        {
            if (m.user_id <= 0) continue;
            if (!IEqualsAscii(m.display_name, target)) continue;
            ++outMatchCount;
            if (outMatchCount == 1)
            {
                firstID = m.user_id;
                firstName = m.display_name;
            }
        }

        if (outMatchCount == 0) return PeerResolveResult::NotFound;
        if (outMatchCount > 1)  return PeerResolveResult::Ambiguous;

        outID = firstID;
        outName = firstName;
        return PeerResolveResult::Ok;
    }

    // ------------------------------------------------------------
    // Sub-command implementations
    // ------------------------------------------------------------
    void CmdHelp(const std::function<void(const UnicodeString&)>& out)
    {
        Emit(out, L"Voice chat commands:");
        Emit(out, L"  /voice status                   - show current settings");
        Emit(out, L"  /voice mics                     - list microphone devices");
        Emit(out, L"  /voice mic <N>                  - select microphone by number");
        Emit(out, L"  /voice gain <0.0-4.0>           - set microphone input gain");
        Emit(out, L"  /voice volume <0.0-2.0>         - set master voice volume");
        Emit(out, L"  /voice peers                    - list remote voice peers");
        Emit(out, L"  /voice mute <name|userID>       - mute a peer (persistent)");
        Emit(out, L"  /voice unmute <name|userID>     - unmute a peer (persistent)");
        Emit(out, L"  /voice mutes                    - list saved ignores");
        Emit(out, L"  /voice peervol <name|userID> <0-2> - set per-peer volume");
        Emit(out, L"Push-to-talk: hold LEFT ALT while speaking.");
    }

    void CmdStatus(const std::function<void(const UnicodeString&)>& out)
    {
        if (TheVoiceManager == nullptr)
        {
            Emit(out, L"Voice subsystem is not initialised.");
            return;
        }

        Voice::VoiceMode mode = TheVoiceManager->GetMode();
        const wchar_t* modeStr = L"disabled";
        switch (mode)
        {
            case Voice::VoiceMode::LOBBY_ALL:    modeStr = L"lobby (all)";      break;
            case Voice::VoiceMode::IN_GAME_TEAM: modeStr = L"in-game (team)";   break;
            case Voice::VoiceMode::DISABLED:     modeStr = L"disabled";         break;
        }

        const std::wstring& dev = TheVoiceManager->GetCaptureDevice();
        EmitFmt(out, L"Voice: mode=%ls  mic='%ls'  gain=%.2f  volume=%.2f",
            modeStr,
            dev.empty() ? L"<default communications>" : dev.c_str(),
            TheVoiceManager->GetMicGain(),
            TheVoiceManager->GetGlobalVoiceVolume());
    }

    void CmdListMics(const std::function<void(const UnicodeString&)>& out)
    {
        std::vector<Voice::CaptureDeviceInfo> devices =
            Voice::VoiceManager::EnumerateCaptureDevices();
        if (devices.empty())
        {
            Emit(out, L"No capture devices found.");
            return;
        }
        EmitFmt(out, L"%d capture device(s):", (int)devices.size());

        const std::wstring& currentID =
            TheVoiceManager ? TheVoiceManager->GetCaptureDevice() : std::wstring();

        for (size_t i = 0; i < devices.size(); ++i)
        {
            const Voice::CaptureDeviceInfo& d = devices[i];
            const bool selected =
                (!currentID.empty() && currentID == d.id) ||
                (currentID.empty()  && d.isDefaultCommunications);

            EmitFmt(out, L"  %d%ls %ls%ls%ls",
                (int)(i + 1),
                selected ? L" *" : L"  ",
                d.friendlyName.c_str(),
                d.isDefaultCommunications ? L"  [default comms]" : L"",
                d.isDefaultConsole        ? L"  [default console]" : L"");
        }
        Emit(out, L"Use /voice mic <N> to select one.");
    }

    void CmdSelectMic(int index, const std::function<void(const UnicodeString&)>& out)
    {
        std::vector<Voice::CaptureDeviceInfo> devices =
            Voice::VoiceManager::EnumerateCaptureDevices();
        if (index < 1 || index > static_cast<int>(devices.size()))
        {
            EmitFmt(out, L"Invalid device number. Use /voice mics to see the list (1..%d).",
                    (int)devices.size());
            return;
        }
        const Voice::CaptureDeviceInfo& d = devices[index - 1];

        if (TheVoiceManager != nullptr)
        {
            TheVoiceManager->SetCaptureDevice(d.id);
        }

        // Persist to JSON settings.
        NGMP_OnlineServicesManager::Settings.Save_Voice_CaptureDeviceID(d.id);

        EmitFmt(out, L"Microphone set to: %ls", d.friendlyName.c_str());
    }

    void CmdGain(float gain, const std::function<void(const UnicodeString&)>& out)
    {
        if (gain < 0.0f) gain = 0.0f;
        if (gain > 4.0f) gain = 4.0f;
        if (TheVoiceManager != nullptr)
        {
            TheVoiceManager->SetMicGain(gain);
        }
        NGMP_OnlineServicesManager::Settings.Save_Voice_MicGain(gain);
        EmitFmt(out, L"Microphone gain set to %.2f", gain);
    }

    void CmdVolume(float volume, const std::function<void(const UnicodeString&)>& out)
    {
        if (volume < 0.0f) volume = 0.0f;
        if (volume > 2.0f) volume = 2.0f;
        if (TheVoiceManager != nullptr)
        {
            TheVoiceManager->SetGlobalVoiceVolume(volume);
        }
        NGMP_OnlineServicesManager::Settings.Save_Voice_GlobalVolume(volume);
        EmitFmt(out, L"Master voice volume set to %.2f", volume);
    }

    void CmdPeers(const std::function<void(const UnicodeString&)>& out)
    {
        NetworkMesh* pMesh = NGMP_OnlineServicesManager::GetNetworkMesh();
        if (pMesh == nullptr)
        {
            Emit(out, L"No active network mesh (not in a lobby/match).");
            return;
        }
        std::map<int64_t, PlayerConnection>& conns = pMesh->GetAllConnections();
        if (conns.empty())
        {
            Emit(out, L"No remote peers connected.");
            return;
        }
        EmitFmt(out, L"%d peer(s):", (int)conns.size());
        for (auto& kv : conns)
        {
            const int64_t id = kv.first;
            std::string name = LookupPeerName(id);
            const bool muted  = TheVoiceManager ? TheVoiceManager->IsPeerMuted(id) : false;
            const float pvol  = TheVoiceManager ? TheVoiceManager->GetPeerVolume(id) : 1.0f;
            const bool speaking = TheVoiceManager ? TheVoiceManager->IsPeerSpeaking(id) : false;

            EmitFmt(out, L"  %lld  %ls  vol=%.2f%ls%ls",
                (long long)id,
                name.empty() ? L"<unknown>" : WideFromNarrow(name).str(),
                pvol,
                muted    ? L"  [m