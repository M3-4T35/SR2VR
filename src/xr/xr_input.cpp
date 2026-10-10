#include "xr/xr_input.h"
#include "input/actions.h"
#include "input/keys.h"
#include "input/vpad.h"
#include "util/config.h"
#include "util/log.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
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
// Rotation par crans : un cran quand le stick dépasse 0,6, réarmé quand il repasse sous 0,3
constexpr float kSnapEngage = 0.6f;
constexpr float kSnapRelease = 0.3f;
constexpr float kSnapToleranceDegrees = 1.5f; // écart accepté sans rattrapage
constexpr int kSnapMaxBursts = 5;
constexpr int kSnapMaxBurstPixels = 3000;
constexpr ULONGLONG kSnapTimeoutMs = 1500;
// Après un envoi : la caméra du jeu est relue au plus tôt après ce délai et ce nombre d'images
constexpr ULONGLONG kSnapSettleMs = 150;
constexpr unsigned kSnapSettleFrames = 3;
// Rapport encore inconnu : le premier cran commence par un essai de cette part de l'angle
constexpr float kSnapProbeShare = 0.3f;
// Rapport de départ, avant toute mesure : la vitesse de rotation continue réglée par le joueur
// fait à peu près 150 degrés par seconde
constexpr float kSnapGuessDegreesPerSecond = 150.0f;

float WrapDegrees(float a)
{
    while (a > 180.0f)
        a -= 360.0f;
    while (a < -180.0f)
        a += 360.0f;
    return a;
}
// Sans image de jeu préparée depuis ce délai, on est dans un menu
constexpr ULONGLONG kGameplayTimeoutMs = 300;

// Positions des touches (codes de balayage) : W, S, A, D et E d'un clavier QWERTY
constexpr WORD kScanForward = 0x11, kScanBack = 0x1F, kScanLeft = 0x1E, kScanRight = 0x20, kScanInteract = 0x12;
// Touches par défaut du jeu : Espace pour sauter, C pour s'accroupir (bascule)
constexpr int kScanJump = 0x39, kScanCrouch = 0x2E;

std::atomic<ULONGLONG> g_gameplayTick{0};

// Pose de visée de chaque manette (0 gauche, 1 droite), publiée par le thread XR : orientation
// dans l'espace LOCAL et position par rapport à la tête
std::mutex g_aimMutex;
float g_aimQuat[2][4]{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f, 1.0f}};
float g_aimOffset[2][3]{};
ULONGLONG g_aimTick[2]{};
constexpr ULONGLONG kAimMaxAgeMs = 300;

std::atomic<unsigned> g_viewCycleRequests{0};

// Position des manettes par rapport à la tête, publiée par le thread XR
std::mutex g_handMutex;
float g_handOffset[2][3]{};
ULONGLONG g_handTick[2]{};

void PublishHand(int hand, const XrVector3f* position, const XrPosef* head)
{
    std::lock_guard lock(g_handMutex);
    if (position && head) {
        g_handOffset[hand][0] = position->x - head->position.x;
        g_handOffset[hand][1] = position->y - head->position.y;
        g_handOffset[hand][2] = position->z - head->position.z;
        g_handTick[hand] = GetTickCount64();
    } else {
        g_handTick[hand] = 0;
    }
}

// Roue des armes : une arme est désignée au-delà de 0,6 ; stick revenu sous 0,25, elle est prise
constexpr float kWheelPoint = 0.6f;
constexpr float kWheelRelease = 0.25f;
// Après le relâchement du bouton, le stick reste pointé le temps que le jeu prenne l'arme
constexpr ULONGLONG kWheelSettleMs = 200;

void PublishAim(int hand, const XrPosef* pose, const XrPosef* head)
{
    std::lock_guard lock(g_aimMutex);
    if (pose && head) {
        g_aimQuat[hand][0] = pose->orientation.x;
        g_aimQuat[hand][1] = pose->orientation.y;
        g_aimQuat[hand][2] = pose->orientation.z;
        g_aimQuat[hand][3] = pose->orientation.w;
        g_aimOffset[hand][0] = pose->position.x - head->position.x;
        g_aimOffset[hand][1] = pose->position.y - head->position.y;
        g_aimOffset[hand][2] = pose->position.z - head->position.z;
        g_aimTick[hand] = GetTickCount64();
    } else {
        g_aimTick[hand] = 0;
    }
}

