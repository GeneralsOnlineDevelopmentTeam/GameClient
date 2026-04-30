// ----------------------------------------------------------------------------
// VoiceOptionsUI.cpp
//
// Runtime-built voice-chat options panel for the vanilla OptionsMenu.
// See VoiceOptionsUI.h for the architectural intent.
// ----------------------------------------------------------------------------

#include "PreRTS.h"

#if defined(ENABLE_VOICE_CHAT)

#include "GameNetwork/GeneralsOnline/Voice/VoiceOptionsUI.h"
#include "GameNetwork/GeneralsOnline/Voice/VoiceManager.h"
#include "GameNetwork/GeneralsOnline/Voice/VoiceCapture.h"
#include "GameNetwork/GeneralsOnline/GeneralsOnline_Settings.h"
#include "GameNetwork/GeneralsOnline/ngmp_interfaces.h"

#include "GameClient/GameWindow.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/GadgetCheckBox.h"
#include "GameClient/GadgetSlider.h"
#include "GameClient/GadgetComboBox.h"
#include "GameClient/GadgetStaticText.h"
#include "GameClient/GameText.h"
#include "Common/NameKeyGenerator.h"

#include <vector>

// ---------------------------------------------------------------------------
// Internal state: all pointers live for the lifetime of one OptionsMenu
// instance. They are cleared in Teardown(). Gadgets are owned by the
// WindowLayout, so we never destroy them manually.
// ---------------------------------------------------------------------------
namespace
{
    GameWindow* s_comboMic       = nullptr;
    GameWindow* s_sliderMicGain  = nullptr;
    GameWindow* s_sliderVoiceVol = nullptr;
    GameWindow* s_checkEnabled   = nullptr;
    GameWindow* s_labelHeader    = nullptr;
    GameWindow* s_labelMic       = nullptr;
    GameWindow* s_labelGain      = nullptr;
    GameWindow* s_labelVol       = nullptr;

    NameKeyType s_comboMicID       = NAMEKEY_INVALID;
    NameKeyType s_sliderMicGainID  = NAMEKEY_INVALID;
    NameKeyType s_sliderVoiceVolID = NAMEKEY_INVALID;
    NameKeyType s_checkEnabledID   = NAMEKEY_INVALID;

    // Bring voice namespace types into local scope for brevity.
    using Voice::VoiceManager;
    using Voice::CaptureDeviceInfo;

    std::vector<CaptureDeviceInfo> s_enumeratedDevices;

    // Slider range: we map the float settings 0.0..4.0 (gain) and
    // 0.0..2.0 (volume) onto integer slider ticks.
    const Int kGainSliderMin  = 0;
    const Int kGainSliderMax  = 400;  // 0.00 .. 4.00 step 0.01
    const Int kVolSliderMin   = 0;
    const Int kVolSliderMax   = 200;  // 0.00 .. 2.00 step 0.01

    // Vertical stack parameters for the runtime-created controls. We
    // anchor the panel at the screen's right edge relative to an existing
    // control (`sliderVoiceVolume`) that is guaranteed to exist in the
    // vanilla OptionsMenu layout.
    const Int kPanelPadX   = 12;  // horizontal gap from anchor
    const Int kRowHeight   = 22;
    const Int kRowSpacing  = 6;
    const Int kLabelWidth  = 90;
    const Int kCtrlWidth   = 200;

    GameWindow* LookupByName(const char* name)
    {
        NameKeyType id = TheNameKeyGenerator->nameToKey(name);
        return TheWindowManager->winGetWindowFromId(nullptr, id);
    }

    // Clone the visual style (WinInstanceData) of a template window onto
    // the instance data used to create a new gadget. Returns true on
    // success. The caller still has to set m_owner / m_textLabelString.
    bool CloneStyleFrom(GameWindow* tpl, WinInstanceData& out)
    {
        if (tpl == nullptr) return false;
        WinInstanceData* src = tpl->winGetInstanceData();
        if (src == nullptr) return false;
        out = *src;
        return true;
    }

    UnicodeString FetchLabel(const char* csfKey, const wchar_t* fallback)
    {
        UnicodeString s;
        if (TheGameText != nullptr)
        {
            s = TheGameText->fetch(csfKey);
        }
        if (s.isEmpty())
        {
            s = UnicodeString(fallback);
        }
        return s;
    }
} // anonymous namespace

