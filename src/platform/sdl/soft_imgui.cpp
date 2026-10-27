#include "platform/sdl/soft_imgui.hpp"
#include <algorithm>
#include <cmath>
#include "imgui.h"

void softImGuiInit(SoftTexture& tex) {
    ImGuiIO& io = ImGui::GetIO();
    unsigned char* pixels = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    tex.w = w;
    tex.h = h;
    tex.px.assign(reinterpret_cast<const uint32_t*>(pixels), reinterpret_cast<const uint32_t*>(pixels) + size_t(w) * size_t(h));
    io.Fonts->SetTexID(ImTextureID(&tex));
}

namespace {

// Melange "source par-dessus" d'une couleur RGBA (0..255) sur un pixel de destination
inline void blend(uint32_t& d, uint32_t r, uint32_t g, uint32_t b, uint32_t a, bool rgbOrder) {
    if (a == 0) return;
    uint32_t dr, dg, db;
    if (rgbOrder) { dr = (d >> 16) & 255; dg = (d >> 8) & 255; db = d & 255; }
    else          { db = (d >> 16) & 255; dg = (d >> 8) & 255; dr = d & 255; }
    if (a < 255) {
        r = (r * a + dr * (255 - a) + 127) / 255;
        g = (g * a + dg * (255 - a) + 127) / 255;
        b = (b * a + db * (255 - a) + 127) / 255;
    }
    d = 0xFF000000u | (rgbOrder ? ((r << 16) | (g << 8) | b) : ((b << 16) | (g << 8) | r));
}

struct V { float x, y, u, v; float r, g, b, a; };

// Triangle : remplissage par demi-plans dans la boite englobante, limitee au rectangle de decoupe.
// Les coordonnees de texture et la couleur sont interpolees ; la texture est lue au plus proche.
void drawTriangle(const V& a, const V& b, const V& c, const SoftTexture* tex, const SoftTarget& t,
                  int cx0, int cy0, int cx1, int cy1) {
    const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (std::fabs(area) < 1e-6f) return;
    const int x0 = std::max(cx0, int(std::floor(std::min({ a.x, b.x, c.x }))));
    const int x1 = std::min(cx1 - 1, int(std::ceil(std::max({ a.x, b.x, c.x }))));
    const int y0 = std::max(cy0, int(std::floor(std::min({ a.y, b.y, c.y }))));
    const int y1 = std::min(cy1 - 1, int(std::ceil(std::max({ a.y, b.y, c.y }))));
    if (x0 > x1 || y0 > y1) return;
    const float inv = 1.0f / area;
    // couleur constante (cas tres courant : rectangles, texte d'une seule couleur)
    const bool flatColor = a.r == b.r && a.r == c.r && a.g == b.g && a.g == c.g && a.b == b.b && a.b == c.b && a.a == b.a && a.a == c.a;
    for (int y = y0; y <= y1; y++) {
        const float py = float(y) + 0.5f;
        uint32_t* row = t.pixels + size_t(y) * size_t(t.pitch);
        for (int x = x0; x <= x1; x++) {
            const float px = float(x) + 0.5f;
            float w0 = ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px)) * inv;
            float w1 = ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px)) * inv;
            float w2 = 1.0f - w0 - w1;
            if (w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f) continue;
            float r = a.r, g = a.g, bl = a.b, al = a.a;
            if (!flatColor) {
                r = a.r * w0 + b.r * w1 + c.r * w2; g = a.g * w0 + b.g * w1 + c.g * w2;
                bl = a.b * w0 + b.b * w1 + c.b * w2; al = a.a * w0 + b.a * w1 + c.a * w2;
            }
            if (tex && !tex->px.empty()) {
                const float u = a.u * w0 + b.u * w1 + c.u * w2, v = a.v * w0 + b.v * w1 + c.v * w2;
                const int tx = std::clamp(int(u * float(tex->w)), 0, tex->w - 1);
                const int ty = std::clamp(int(v * float(tex->h)), 0, tex->h - 1);
                const uint32_t tp = tex->px[size_t(ty) * size_t(tex->w) + size_t(tx)];
                r *= float(tp & 255) / 255.0f; g *= float((tp >> 8) & 255) / 255.0f;
                bl *= float((tp >> 16) & 255) / 255.0f; al *= float(tp >> 24) / 255.0f;
            }
            blend(row[x], uint32_t(r + 0.5f), uint32_t(g + 0.5f), uint32_t(bl + 0.5f), uint32_t(al + 0.5f), t.rgbOrder);
        }
    }
}

}  // namespace

void softImGuiRender(const ImDrawData* data, const SoftTarget& t) {
    if (!data || !t.pixels) return;
    const ImVec2 off = data->DisplayPos;
    for (int n = 0; n < data->CmdListsCount; n++) {
        const ImDrawList* list = data->CmdLists[n];
        const ImDrawVert* vb = list->VtxBuffer.Data;
        const ImDrawIdx* ib = list->IdxBuffer.Data;
        for (const ImDrawCmd& cmd : list->CmdBuffer) {
            if (cmd.UserCallback) {
                if (cmd.UserCallback != ImDrawCallback_ResetRenderState) cmd.UserCallback(list, &cmd);
                continue;
            }
            const int cx0 = std::max(0, int(cmd.ClipRect.x - off.x)), cy0 = std::max(0, int(cmd.ClipRect.y - off.y));
            const int cx1 = std::min(t.width, int(std::ceil(cmd.ClipRect.z - off.x)));
            const int cy1 = std::min(t.height, int(std::ceil(cmd.ClipRect.w - off.y)));
            if (cx0 >= cx1 || cy0 >= cy1) continue;
            const SoftTexture* tex = reinterpret_cast<const SoftTexture*>(cmd.GetTexID());
            for (unsigned i = 0; i + 2 < cmd.ElemCount; i += 3) {
                V v[3];
                for (int k = 0; k < 3; k++) {
                    const ImDrawVert& s = vb[cmd.VtxOffset + ib[cmd.IdxOffset + i + unsigned(k)]];
                    v[k] = { s.pos.x - off.x, s.pos.y - off.y, s.uv.x, s.uv.y,
                             float(s.col & 255), float((s.col >> 8) & 255), float((s.col >> 16) & 255), float(s.col >> 24) };
                }
                drawTriangle(v[0], v[1], v[2], tex, t, cx0, cy0, cx1, cy1);
            }
        }
    }
}
