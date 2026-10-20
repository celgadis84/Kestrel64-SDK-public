# Proportional fonts: Lato (antialiased, kerned, Latin-1) at two sizes and m5x7 (1-bit
# pixel font), baked at build time from assets/fonts.
k64_add_rom(NAME 20_fonts SOURCES main.c TITLE "K64 20 FONTS"
  FONTS "lato16=${K64_SDK_DIR}/assets/fonts/Lato.ttf:--size 16 --chars 32-126,161-255"
        "lato12=${K64_SDK_DIR}/assets/fonts/Lato.ttf:--size 12"
        "pixel=${K64_SDK_DIR}/assets/fonts/m5x7.ttf:--size 16 --chars 32-126,161-255 --mono")