// ---------------------------------------------------------------------------
// Build
// ---------------------------------------------------------------------------
void VoiceOptionsUI::Build(GameWindow* parent)
{
    Teardown(); // make sure we start clean on re-entry

    if (parent == nullptr || TheWindowManager == nullptr)
    {
        return;
    }

    // The voice checkbox lives as a real entry in the patched
    // OptionsMenu.wnd (see Window/Menus/OptionsMenu.wnd override). We
    // simply look it up by name and bind its initial checked state from
    // GenOnlineSettings. If a user runs on an unpatched .wnd, the lookup
    // returns null and we silently skip — slash commands still work.
    GenOnlineSettings& settings = NGMP_OnlineServicesManager::Settings;

    s_checkEnabled = LookupByName("OptionsMenu.wnd:VoiceOptions_CheckEnabled");
    if (s_checkEnabled != nullptr)
    {
        s_checkEnabledID = TheNameKeyGenerator->nameToKey(
            "OptionsMenu.wnd:VoiceOptions_CheckEnabled");
        GadgetCheckBoxSetChecked(s_checkEnabled, settings.Voice_GetEnabled());
    }

    // Mic gain slider: map float 0..4 onto slider min..max.
    s_sliderMicGain = LookupByName("OptionsMenu.wnd:VoiceOptions_SliderMicGain");
    if (s_sliderMicGain != nullptr)
    {
        s_sliderMicGainID = TheNameKeyGenerator->nameToKey(
            "OptionsMenu.wnd:VoiceOptions_SliderMicGain");
        Int mn = 0, mx = 100;
        GadgetSliderGetMinMax(s_sliderMicGain, &mn, &mx);
        float gain = settings.Voice_GetMicGain();
        if (gain < 0.0f) gain = 0.0f;
        if (gain > 4.0f) gain = 4.0f;
        Int pos = mn + (Int)((gain / 4.0f) * (float)(mx - mn) + 0.5f);
        GadgetSliderSetPosition(s_sliderMicGain, pos);
    }

    // Voice chat volume slider: map float 0..2 onto slider min..max.
    s_sliderVoiceVol = LookupByName("OptionsMenu.wnd:VoiceOptions_SliderVoiceVol");
    if (s_sliderVoiceVol != nullptr)
    {
        s_sliderVoiceVolID = TheNameKeyGenerator->nameToKey(
            "OptionsMenu.wnd:VoiceOptions_SliderVoiceVol");
        Int mn = 0, mx = 100;
        GadgetSliderGetMinMax(s_sliderVoiceVol, &mn, &mx);
        float vol = settings.Voice_GetGlobalVolume();
        if (vol < 0.0f) vol = 0.0f;
        if (vol > 2.0f) vol = 2.0f;
        Int pos = mn + (Int)((vol / 2.0f) * (float)(mx - mn) + 0.5f);
        GadgetSliderSetPosition(s_sliderVoiceVol, pos);
    }
    return;

    // ---- Legacy runtime-creation path (kept for reference) -----------

    // Mirror the proven runtime-checkbox pattern from PopupHostGame.cpp:
    // clone visual style from an existing checkbox in the same layout
    // and place the new gadget directly below it. We use CheckSendDelay
    // because it lives in the bottom-left "Network" region of the
    // OptionsMenu where there is space for an extra row.
    GameWindow* tplCheckBox = LookupByName("OptionsMenu.wnd:CheckSendDelay");
    if (tplCheckBox == nullptr)
    {
        // Fall back to AlternateMouse, which always exists.
        tplCheckBox = LookupByName("OptionsMenu.wnd:CheckAlternateMouse");
    }
    if (tplCheckBox == nullptr)
    {
        return;
    }

    // Clone style/visuals from the source checkbox.
    WinInstanceData* srcInstData = tplCheckBox->winGetInstanceData();
    WinInstanceData  instData;
    if (srcInstData != nullptr)
    {
        instData = *srcInstData;
    }
    else
    {
        instData.init();
    }
    instData.m_owner = parent;
    instData.m_textLabelString = "GUI:VoiceChat";

    // Match the source checkbox's exact size; place one row below it.
    Int chkW = 0, chkH = 0;
    tplCheckBox->winGetSize(&chkW, &chkH);
    Int chkX = 0, chkY = 0;
    tplCheckBox->winGetPosition(&chkX, &chkY);

    const UnsignedInt status = tplCheckBox->winGetStatus();

    s_checkEnabled = TheWindowManager->gogoGadgetCheckbox(
        parent,
        status,
        chkX,
        chkY + chkH + 4, // one checkbox below source, small gap
        chkW,
        chkH,
        &instData,
        nullptr, // keep default font from instance data
        FALSE);  // visuals already cloned

    if (s_checkEnabled != nullptr)
    {
        s_checkEnabledID = TheNameKeyGenerator->nameToKey(
            "OptionsMenu.wnd:VoiceOptions_CheckEnabled");
        s_checkEnabled->winSetWindowId((Int)s_checkEnabledID);

        UnicodeString label = FetchLabel("GUI:VoiceChat", L"Voice Chat");
        GadgetCheckBoxSetText(s_checkEnabled, label);
        GadgetCheckBoxSetChecked(s_checkEnabled,
            NGMP_OnlineServicesManager::Settings.Voice_GetEnabled());
    }
}

