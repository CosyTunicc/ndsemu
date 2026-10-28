#!/usr/bin/env bash
# Compile ndsemu pour la PS5 (programme maison lance par le Homebrew Launcher de websrv).
#
# Il faut : Linux (ou WSL Ubuntu 24.04 sous Windows), clang-18 / lld-18, et le ps5-payload-sdk :
#   sudo apt-get install -y clang-18 lld-18 llvm-18 build-essential cmake git wget unzip python3
#   wget https://github.com/ps5-payload-dev/sdk/releases/latest/download/ps5-payload-sdk.zip
#   sudo unzip -d /opt ps5-payload-sdk.zip
#   export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
# Puis, depuis le dossier du projet :
#   ./tools/build_ps5.sh
# Resultat : build_ps5/ndsemu-ps5.zip, qui contient le dossier homebrew/ndsemu a copier sur la PS5.
set -e
if [[ -z "$PS5_PAYLOAD_SDK" ]]; then
    echo "erreur : PS5_PAYLOAD_SDK n'est pas defini (export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk)"
    exit 1
fi
RACINE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${PS5_PAYLOAD_SDK}/toolchain/prospero.sh"
SORTIE="$RACINE/build_ps5"
mkdir -p "$SORTIE/obj"

# 1. SDL2 pour PS5 (image, son, manette), installe dans le SDK une fois pour toutes
if [[ ! -f "$PS5_SYSROOT$PS5_HBROOT/lib/libSDL2.a" ]]; then
    echo "=== SDL2 pour PS5 (premiere fois seulement) ==="
    rm -rf "$SORTIE/SDL"
    git clone --depth 1 https://github.com/ps5-payload-dev/SDL "$SORTIE/SDL"
    "$CMAKE" -DCMAKE_BUILD_TYPE=Release -DSDL_OPENGL=OFF -DSDL_LOADSO=YES -DSDL_SHARED=OFF -DSDL_STATIC=ON \
        -DSDL_LIBSAMPLERATE=OFF -B "$SORTIE/SDL/build" -S "$SORTIE/SDL"
    make -C "$SORTIE/SDL/build" -j"$(nproc)"
    make -C "$SORTIE/SDL/build" install
fi

# 2. Polices incluses dans le programme
python3 "$RACINE/tools/embed_fonts.py"

# 3. Compilation
SRC="$RACINE/src"
IM="$RACINE/third_party/imgui"
FICHIERS=(
    "$SRC"/core/*.cpp
    "$SRC/ui/library.cpp"
    "$SRC/platform/sdl/main_sdl.cpp"
    "$SRC/platform/sdl/soft_imgui.cpp"
    "$IM/imgui.cpp" "$IM/imgui_draw.cpp" "$IM/imgui_tables.cpp" "$IM/imgui_widgets.cpp"
)
SDL_CFLAGS="$($PKG_CONFIG --cflags sdl2)"
SDL_LIBS="$($PKG_CONFIG --libs sdl2 | sed 's/-lsamplerate//')"     # inutile ici (desactive dans SDL)
# -march=znver2 : processeur de la PS5 (Zen 2, AVX2)
OBJETS=()
for f in "${FICHIERS[@]}"; do
    o="$SORTIE/obj/$(basename "${f%.cpp}").o"
    echo "  $(basename "$f")"
    "$CXX" -std=c++17 -O3 -march=znver2 -Wall -I"$SRC" -I"$IM" $SDL_CFLAGS -c "$f" -o "$o" &
    OBJETS+=("$o")
    while (( $(jobs -r | wc -l) >= $(nproc) )); do wait -n; done
done
wait
"$CXX" -o "$SORTIE/eboot.elf" "${OBJETS[@]}" $SDL_LIBS
echo "=== eboot.elf : $(du -h "$SORTIE/eboot.elf" | cut -f1) ==="

# 4. Dossier pour le Homebrew Launcher : homebrew/ndsemu/eboot.elf + sce_sys/icon0.png
DIST="$SORTIE/dist/homebrew/ndsemu"
rm -rf "$SORTIE/dist"
mkdir -p "$DIST/sce_sys" "$DIST/roms"
cp "$SORTIE/eboot.elf" "$DIST/"
cp "$RACINE/assets/ps5/icon0.png" "$DIST/sce_sys/"
cp "$RACINE/assets/ps5/LISEZMOI_PS5.txt" "$DIST/"
cp "$RACINE/LICENCES.txt" "$DIST/"
(cd "$SORTIE/dist" && rm -f ../ndsemu-ps5.zip && zip -qr ../ndsemu-ps5.zip homebrew)
echo "=== pret : $SORTIE/ndsemu-ps5.zip ==="
