# The T-rex model twice: resident (reference) and packed in ROM; plus the shrine model and
# a texture packed, to compare load times and ratios. patterns.bin (assets/gen_patterns.py):
# short-period runs for the decoder's overlapping match copies, checked against main.c's copy.
k64_add_rom(NAME 19_pack SOURCES main.c TITLE "K64 19 PACK"
  MODELS "trex_ref=${K64_SDK_DIR}/assets/trex/trex_walking_seamless.glb:--tris 2400 --lods 1600,1100,600"
  PACKED "trex_z=${K64_SDK_DIR}/assets/trex/trex_walking_seamless.glb:--tris 2400 --lods 1600,1100,600"
         "shrine_z=${K64_SDK_DIR}/demos/12_static/shrine.glb:--tex 64x64:ci4"
         "bricks_z=${K64_SDK_DIR}/demos/02_sprites/assets/bricks.png:ci4"
         "patterns_z=${CMAKE_CURRENT_SOURCE_DIR}/assets/patterns.bin")