// ---------------------------------------------------------------------------
// Teardown
// ---------------------------------------------------------------------------
void VoiceOptionsUI::Teardown()
{
    s_comboMic       = nullptr;
    s_sliderMicGain  = nullptr;
    s_sliderVoiceVol = nullptr;
    s_checkEnabled   = nullptr;
    s_labelHeader    = nullptr;
    s_labelMic       = nullptr;
    s_labelGain      = nullptr;
    s_labelVol       = nullptr;
    s_comboMicID       = NAMEKEY_INVALID;
    s_sliderMicGainID  = NAMEKEY_INVALID;
    s_sliderVoiceVolID = NAMEKEY_INVALID;
    s_checkEnabledID   = NAMEKEY_INVALID;
    s_enumeratedDevices.clear();
}

// ---------------------------------------------------------------------------
// TryHandleEvent
// ---------------------------------------------------------------------------
bool VoiceOptionsUI::TryHandleEvent(GameWindow* control, unsigned int /*msg*/, void* /*mData2*/)
{
    if (control == nullptr) return false;

    // We don't really need to act on live events — the final values are
    // read out in Save() from the gadgets themselves. But claim the event
    // so OptionsMenu's default handler doesn't misinterpret our controls.
    if (control == s_comboMic ||
        control == s_sliderMicGain ||
        control == s_sliderVoiceVol ||
        control == s_checkEnabled)
    {
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Save
// ---------------------------------------------------------------------------
void VoiceOptionsUI::Save()
{
    GenOnlineSettings& settings = NGMP_OnlineServicesManager::Settings;

    // Only the Enabled checkbox is currently built at runtime; the
    // remaining voice settings (mic device, gain, volume) are managed
    // via /voice slash commands until they get a real .wnd entry.
    if (s_checkEnabled != nullptr)
    {
        const bool enabled = (GadgetCheckBoxIsChecked(s_checkEnabled) != FALSE);
        settings.Save_Voice_Enabled(enabled);
    }

    if (s_sliderMicGain != nullptr)
    {
        Int mn = 0, mx = 100;
        GadgetSliderGetMinMax(s_sliderMicGain, &mn, &mx);
        const Int pos = GadgetSliderGetPosition(s_sliderMicGain);
        const float range = (float)(mx - mn);
        const float gain  = (range > 0.0f) ? ((float)(pos - mn) / range) * 4.0f : 0.0f;
        settings.Save_Voice_MicGain(gain);
    }

    if (s_sliderVoiceVol != nullptr)
    {
        Int mn = 0, mx = 100;
        GadgetSliderGetMinMax(s_sliderVoiceVol, &mn, &mx);
        const Int pos = GadgetSliderGetPosition(s_sliderVoiceVol);
        const float range = (float)(mx - mn);
        const float vol   = (range > 0.0f) ? ((float)(pos - mn) / range) * 2.0f : 0.0f;
        settings.Save_Voice_GlobalVolume(vol);
    }
}

#endif // ENABLE_VOICE_CHAT
