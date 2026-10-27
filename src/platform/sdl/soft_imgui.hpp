#pragma once
// Rendu logiciel de Dear ImGui : dessine les triangles d'une image ImGui directement dans une zone de pixels
// (la surface SDL de la fenetre). Sert la ou il n'y a pas de rendu graphique accelere simple (PS5).
#include <cstdint>
#include <vector>

struct ImDrawData;

// Une texture pour ImGui (icones des jeux, police) : pixels 0xAABBGGRR (octets R, G, B, A)
struct SoftTexture {
    int w = 0, h = 0;
    std::vector<uint32_t> px;
};

// Zone de destination. rgbOrder : vrai si les pixels sont 0x00RRGGBB (XRGB / ARGB), faux si 0x00BBGGRR (XBGR / ABGR).
struct SoftTarget {
    uint32_t* pixels = nullptr;
    int width = 0, height = 0, pitch = 0;     // pitch en pixels
    bool rgbOrder = true;
};

// Cree la texture de la police d'ImGui (a appeler apres avoir ajoute les polices)
void softImGuiInit(SoftTexture& fontTexture);
void softImGuiRender(const ImDrawData* data, const SoftTarget& target);
