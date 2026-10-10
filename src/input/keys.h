#pragma once
// Clavier et souris envoyés au jeu. Saints Row 2 lit son clavier par Windows (messages,
// hook clavier, GetAsyncKeyState), pas par DirectInput : SendInput suffit, à condition que
// la fenêtre du jeu soit au premier plan. Rien n'est envoyé sinon, pour ne pas taper dans
// une autre application.
#include <windows.h>

namespace input {

// Fenêtre principale du jeu (cherchée une fois, puis gardée tant qu'elle existe)
HWND GameWindow();

// Appui (down) ou relâchement d'une touche (code VK_*). false si l'appui n'a pas été envoyé
// parce que le jeu n'est pas au premier plan (on tente alors de l'y remettre).
bool SendKey(WORD vk, bool down);

// Idem pour une touche désignée par sa position sur le clavier (code de balayage) : Windows
// en déduit la touche selon la disposition courante, exactement comme si l'utilisateur
// appuyait dessus. 0x11 est ainsi « Z » en AZERTY et « W » en QWERTY.
bool SendScan(WORD scanCode, bool down);

// Déplacement relatif de la souris, en pixels. false si le jeu n'est pas au premier plan.
bool MoveMouse(int dx, int dy);

// Nombre d'appuis envoyés depuis le dernier appel
unsigned TakeSentCount();

} // namespace input