XrPath ToPath(XrInstance instance, const char* text)
{
    XrPath path = XR_NULL_PATH;
    xrStringToPath(instance, text, &path);
    return path;
}

// Actions, dans l'ordre du tableau `actions` de Init
enum {
    kLeftStick,
    kRightStick,
    kConfirm,
    kBack,
    kLeftTrigger,
    kRightTrigger,
    kRightGrip,
    kAimPose,
    kLeftClick,
    kRightClick,
    kLeftGrip,
    kReload,
    kLeftGripPose,
    kRightGripPose,
    kActionCount
};

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
      {kRightGrip, "/user/hand/right/input/squeeze/value"},
      {kAimPose, "/user/hand/right/input/aim/pose"},
      {kLeftClick, "/user/hand/left/input/thumbstick/click"},
      {kRightClick, "/user/hand/right/input/thumbstick/click"},
      {kLeftGrip, "/user/hand/left/input/squeeze/value"},
      {kReload, "/user/hand/left/input/x/click"}}},
    {"/interaction_profiles/valve/index_controller",
     {{kLeftStick, "/user/hand/left/input/thumbstick"},
      {kRightStick, "/user/hand/right/input/thumbstick"},
      {kConfirm, "/user/hand/right/input/a/click"},
      {kBack, "/user/hand/right/input/b/click"},
      {kBack, "/user/hand/left/input/b/click"},
      {kLeftTrigger, "/user/hand/left/input/trigger/value"},
      {kRightTrigger, "/user/hand/right/input/trigger/value"},
      {kRightGrip, "/user/hand/right/input/squeeze/value"},
      {kAimPose, "/user/hand/right/input/aim/pose"},
      {kLeftClick, "/user/hand/left/input/thumbstick/click"},
      {kRightClick, "/user/hand/right/input/thumbstick/click"},
      {kLeftGrip, "/user/hand/left/input/squeeze/value"},
      {kReload, "/user/hand/left/input/a/click"}}},
    {"/interaction_profiles/microsoft/motion_controller",
     {{kLeftStick, "/user/hand/left/input/thumbstick"},
      {kRightStick, "/user/hand/right/input/thumbstick"},
      {kConfirm, "/user/hand/right/input/trackpad/click"},
      {kBack, "/user/hand/left/input/menu/click"},
      {kBack, "/user/hand/right/input/menu/click"},
      {kLeftTrigger, "/user/hand/left/input/trigger/value"},
      {kRightTrigger, "/user/hand/right/input/trigger/value"},
      {kRightGrip, "/user/hand/right/input/squeeze/click"},
      {kAimPose, "/user/hand/right/input/aim/pose"},
      {kLeftClick, "/user/hand/left/input/thumbstick/click"},
      {kRightClick, "/user/hand/right/input/thumbstick/click"},
      {kLeftGrip, "/user/hand/left/input/squeeze/click"}}},
    {"/interaction_profiles/htc/vive_controller",
     {{kLeftStick, "/user/hand/left/input/trackpad"},
      {kRightStick, "/user/hand/right/input/trackpad"},
      {kConfirm, "/user/hand/right/input/trackpad/click"},
      {kBack, "/user/hand/left/input/menu/click"},
      {kBack, "/user/hand/right/input/menu/click"},
      {kLeftTrigger, "/user/hand/left/input/trigger/value"},
      {kRightTrigger, "/user/hand/right/input/trigger/value"},
      {kRightGrip, "/user/hand/right/input/squeeze/click"},
      {kAimPose, "/user/hand/right/input/aim/pose"},
      {kLeftGrip, "/user/hand/left/input/squeeze/click"}}},
    {"/interaction_profiles/khr/simple_controller",
     {{kConfirm, "/user/hand/left/input/select/click"},
      {kConfirm, "/user/hand/right/input/select/click"},
      {kBack, "/user/hand/left/input/menu/click"},
      {kBack, "/user/hand/right/input/menu/click"},
      {kAimPose, "/user/hand/right/input/aim/pose"}}},
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

