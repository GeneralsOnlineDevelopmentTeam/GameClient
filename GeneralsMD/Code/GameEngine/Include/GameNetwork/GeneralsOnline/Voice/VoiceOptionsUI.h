// ----------------------------------------------------------------------------
// VoiceOptionsUI.h
//
// GeneralsOnline voice-chat options panel. Builds its own UI controls at
// runtime on top of the existing OptionsMenu layout so no .wnd modifications
// are required — every user that updates their .exe gets the voice panel
// automatically. The module exposes four lightweight entry points that
// OptionsMenu.cpp calls at well-defined lifecycle points.
//
// All functions are safe to call when ENABLE_VOICE_CHAT is undefined — they
// become no-ops via the header guard below.
// ----------------------------------------------------------------------------

#pragma once

#if defined(ENABLE_VOICE_CHAT)

class GameWindow;

namespace VoiceOptionsUI
{
    // Called once from OptionsMenuInit() after the vanilla init is done.
    // Clones template controls (slider / combobox / checkbox) from the
    // already-loaded OptionsMenu layout and populates the voice panel with
    // current settings from NGMP_OnlineServicesManager::Settings.
    //
    // `parent` is the OptionsMenu parent window (the one that winSetModal is
    // called on at the end of OptionsMenuInit).
    void Build(GameWindow* parent);

    // Called from OptionsMenuShutdown() to release references. Does not
    // destroy the created gadgets — they die with the WindowLayout.
    void Teardown();

    // Called from OptionsMenuSystem() before the standard GBM_SELECTED /
    // GSM_SLIDER_TRACK dispatch. Returns true if the event was consumed
    // by a voice-panel control, in which case OptionsMenu should stop
    // processing the message.
    bool TryHandleEvent(GameWindow* control, unsigned int msg, void* mData2);

    // Called from the buttonAccept branch of OptionsMenuSystem() right
    // before the layout is destroyed. Writes the current voice-panel
    // values into GenOnlineSettings and applies them to TheVoiceManager.
    void Save();
}

#endif // ENABLE_VOICE_CHAT
