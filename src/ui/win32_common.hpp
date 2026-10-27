#pragma once
// Petits services Windows partages par la bibliotheque et la fenetre de jeu (implementes dans library_win32.cpp).
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#include "ui/library_ui.hpp"

HICON makeAppIcon(int size);                 // icone de ndsemu (carre orange, deux ecrans)
UiFontFiles systemUiFonts();                 // Segoe UI / Consolas (ou Arial / Courier New)
std::wstring exeDirectory();                 // dossier du programme
UINT windowDpi(HWND hwnd);                   // ppp de la fenetre (96 = 100 %)
void enablePerMonitorDpi();                  // nettete sur les ecrans a 125 %, 150 %...
std::string utf8FromWide(const std::wstring& w);
std::wstring wideFromUtf8(const std::string& s);
#endif