bool GetHandOffset(int hand, float offset[3])
{
    if (hand < 0 || hand > 1)
        return false;
    std::lock_guard lock(g_handMutex);
    if (!g_handTick[hand] || GetTickCount64() - g_handTick[hand] > kAimMaxAgeMs)
        return false;
    memcpy(offset, g_handOffset[hand], sizeof(g_handOffset[hand]));
    return true;
}

unsigned TakeViewCycleRequests()
{
    return g_viewCycleRequests.exchange(0);
}

bool GetAimRay(int hand, float quat[4], float offset[3])
{
    if (hand < 0 || hand > 1)
        return false;
    std::lock_guard lock(g_aimMutex);
    if (!g_aimTick[hand] || GetTickCount64() - g_aimTick[hand] > kAimMaxAgeMs)
        return false;
    memcpy(quat, g_aimQuat[hand], sizeof(g_aimQuat[hand]));
    memcpy(offset, g_aimOffset[hand], sizeof(g_aimOffset[hand]));
    return true;
}

void MenuInput::ReadConfig()
{
    turnSpeed_ = config::Float("Controls", "turn_speed", 1200.0f);
    if (!(turnSpeed_ >= -20000.0f && turnSpeed_ <= 20000.0f))
        turnSpeed_ = 1200.0f;
    turnMode_ = config::Int("Controls", "turn_mode", 0) == 1 ? 1 : 0;
    snapAngle_ = config::Float("Controls", "snap_angle", 30.0f);
    if (!(snapAngle_ >= 5.0f && snapAngle_ <= 180.0f))
        snapAngle_ = 30.0f;
    // Rapport pixels / degré : celui appris lors d'une partie précédente, sinon une supposition
    if (!snapLoaded_) {
        snapLoaded_ = true;
        const float saved = config::Float("Learned", "turn_pixels_per_degree", 0.0f);
        if (std::fabs(saved) > 0.05f && std::fabs(saved) < 1000.0f) {
            pixelsPerDegree_ = snapSaved_ = saved;
            snapLearned_ = true;
        }
    }
    if (!snapLearned_ && !snap_.active)
        pixelsPerDegree_ = turnSpeed_ / kSnapGuessDegreesPerSecond;
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
    wheelButton_ = config::Int("Controls", "wheel_button", input::vpad::kEast);
    if (wheelButton_ < 0 || wheelButton_ >= input::vpad::kButtonCount)
        wheelButton_ = input::vpad::kEast;
    wheelToggle_ = config::Int("Controls", "wheel_toggle", 1) != 0;
    fireMode_ = config::Int("Controls", "fire_mode", 0);
    if (fireMode_ < 0 || fireMode_ > 2)
        fireMode_ = 0;
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
        !CreateAction(set_, XR_ACTION_TYPE_BOOLEAN_INPUT, "right_grip", "Interagir (E)", &rightGrip_) ||
        !CreateAction(set_, XR_ACTION_TYPE_POSE_INPUT, "aim_pose", "Viser (main droite)", &aimPose_) ||
        !CreateAction(set_, XR_ACTION_TYPE_BOOLEAN_INPUT, "left_click", "Changer de vue", &leftClick_) ||
        !CreateAction(set_, XR_ACTION_TYPE_BOOLEAN_INPUT, "right_click", "Roue des armes", &rightClick_) ||
        !CreateAction(set_, XR_ACTION_TYPE_BOOLEAN_INPUT, "left_grip", "Courir", &leftGrip_) ||
        !CreateAction(set_, XR_ACTION_TYPE_BOOLEAN_INPUT, "reload", "Recharger", &reload_) ||
        !CreateAction(set_, XR_ACTION_TYPE_POSE_INPUT, "left_aim_pose", "Viser (main gauche)", &leftAimPose_) ||
        !CreateAction(set_, XR_ACTION_TYPE_POSE_INPUT, "left_grip_pose", "Main gauche", &gripPose_[0]) ||
        !CreateAction(set_, XR_ACTION_TYPE_POSE_INPUT, "right_grip_pose", "Main droite", &gripPose_[1])) {
        LOG_XR("ERREUR : actions des manettes non creees, manettes desactivees");
        return false;
    }

    const XrAction actions[kActionCount] = {leftStick_, rightStick_, confirm_, back_, leftTrigger_, rightTrigger_,
                                            rightGrip_, aimPose_, leftClick_, rightClick_, leftGrip_, reload_,
                                            gripPose_[0], gripPose_[1]};
    int accepted = 0;
    for (const Profile& profile : kProfiles) {
        std::vector<XrActionSuggestedBinding> suggested;
        for (const Binding& b : profile.bindings)
            suggested.push_back({actions[b.action], ToPath(instance, b.path)});
        // Poses de prise des deux mains et pose de visée de la gauche : tous les profils les ont
        suggested.push_back({leftAimPose_, ToPath(instance, "/user/hand/left/input/aim/pose")});
        suggested.push_back({gripPose_[0], ToPath(instance, "/user/hand/left/input/grip/pose")});
        suggested.push_back({gripPose_[1], ToPath(instance, "/user/hand/right/input/grip/pose")});
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
    // Espaces des poses de visée : situent le rayon de chaque manette à chaque image
    XrActionSpaceCreateInfo sci{XR_TYPE_ACTION_SPACE_CREATE_INFO};
    sci.poseInActionSpace.orientation.w = 1.0f;
    for (int hand = 0; hand < 2; ++hand) {
        sci.action = hand ? aimPose_ : leftAimPose_;
        if (XR_FAILED(xrCreateActionSpace(session, &sci, &aimSpace_[hand]))) {
            aimSpace_[hand] = XR_NULL_HANDLE;
            LOG_XR("manettes : espace de visee de la main %s non cree, pas de visee avec cette main",
                   hand ? "droite" : "gauche");
        }
    }
    for (int hand = 0; hand < 2; ++hand) {
        sci.action = gripPose_[hand];
        if (XR_FAILED(xrCreateActionSpace(session, &sci, &gripSpace_[hand]))) {
            gripSpace_[hand] = XR_NULL_HANDLE;
            LOG_XR("manettes : espace de la main %s non cree", hand ? "droite" : "gauche");
        }
    }
    ready_ = true;
    LOG_XR("manettes : %d profil(s) acceptes. Partie : stick gauche = deplacement, stick droit = tourner (%s, %.0f px/s), "
           "haut = sauter (position 0x%02X), bas = s'accroupir (position 0x%02X), poignee droite = E, gachette droite "
           "= tir, gachette gauche = attaque secondaire (tir avec deux armes), poignee gauche = courir, X = recharger "
           "(tir : fire_mode=%d). Menus : sticks = fleches, gachettes = Ctrl / Maj. A = Entree, B ou menu gauche = Echap",
           accepted, turnMode_ == 1 ? "par crans" : "en continu", turnSpeed_, jump_.code, crouch_.code, fireMode_);
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

void MenuInput::SendSnapBurst(float degrees, float yaw, unsigned sequence)
{
    int pixels = static_cast<int>(std::lround(degrees * pixelsPerDegree_));
    if (pixels > kSnapMaxBurstPixels)
        pixels = kSnapMaxBurstPixels;
    if (pixels < -kSnapMaxBurstPixels)
        pixels = -kSnapMaxBurstPixels;
    snap_.burstYaw = snap_.lastYaw = yaw;
    snap_.burstSequence = snap_.lastSequence = sequence;
    snap_.burstPixels = pixels;
    snap_.burstDegrees = degrees;
    snap_.burstTick = GetTickCount64();
    if (!pixels || !input::MoveMouse(pixels, 0)) {
        // Jeu pas au premier plan (rien n'est envoyé ailleurs) : cran abandonné
        snap_.burstPixels = 0;
        snap_.active = false;
        return;
    }
    snap_.pixels += pixels;
    ++snap_.bursts;
}

void MenuInput::FinishSnap(const char* why, float yaw, ULONGLONG now)
{
    snap_.active = false;
    ++snapCount_;
    // Rapport appris gardé pour la prochaine partie (le premier cran sera juste d'emblée)
    if (snapLearned_ && std::fabs(pixelsPerDegree_ - snapSaved_) > 0.03f * std::fabs(pixelsPerDegree_) && config::Path()[0]) {
        char text[32];
        snprintf(text, sizeof(text), "%.3f", pixelsPerDegree_);
        if (WritePrivateProfileStringA("Learned", "turn_pixels_per_degree", text, config::Path()))
            snapSaved_ = pixelsPerDegree_;
    }
    // Les premiers crans, puis un sur vingt : de quoi juger la rotation sans casque
    if (snapCount_ > 12 && snapCount_ % 20)
        return;
    if (snap_.closed)
        LOG_XR("rotation par crans n%u : %.0f degres voulus, %.1f tournes en %d envoi(s) (%d px, %llu ms), %.2f px/degre%s%s",
               snapCount_, snap_.target, WrapDegrees(yaw - snap_.startYaw), snap_.bursts, snap_.pixels,
               now - snap_.startTick, pixelsPerDegree_, why[0] ? " - " : "", why);
    else
        LOG_XR("rotation par crans n%u : %.0f degres voulus, %d px envoyes sans mesure du jeu (%.2f px/degre suppose)%s%s",
               snapCount_, snap_.target, snap_.pixels, pixelsPerDegree_, why[0] ? " - " : "", why);
}

void MenuInput::UpdateSnap(float rx, float ry, bool blocked, ULONGLONG now)
{
    const float ax = std::fabs(rx);
    if (ax < kSnapRelease)
        snapArmed_ = true;

    float yaw = 0.0f;
    unsigned sequence = 0;
    bool steady = false;
    const bool measured = input::actions::GameYaw(&yaw, &sequence, &steady);

    if (!snap_.active) {
        if (!snapArmed_ || blocked || ax <= kSnapEngage || ax <= std::fabs(ry))
            return;
        snapArmed_ = false;
        snap_ = Snap{};
        snap_.active = true;
        snap_.closed = measured && steady;
        snap_.target = rx > 0.0f ? snapAngle_ : -snapAngle_;
        snap_.startYaw = yaw;
        snap_.startTick = now;
        // Rapport encore supposé et jeu mesurable : un essai d'abord, le reste une fois mesuré
        SendSnapBurst(snap_.closed && !snapLearned_ ? snap_.target * kSnapProbeShare : snap_.target, yaw, sequence);
        // Sans mesure (véhicule : la caméra suit aussi la route) : un seul envoi, au rapport connu
        if (snap_.active && !snap_.closed)
            FinishSnap("", yaw, now);
        return;
    }

    if (!measured || !steady) {
        FinishSnap("mesure perdue", snap_.lastYaw, now);
        return;
    }
    if (now - snap_.startTick > kSnapTimeoutMs) {
        FinishSnap("delai depasse", yaw, now);
        return;
    }
    if (sequence == snap_.lastSequence)
        return; // pas de nouvelle image de simulation
    const float moved = std::fabs(WrapDegrees(yaw - snap_.lastYaw));
    snap_.lastYaw = yaw;
    snap_.lastSequence = sequence;
    // La caméra a-t-elle fini de tourner ? Quelques images après l'envoi (le jeu lit la souris à
    // l'image suivante), puis immobile (ou dix images : elle tourne peut-être pour une autre raison)
    const unsigned since = sequence - snap_.burstSequence;
    const ULONGLONG waited = now - snap_.burstTick;
    if (since < kSnapSettleFrames || waited < kSnapSettleMs)
        return;
    const float turned = WrapDegrees(yaw - snap_.burstYaw);
    if (std::fabs(turned) < 0.5f) {
        if (since < 8 || waited < 400)
            return;
        // Rapport encore supposé : l'essai était peut-être trop petit pour ce jeu, on le triple
        if (!snapLearned_ && snap_.bursts < 3) {
            pixelsPerDegree_ *= 3.0f;
            SendSnapBurst(snap_.burstDegrees, yaw, sequence);
            if (snap_.active)
                return;
        }
        // Le jeu n'a pas réagi à la souris (pause, fenêtre sans la souris) : inutile d'insister
        FinishSnap("le jeu n'a pas tourne", yaw, now);
        return;
    }
    if (moved > 0.4f && since < 10)
        return;
    // Rapport mesuré sur cet envoi (signe compris : un jeu réglé en souris inversée s'apprend
    // aussi). Seuls comptent les grands envois : les petits rattrapages mesurent mal.
    if (std::fabs(turned) > 1.5f && (!snapLearned_ || std::fabs(snap_.burstDegrees) >= 0.4f * snapAngle_)) {
        const float ratio = static_cast<float>(snap_.burstPixels) / turned;
        if (std::fabs(ratio) > 0.05f && std::fabs(ratio) < 1000.0f) {
            pixelsPerDegree_ = snapLearned_ && ratio * pixelsPerDegree_ > 0.0f ? 0.5f * (pixelsPerDegree_ + ratio) : ratio;
            snapLearned_ = true;
        }
    }
    const float remaining = snap_.target - WrapDegrees(yaw - snap_.startYaw);
    if (std::fabs(remaining) <= kSnapToleranceDegrees) {
        FinishSnap("", yaw, now);
    } else if (snap_.bursts >= kSnapMaxBursts) {
        FinishSnap("trop d'envois", yaw, now);
    } else {
        SendSnapBurst(remaining, yaw, sequence);
        if (!snap_.active)
            FinishSnap("jeu pas au premier plan", yaw, now);
    }
}

void MenuInput::SetMouseButton(int button, bool pressed)
{
    if (pressed == mouseHeld_[button])
        return;
    // Un relâchement part toujours ; un appui seulement si le jeu est au premier plan
    if (input::SendMouseButton(button, pressed) || !pressed)
        mouseHeld_[button] = pressed;
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
    snap_.active = false;
    input::actions::ReleaseAll();
    SetMouseButton(0, false);
    SetMouseButton(1, false);
    PublishAim(0, nullptr, nullptr);
    PublishAim(1, nullptr, nullptr);
    aimPoseValid_[0] = aimPoseValid_[1] = false;
    PublishHand(0, nullptr, nullptr);
    PublishHand(1, nullptr, nullptr);
    wheelOpen_ = false;
    wheelCloseTick_ = 0;
    input::vpad::Reset();
}

// Roue des armes. Sur la manette du jeu, elle s'ouvre tant qu'un bouton est tenu (B par défaut)
// et le stick gauche désigne l'arme, prise au relâchement du bouton. Ici : un clic du stick
// droit l'ouvre, le stick droit désigne, et le laisser revenir au centre prend l'arme (ou un
// second clic). Avec [Controls] wheel_toggle=0, la roue reste ouverte tant que le clic est tenu.
void MenuInput::UpdateWheel(bool gameplay, bool click, float rx, float ry, ULONGLONG now)
{
    namespace vpad = input::vpad;
    const bool pressed = click && !rightClickHeld_;
    rightClickHeld_ = click;
    if (!vpad::Ready()) {
        wheelOpen_ = false;
        return;
    }

    const float magnitude = std::sqrt(rx * rx + ry * ry);
    bool close = false;
    if (!wheelOpen_) {
        if (gameplay && !wheelCloseTick_ && (wheelToggle_ ? pressed : click)) {
            wheelOpen_ = true;
            wheelPointed_ = false;
            wheelDir_[0] = wheelDir_[1] = 0.0f;
            LOG_XR("manettes : roue des armes ouverte (bouton %d de la manette virtuelle)", wheelButton_);
        }
    } else if (!gameplay) {
        close = true; // pause, menu : on lâche tout
    } else if (wheelToggle_) {
        close = pressed || (wheelPointed_ && magnitude < kWheelRelease);
    } else {
        close = !click;
    }

    if (wheelOpen_ && !close) {
        if (magnitude > kWheelPoint) {
            wheelPointed_ = true;
            wheelDir_[0] = rx / magnitude;
            wheelDir_[1] = ry / magnitude;
        }
        vpad::SetButton(wheelButton_, true);
        // Stick droit -> stick gauche de la manette ; y vers le bas pour SDL. Tant qu'aucune
        // arme n'est désignée, le stick suit la main ; ensuite il garde la dernière direction.
        const float x = magnitude > kWheelRelease ? rx : wheelDir_[0];
        const float y = magnitude > kWheelRelease ? ry : wheelDir_[1];
        vpad::SetAxis(vpad::kLeftX, x);
        vpad::SetAxis(vpad::kLeftY, -y);
    } else if (wheelOpen_) {
        // Fermeture : bouton relâché, stick encore pointé un instant
        wheelOpen_ = false;
        wheelWaitCenter_ = true;
        wheelCloseTick_ = now + kWheelSettleMs;
        vpad::SetButton(wheelButton_, false);
        vpad::SetAxis(vpad::kLeftX, wheelDir_[0]);
        vpad::SetAxis(vpad::kLeftY, -wheelDir_[1]);
        LOG_XR("manettes : roue des armes fermee%s", wheelPointed_ ? "" : " sans choix");
    } else if (wheelCloseTick_ && now >= wheelCloseTick_) {
        wheelCloseTick_ = 0;
        vpad::SetAxis(vpad::kLeftX, 0.0f);
        vpad::SetAxis(vpad::kLeftY, 0.0f);
    }
}

void MenuInput::Update(XrSession session, bool focused, XrSpace base, XrTime time, const XrPosef* head)
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
    // Manette virtuelle : attachée dès que la SDL du jeu est prête
    if (!input::vpad::Ready() && now - lastAttach_ >= 2000) {
        lastAttach_ = now;
        input::vpad::Attach();
    }

    XrActiveActionSet active{set_, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    if (xrSyncActions(session, &sync) != XR_SUCCESS) { // XR_SESSION_NOT_FOCUSED compris
        ReleaseAll();
        return;
    }

    // Les deux mains : d'où part leur rayon de visée et où il pointe, à l'instant d'affichage ;
    // position de la prise, rapportée à la tête
    for (int hand = 0; hand < 2; ++hand) {
        XrSpaceLocation aim{XR_TYPE_SPACE_LOCATION};
        const XrSpaceLocationFlags aimFlags =
            XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT |
            XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
        const bool aimed = head && aimSpace_[hand] != XR_NULL_HANDLE && base != XR_NULL_HANDLE &&
                           XR_SUCCEEDED(xrLocateSpace(aimSpace_[hand], base, time, &aim)) &&
                           (aim.locationFlags & aimFlags) == aimFlags;
        PublishAim(hand, aimed ? &aim.pose : nullptr, head);
        aimPoseValid_[hand] = aimed;
        if (aimed)
            aimPoseLocal_[hand] = aim.pose;

        XrSpaceLocation grip{XR_TYPE_SPACE_LOCATION};
        const XrSpaceLocationFlags gripFlags =
            XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
        const bool held = head && gripSpace_[hand] != XR_NULL_HANDLE && base != XR_NULL_HANDLE &&
                          XR_SUCCEEDED(xrLocateSpace(gripSpace_[hand], base, time, &grip)) &&
                          (grip.locationFlags & gripFlags) == gripFlags;
        PublishHand(hand, held ? &grip.pose.position : nullptr, head);
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

    float lx, ly, rx, ry;
    stick(leftStick_, &lx, &ly);
    stick(rightStick_, &rx, &ry);

    const ULONGLONG gameplayTick = g_gameplayTick;
    const bool gameplay = gameplayTick && now >= gameplayTick && now - gameplayTick < kGameplayTimeoutMs;
    if (static_cast<int>(gameplay) != gameplayShown_) {
        LOG_XR("manettes : mode %s", gameplay ? "partie (deplacement, tourner, tir, interagir)" : "menus (fleches)");
        gameplayShown_ = gameplay;
    }

    // Gâchettes. Dans les menus : Ctrl et Maj (onglets) ; ces modificateurs passent avant les
    // flèches qu'ils accompagnent. En partie : la droite tire, la gauche fait l'attaque
    // secondaire, ou tire elle aussi quand le personnage tient une arme dans chaque main. Le tir
    // est posé directement dans les commandes du jeu par le moteur ([Controls] fire_mode=0) ;
    // à défaut, boutons de la souris (1) ou gâchettes de la manette virtuelle (2). Maj (courir)
    // est sur la poignée gauche.
    namespace actions = input::actions;
    const bool leftTrigger = pressed(leftTrigger_), rightTrigger = pressed(rightTrigger_);
    const bool fireLeft = gameplay && leftTrigger && !wheelOpen_;
    const bool fireRight = gameplay && rightTrigger && !wheelOpen_;
    int fireMode = fireMode_;
    if (fireMode == 0 && !actions::Direct())
        fireMode = input::vpad::Ready() ? 2 : 1;
    if (fireMode == 2 && !input::vpad::Ready())
        fireMode = 1;
    const bool dual = actions::DualWield();
    const bool primary = fireRight || (fireLeft && dual), secondary = fireLeft && !dual;
    actions::Set(actions::kTriggerLeft, fireMode == 0 && fireLeft);
    actions::Set(actions::kTriggerRight, fireMode == 0 && fireRight);
    SetMouseButton(0, fireMode == 1 && primary);
    SetMouseButton(1, fireMode == 1 && secondary);
    input::vpad::SetAxis(input::vpad::kRightTrigger, fireMode == 2 && primary ? 1.0f : 0.0f);
    input::vpad::SetAxis(input::vpad::kLeftTrigger, fireMode == 2 && secondary ? 1.0f : 0.0f);
    SetKey(control_, !gameplay && leftTrigger, false);
    SetKey(shift_, gameplay ? pressed(leftGrip_) : rightTrigger, false);
    const bool reload = gameplay && pressed(reload_);
    actions::Set(actions::kReload, fireMode == 0 && reload);
    input::vpad::SetButton(input::vpad::kWest, fireMode != 0 && reload);
    if ((fireLeft || fireRight) && !firedLogged_) {
        static const char* const kModes[] = {"commande posee dans le jeu", "bouton de souris", "manette virtuelle"};
        LOG_XR("manettes : premier tir a la gachette %s (%s)", fireRight ? "droite" : "gauche", kModes[fireMode]);
        firedLogged_ = true;
    }

    // Clic du stick gauche : vue suivante (en partie seulement)
    const bool leftClick = pressed(leftClick_);
    if (leftClick && !leftClickHeld_ && gameplay)
        ++g_viewCycleRequests;
    leftClickHeld_ = leftClick;

    // Roue des armes : tant qu'elle est ouverte, les sticks ne servent qu'à elle
    UpdateWheel(gameplay, pressed(rightClick_), rx, ry, now);
    if (wheelOpen_) {
        lx = ly = rx = ry = 0.0f;
    } else if (wheelWaitCenter_) {
        // La roue vient de se fermer : le stick droit ne fait ni tourner ni sauter avant d'être
        // revenu au centre
        if (rx * rx + ry * ry < kWheelRelease * kWheelRelease)
            wheelWaitCenter_ = false;
        else
            rx = ry = 0.0f;
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
        if (turnMode_ == 1)
            UpdateSnap(rx, ry, jump_.held || crouch_.held, now);
        else if (!jump_.held && !crouch_.held && std::fabs(rx) > kTurnDeadzone)
            turn = (rx - (rx > 0.0f ? kTurnDeadzone : -kTurnDeadzone)) / (1.0f - kTurnDeadzone);
        if (turnMode_ != 1)
            snap_.active = false;
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
        snap_.active = false;
        snapArmed_ = false; // un stick incliné dans un menu ne fait pas tourner au retour en partie

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
