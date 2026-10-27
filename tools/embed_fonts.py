#!/usr/bin/env python3
# Transforme les polices de assets/fonts en tableaux C++ (src/platform/sdl/fonts_embedded.inc) : la version
# SDL / PS5 n'a pas de polices systeme, elles sont donc incluses dans le programme.
# Polices : DejaVu Sans (licence libre Bitstream Vera, voir LICENCES.txt), reduites aux caracteres latins.
import os
racine = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sortie = os.path.join(racine, "src", "platform", "sdl", "fonts_embedded.inc")
with open(sortie, "w") as out:
    out.write("// Fichier genere par tools/embed_fonts.py : ne pas modifier a la main.\n")
    for nom, var in (("DejaVuSans-latin.ttf", "kFontRegular"), ("DejaVuSans-Bold-latin.ttf", "kFontBold")):
        data = open(os.path.join(racine, "assets", "fonts", nom), "rb").read()
        out.write("static const unsigned char %s[%d] = {\n" % (var, len(data)))
        for i in range(0, len(data), 24):
            out.write("    " + ", ".join("0x%02X" % b for b in data[i:i + 24]) + ",\n")
        out.write("};\n")
print("ecrit", sortie)
