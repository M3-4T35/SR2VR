#pragma once
// Manette virtuelle présentée au jeu.
//
// Juiced lit la manette par SDL3 (`SDL=1` dans juiced.ini, SDL3.dll chargée dans le processus
// du jeu) : à chaque image il prend la première manette que SDL connaît et recopie ses boutons
// et ses axes dans la manette du jeu. SDL sait créer une manette virtuelle : on en attache une
// à la même SDL3.dll, et le jeu la voit comme une manette Xbox branchée. Ce que la manette du
// jeu sait faire (roue des armes au stick, gâchettes analogiques...) devient ainsi accessible
// sans connaître les touches du clavier.
//
// Boutons et axes : numérotation des manettes de SDL (SDL_GamepadButton, SDL_GamepadAxis).

namespace input::vpad {

enum Button {
    kSouth = 0, // A
    kEast = 1,  // B
    kWest = 2,  // X
    kNorth = 3, // Y
    kBack = 4,
    kGuide = 5,
    kStart = 6,
    kLeftStick = 7,
    kRightStick = 8,
    kLeftShoulder = 9,
    kRightShoulder = 10,
    kDpadUp = 11,
    kDpadDown = 12,
    kDpadLeft = 13,
    kDpadRight = 14,
    kButtonCount = 15
};

enum Axis { kLeftX = 0, kLeftY = 1, kRightX = 2, kRightY = 3, kLeftTrigger = 4, kRightTrigger = 5, kAxisCount = 6 };

// Attache la manette virtuelle si ce n'est pas fait (sans effet tant que SDL3.dll ou son
// sous-système des manettes ne sont pas prêts : à rappeler de temps en temps). true : prête.
bool Attach();
bool Ready();

// État d'un bouton ; sans effet si la manette n'est pas prête ou si rien ne change
void SetButton(int button, bool down);
// Sticks : -1 à 1, x vers la droite, y vers le BAS (convention de SDL). Gâchettes : 0 à 1.
void SetAxis(int axis, float value);
// Tout relâché, sticks au centre
void Reset();

} // namespace input::vpad
