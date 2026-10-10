#include "xr/xr_input.h"
#include "input/keys.h"
#include "util/config.h"
#include "util/log.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

namespace xr {
namespace {

// Menus : stick engagé au-delà de 0,7, relâché en dessous de 0,4 (évite les rebonds)
constexpr float kEngage = 0.7f;
constexpr float kRelease = 0.4f;
// Répétition d'une flèche tenue, comme un clavier
constexpr ULONGLONG kRepeatDelayMs = 400;
constexpr ULONGLONG kRepeatPeriodMs = 130;
// Partie : une direction de déplacement s'enfonce au-delà de 0,45 et se relâche sous 0,3
constexpr float kMoveEngage = 0.45f;
constexpr float kMoveRelease = 0.3f;
constexpr float kTurnDeadzone = 0.15f;
// Sans image de jeu préparée depuis ce délai, on est dans un menu
constexpr ULONGLONG kGameplayTimeoutMs = 300;

// Positions des touches (codes de balayage) : W, S, A, D et E d'un clavier QWERTY
constexpr WORD kScanForward = 0x11, kScanBack = 0x1F, kScanLeft = 0x1E, kScanRight = 0x20, kScanInteract = 0x12;
// Touches par défaut du jeu : Espace pour sauter, C pour s'accroupir (bascule)
constexpr int kScanJump = 0x39, kScanCrouch = 0x2E;

std::atomic<ULONGLONG> g_gameplayTick{0};

XrPath ToPath(XrInstance instance, const char* text)
{
    XrPath path = XR_NULL_PATH;
    xrStringToPath(instance, text, &path);
    return path;
}

// Actions, dans l'ordre du tableau `actions` de Init
enum { kLeftStick, kRightStick, kConfirm, kBack, kLeftTrigger, kRightTrigger, kRightGrip, kActionCount };

struct Binding {
    int action;
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
     {{kLeftStick, "/user/hand/left/input/thumbstick"},
      {kRightStick, "/user/hand/right/input/thumbstick"},
      {kConfirm, "/user/hand/right/input/a/click"},
      {kBack, "/user/hand/right/input/b/click"},
      {kBack, "/user/hand/left/input/menu/click"},
      {kLeftTrigger, "/user/hand/left/input/trigger/value"},
      {kRightTrigger, "/user/hand/right/input/trigger/value"},
      {kRightGrip, "/user/hand/right/input/squeeze/value"}}},
    {"/interaction_profiles/valve/index_controller",
     {{kLeftStick, "/user/hand/left/input/thumbstick"},
      {kRightStick, "/user/hand/right/input/thumbstick"},
      {kConfirm, "/user/hand/right/input/a/click"},
      {kBack, "/user/hand/right/input/b/click"},
      {kBack, "/user/hand/left/input/b/click"},
      {kLeftTrigger, "/user/hand/left/input/trigger/value"},
      {kRightTrigger, "/user/hand/right/input/trigger/value"},
      {kRightGrip, "/user/hand/right/input/squeeze/value"}}},
    {"/interaction_profiles/microsoft/motion_controller",
     {{kLeftStick, "/user/hand/left/input/thumbstick"},
      {kRightStick, "/user/hand/right/input/thumbstick"},
      {kConfirm, "/user/hand/right/input/trackpad/click"},
      {kBack, "/user/hand/left/input/menu/click"},
      {kBack, "/user/hand/right/input/menu/click"},
      {kLeftTrigger, "/user/hand/left/input/trigger/value"},
      {kRightTrigger, "/user/hand/right/input/trigger/value"},
      {kRightGrip, "/user/hand/right/input/squeeze/click"}}},
    {"/interaction_profiles/htc/vive_controller",
     {{kLeftStick, "/user/hand/left/input/trackpad"},
      {kRightStick, "/user/hand/right/input/trackpad"},
      {kConfirm, "/user/hand/right/input/trackpad/click"},
      {kBack, "/user/hand/left/input/menu/click"},
      {kBack, "/user/hand/right/input/menu/click"},
      {kLeftTrigger, "/user/hand/left/input/trigger/value"},
      {kRightTrigger, "/user/hand/right/input/trigger/value"},
      {kRightGrip, "/user/hand/right/input/squeeze/click"}}},
    {"/interaction_profiles/khr/simple_controller",
     {{kConfirm, "/user/hand/left/input/select/click"},
      {kConfirm, "/user/hand/right/input/select/click"},
      {kBack, "/user/hand/left/input/menu/click"},
      {kBack, "/user/hand/right/input/menu/click"}}},
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

void NoteGameplayFrame()
{
    const ULONGLONG now = GetTickCount64();
    g_gameplayTick = now ? now : 1;
}

void MenuInput::ReadConfig()
{
    turnSpeed_ = config::Float("Controls", "turn_speed", 1200.0f);
    if (!(turnSpeed_ >= -20000.0f && turnSpeed_ <= 20000.0f))
        turnSpeed_ = 1200.0f;
    // Touches du saut et de l'accroupissement, si elles ont été changées dans le jeu
    const auto assign = [this](Key& key, const char* name, int fallback) {
        int code = config::Int("Controls", name, fallback);
        if (code < 1 || code > 0x7F)
            code = fallback;
        if (key.code != code) {
            SetKey(key, false, false); // relâche l'ancienne touche avant d'en changer
            key.code = static_cast<WORD>(code);
        }
        key.scan = true;
    };
    assign(jump_, "jump_scan", kScanJump);
    assign(crouch_, "crouch_scan", kScanCrouch);
}

bool MenuInput::Init(XrInstance instance, XrSession session)
{
    const WORD arrowKeys[4] = {VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT};
    const WORD moveScans[4] = {kScanForward, kScanBack, kScanLeft, kScanRight};
    for (int i = 0; i < 4; ++i) {
        arrows_[i].code = arrowKeys[i];
        move_[i].code = moveScans[i];
        move_[i].scan = true;
    }
    enter_.code = VK_RETURN;
    escape_.code = VK_ESCAPE;
    control_.code = VK_LCONTROL;
    shift_.code = VK_LSHIFT;
    interact_.code = kScanInteract;
    interact_.scan = true;
    ReadConfig();
    QueryPerformanceCounter(&lastUpdate_);

    XrActionSetCreateInfo si{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy_s(si.actionSetName, "sr2vr");
    strcpy_s(si.localizedActionSetName, "Saints Row 2 VR");
    if (XR_FAILED(xrCreateActionSet(instance, &si, &set_)) ||
        !CreateAction(set_, XR_ACTION_TYPE_VECTOR2F_INPUT, "left_stick", "Se deplacer / naviguer", &leftStick_) ||
        !CreateAction(set_, XR_ACTION_TYPE_VECTOR2F_INPUT, "right_stick", "Tourner / naviguer", &rightStick_) ||
        !CreateAction(set_, XR_ACTION_TYPE_BOOLEAN_INPUT, "confirm", "Valider (Entree)", &confirm_) ||
        !CreateAction(set_, XR_ACTION_TYPE_BOOLEAN_INPUT, "back", "Retour (Echap)", &back_) ||
        !CreateAction(set_, XR_ACTION_TYPE_BOOLEAN_INPUT, "left_trigger", "Ctrl", &leftTrigger_) ||
        !CreateAction(set_, XR_ACTION_TYPE_BOOLEAN_INPUT, "right_trigger", "Maj", &rightTrigger_) ||
        !CreateAction(set_, XR_ACTION_TYPE_BOOLEAN_INPUT, "right_grip", "Interagir (E)", &rightGrip_)) {
        LOG_XR("ERREUR : actions des manettes non creees, manettes desactivees");
        return false;
    }

    const XrAction actions[kActionCount] = {leftStick_, rightStick_, confirm_, back_, leftTrigger_, rightTrigger_,
                                            rightGrip_};
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
    LOG_XR("manettes : %d profil(s) acceptes. Partie : stick gauche = deplacement, stick droit = tourner (%.0f px/s), "
           "haut = sauter (position 0x%02X), bas = s'accroupir (position 0x%02X), poignee droite = E. Menus : sticks = "
           "fleches. A = Entree, B ou menu gauche = Echap, gachettes = Ctrl / Maj",
           accepted, turnSpeed_, jump_.code, crouch_.code);
    return true;
}

void MenuInput::SetKey(Key& key, bool pressed, bool repeat)
{
    const auto send = [&key](bool down) {
        return key.scan ? input::SendScan(key.code, down) : input::SendKey(key.code, down);
    };
    const ULONGLONG now = GetTickCount64();
    if (pressed && !key.held) {
        if (send(true)) {
            key.held = true;
            key.nextRepeat = now + kRepeatDelayMs;
            if (!announced_) {
                LOG_XR("manettes : premiere touche envoyee au jeu (%s 0x%02X)", key.scan ? "position" : "code", key.code);
                announced_ = true;
            }
        }
    } else if (pressed && repeat && now >= key.nextRepeat) {
        send(true); // nouvel appui sans relâchement, comme la répétition d'un clavier
        key.nextRepeat = now + kRepeatPeriodMs;
    } else if (!pressed && key.held) {
        send(false);
        key.held = false;
    }
}

void MenuInput::ReleaseAll()
{
    for (Key& key : arrows_)
        SetKey(key, false, false);
    for (Key& key : move_)
        SetKey(key, false, false);
    SetKey(enter_, false, false);
    SetKey(escape_, false, false);
    SetKey(control_, false, false);
    SetKey(shift_, false, false);
    SetKey(interact_, false, false);
    SetKey(jump_, false, false);
    SetKey(crouch_, false, false);
    direction_ = -1;
    turnRemainder_ = 0.0f;
}

void MenuInput::Update(XrSession session, bool focused)
{
    LARGE_INTEGER counter, frequency;
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    float dt = static_cast<float>(counter.QuadPart - lastUpdate_.QuadPart) / static_cast<float>(frequency.QuadPart);
    lastUpdate_ = counter;
    if (!(dt > 0.0f && dt < 0.1f))
        dt = 0.0f; // premier appel ou reprise après une interruption

    if (!ready_)
        return;
    if (!focused) {
        ReleaseAll();
        return;
    }
    const ULONGLONG now = GetTickCount64();
    if (now - lastConfig_ >= 1000) {
        lastConfig_ = now;
        ReadConfig();
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
    const auto pressed = [&](XrAction action) {
        XrActionStateBoolean button{XR_TYPE_ACTION_STATE_BOOLEAN};
        gi.action = action;
        return XR_SUCCEEDED(xrGetActionStateBoolean(session, &gi, &button)) && button.isActive && button.currentState;
    };
    const auto stick = [&](XrAction action, float* x, float* y) {
        XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
        gi.action = action;
        *x = *y = 0.0f;
        if (XR_SUCCEEDED(xrGetActionStateVector2f(session, &gi, &state)) && state.isActive) {
            *x = state.currentState.x;
            *y = state.currentState.y;
        }
    };

    // Modificateurs d'abord : ils doivent être enfoncés avant la touche qu'ils accompagnent
    SetKey(control_, pressed(leftTrigger_), false);
    SetKey(shift_, pressed(rightTrigger_), false);

    float lx, ly, rx, ry;
    stick(leftStick_, &lx, &ly);
    stick(rightStick_, &rx, &ry);

    const ULONGLONG gameplayTick = g_gameplayTick;
    const bool gameplay = gameplayTick && now >= gameplayTick && now - gameplayTick < kGameplayTimeoutMs;
    if (static_cast<int>(gameplay) != gameplayShown_) {
        LOG_XR("manettes : mode %s", gameplay ? "partie (deplacement, tourner, interagir)" : "menus (fleches)");
        gameplayShown_ = gameplay;
    }

    if (gameplay) {
        // Les flèches des menus sont relâchées
        for (Key& key : arrows_)
            SetKey(key, false, false);
        direction_ = -1;

        // Déplacement : chaque direction a sa touche, avec hystérésis ; les diagonales sont permises
        const float axis[4] = {ly, -ly, -lx, lx}; // avant, arrière, gauche, droite
        for (int i = 0; i < 4; ++i)
            SetKey(move_[i], axis[i] > (move_[i].held ? kMoveRelease : kMoveEngage), false);

        // Sauter / s'accroupir : stick droit poussé franchement vers le haut ou le bas. Axe
        // dominant à l'engagement, pour ne pas sauter en tournant ; la touche tient jusqu'au
        // retour du stick vers le centre (le jeu accroupit par bascule : un appui suffit).
        const bool vertical = std::fabs(ry) > std::fabs(rx);
        SetKey(jump_, ry > (jump_.held ? kRelease : kEngage) && (jump_.held || vertical), false);
        SetKey(crouch_, -ry > (crouch_.held ? kRelease : kEngage) && (crouch_.held || vertical), false);

        // Tourner : la souris se déplace à l'horizontale, proportionnellement au stick. Pas
        // pendant un saut ou un accroupissement commandé au stick, qui dévie toujours un peu.
        float turn = 0.0f;
        if (!jump_.held && !crouch_.held && std::fabs(rx) > kTurnDeadzone)
            turn = (rx - (rx > 0.0f ? kTurnDeadzone : -kTurnDeadzone)) / (1.0f - kTurnDeadzone);
        turnRemainder_ += turn * turnSpeed_ * dt;
        const int pixels = static_cast<int>(turnRemainder_);
        if (pixels) {
            input::MoveMouse(pixels, 0);
            turnRemainder_ -= static_cast<float>(pixels);
        }
        if (turn == 0.0f)
            turnRemainder_ = 0.0f;

        SetKey(interact_, pressed(rightGrip_), false);
    } else {
        // Les touches de la partie sont relâchées
        for (Key& key : move_)
            SetKey(key, false, false);
        SetKey(interact_, false, false);
        SetKey(jump_, false, false);
        SetKey(crouch_, false, false);
        turnRemainder_ = 0.0f;

        // Menus : le stick le plus incliné donne une flèche. Axe dominant ; la direction
        // courante tient jusqu'à ce que le stick revienne vers le centre.
        const bool useRight = rx * rx + ry * ry > lx * lx + ly * ly;
        const float x = useRight ? rx : lx, y = useRight ? ry : ly;
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
    }

    SetKey(enter_, pressed(confirm_), false);
    SetKey(escape_, pressed(back_), false);
}

} // namespace xr
