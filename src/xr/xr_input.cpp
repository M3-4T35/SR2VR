#include "xr/xr_input.h"
#include "input/keys.h"
#include "util/log.h"

#include <cmath>
#include <cstring>
#include <vector>

namespace xr {
namespace {

// Stick : engagé au-delà de 0,7, relâché en dessous de 0,4 (évite les rebonds)
constexpr float kEngage = 0.7f;
constexpr float kRelease = 0.4f;
// Répétition d'une flèche tenue, comme un clavier
constexpr ULONGLONG kRepeatDelayMs = 400;
constexpr ULONGLONG kRepeatPeriodMs = 130;

XrPath ToPath(XrInstance instance, const char* text)
{
    XrPath path = XR_NULL_PATH;
    xrStringToPath(instance, text, &path);
    return path;
}

struct Binding {
    int action; // 0 naviguer, 1 valider, 2 retour
    const char* path;
};

struct Profile {
    const char* name;
    std::vector<Binding> bindings;
};

// Seul le profil Touch est validé au casque (Quest 2 par SteamVR). Les autres suivent la
// spécification OpenXR ; un profil refusé par le runtime est signalé dans le log.
const Profile kProfiles[] = {
    {"/interaction_profiles/oculus/touch_controller",
     {{0, "/user/hand/left/input/thumbstick"},
      {0, "/user/hand/right/input/thumbstick"},
      {1, "/user/hand/right/input/a/click"},
      {1, "/user/hand/right/input/trigger/value"},
      {2, "/user/hand/right/input/b/click"},
      {2, "/user/hand/left/input/menu/click"}}},
    {"/interaction_profiles/valve/index_controller",
     {{0, "/user/hand/left/input/thumbstick"},
      {0, "/user/hand/right/input/thumbstick"},
      {1, "/user/hand/right/input/a/click"},
      {1, "/user/hand/right/input/trigger/click"},
      {2, "/user/hand/right/input/b/click"},
      {2, "/user/hand/left/input/b/click"}}},
    {"/interaction_profiles/microsoft/motion_controller",
     {{0, "/user/hand/left/input/thumbstick"},
      {0, "/user/hand/right/input/thumbstick"},
      {1, "/user/hand/right/input/trigger/value"},
      {2, "/user/hand/left/input/menu/click"},
      {2, "/user/hand/right/input/menu/click"}}},
    {"/interaction_profiles/htc/vive_controller",
     {{0, "/user/hand/left/input/trackpad"},
      {0, "/user/hand/right/input/trackpad"},
      {1, "/user/hand/right/input/trigger/click"},
      {2, "/user/hand/left/input/menu/click"},
      {2, "/user/hand/right/input/menu/click"}}},
    {"/interaction_profiles/khr/simple_controller",
     {{1, "/user/hand/left/input/select/click"},
      {1, "/user/hand/right/input/select/click"},
      {2, "/user/hand/left/input/menu/click"},
      {2, "/user/hand/right/input/menu/click"}}},
};

bool CreateAction(XrActionSet set, XrActionType type, const char* name, const char* label, XrAction* action)
{
    XrActionCreateInfo ci{XR_TYPE_ACTION_CREATE_INFO};
    ci.actionType = type;
    strcpy_s(ci.actionName, name);
    strcpy_s(ci.localizedActionName, label);
    return XR_SUCCEEDED(xrCreateAction(set, &ci, action));
}

} // namespace

bool MenuInput::Init(XrInstance instance, XrSession session)
{
    arrows_[0].vk = VK_UP;
    arrows_[1].vk = VK_DOWN;
    arrows_[2].vk = VK_LEFT;
    arrows_[3].vk = VK_RIGHT;
    enter_.vk = VK_RETURN;
    escape_.vk = VK_ESCAPE;

    XrActionSetCreateInfo si{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy_s(si.actionSetName, "menus");
    strcpy_s(si.localizedActionSetName, "Menus");
    if (XR_FAILED(xrCreateActionSet(instance, &si, &set_)) ||
        !CreateAction(set_, XR_ACTION_TYPE_VECTOR2F_INPUT, "navigate", "Naviguer (fleches)", &navigate_) ||
        !CreateAction(set_, XR_ACTION_TYPE_BOOLEAN_INPUT, "confirm", "Valider (Entree)", &confirm_) ||
        !CreateAction(set_, XR_ACTION_TYPE_BOOLEAN_INPUT, "back", "Retour (Echap)", &back_)) {
        LOG_XR("ERREUR : actions des manettes non creees, manettes desactivees");
        return false;
    }

    const XrAction actions[3] = {navigate_, confirm_, back_};
    int accepted = 0;
    for (const Profile& profile : kProfiles) {
        std::vector<XrActionSuggestedBinding> suggested;
        for (const Binding& b : profile.bindings)
            suggested.push_back({actions[b.action], ToPath(instance, b.path)});
        XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        sb.interactionProfile = ToPath(instance, profile.name);
        sb.countSuggestedBindings = static_cast<uint32_t>(suggested.size());
        sb.suggestedBindings = suggested.data();
        const XrResult r = xrSuggestInteractionProfileBindings(instance, &sb);
        if (XR_SUCCEEDED(r))
            ++accepted;
        else
            LOG_XR("manettes : profil %s refuse par le runtime (%d)", profile.name, static_cast<int>(r));
    }

    XrSessionActionSetsAttachInfo ai{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    ai.countActionSets = 1;
    ai.actionSets = &set_;
    if (!accepted || XR_FAILED(xrAttachSessionActionSets(session, &ai))) {
        LOG_XR("ERREUR : xrAttachSessionActionSets, manettes desactivees");
        return false;
    }
    ready_ = true;
    LOG_XR("manettes : %d profil(s) acceptes ; stick = fleches, A ou gachette droite = Entree, B ou menu gauche = Echap",
           accepted);
    return true;
}

void MenuInput::SetKey(Key& key, bool pressed, bool repeat)
{
    const ULONGLONG now = GetTickCount64();
    if (pressed && !key.held) {
        if (input::SendKey(key.vk, true)) {
            key.held = true;
            key.nextRepeat = now + kRepeatDelayMs;
            if (!announced_) {
                LOG_XR("manettes : premiere touche envoyee au jeu (code 0x%02X)", key.vk);
                announced_ = true;
            }
        }
    } else if (pressed && repeat && now >= key.nextRepeat) {
        input::SendKey(key.vk, true); // nouvel appui sans relâchement, comme la répétition d'un clavier
        key.nextRepeat = now + kRepeatPeriodMs;
    } else if (!pressed && key.held) {
        input::SendKey(key.vk, false);
        key.held = false;
    }
}

void MenuInput::ReleaseAll()
{
    for (Key& key : arrows_)
        SetKey(key, false, false);
    SetKey(enter_, false, false);
    SetKey(escape_, false, false);
    direction_ = -1;
}

void MenuInput::Update(XrSession session, bool focused)
{
    if (!ready_)
        return;
    if (!focused) {
        ReleaseAll();
        return;
    }
    XrActiveActionSet active{set_, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    if (xrSyncActions(session, &sync) != XR_SUCCESS) { // XR_SESSION_NOT_FOCUSED compris
        ReleaseAll();
        return;
    }

    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = navigate_;
    XrActionStateVector2f stick{XR_TYPE_ACTION_STATE_VECTOR2F};
    float x = 0.0f, y = 0.0f;
    if (XR_SUCCEEDED(xrGetActionStateVector2f(session, &gi, &stick)) && stick.isActive) {
        x = stick.currentState.x;
        y = stick.currentState.y;
    }
    // Axe dominant ; la direction courante tient jusqu'à ce que le stick revienne vers le centre
    const float ax = std::fabs(x), ay = std::fabs(y);
    int wanted = -1;
    if (ay >= ax && ay > kEngage)
        wanted = y > 0.0f ? 0 : 1;
    else if (ax > ay && ax > kEngage)
        wanted = x < 0.0f ? 2 : 3;
    if (wanted == -1 && direction_ != -1) {
        const float along = direction_ < 2 ? ay : ax;
        if (along > kRelease)
            wanted = direction_;
    }
    direction_ = wanted;
    for (int i = 0; i < 4; ++i)
        SetKey(arrows_[i], i == direction_, true);

    XrActionStateBoolean button{XR_TYPE_ACTION_STATE_BOOLEAN};
    gi.action = confirm_;
    const bool confirm =
        XR_SUCCEEDED(xrGetActionStateBoolean(session, &gi, &button)) && button.isActive && button.currentState;
    SetKey(enter_, confirm, false);
    button = {XR_TYPE_ACTION_STATE_BOOLEAN};
    gi.action = back_;
    const bool back =
        XR_SUCCEEDED(xrGetActionStateBoolean(session, &gi, &button)) && button.isActive && button.currentState;
    SetKey(escape_, back, false);
}

} // namespace xr
