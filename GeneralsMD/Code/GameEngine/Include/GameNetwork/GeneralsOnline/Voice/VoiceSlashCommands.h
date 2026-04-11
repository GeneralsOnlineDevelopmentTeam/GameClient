// VoiceSlashCommands.h
//
// Shared handler for /voice ... chat commands. Used by both the in-game
// chat and the Generals Online lobby chat so voice chat can be tuned from
// any chat input without needing a bespoke UI.
//
// The caller is expected to have already matched the "voice" token and
// just passes the remainder of the line (may be empty).

#pragma once

#ifdef ENABLE_VOICE_CHAT

#include "Common/AsciiString.h"
#include "Common/UnicodeString.h"
#include <functional>

namespace Voice
{
    // Runs a /voice sub-command. `args` is everything after "/voice "
    // (may be empty - defaults to help/status).
    // `out` is invoked once per line of textual feedback to display to
    // the local user (not sent to other players).
    void HandleVoiceSlashCommand(AsciiString args,
                                 std::function<void(const UnicodeString&)> out);
}

#endif // ENABLE_VOICE_CHAT
