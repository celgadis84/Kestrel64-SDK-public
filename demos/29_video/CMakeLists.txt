# The clip is procedural (mkclip.py) and encoded at build time (tools/k64vid.py).
set(clip ${CMAKE_CURRENT_BINARY_DIR}/clip)
add_custom_command(OUTPUT ${clip}/clip.k64v
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/mkclip.py ${clip}
  COMMAND ${Python3_EXECUTABLE} ${K64_SDK_DIR}/tools/k64vid.py ${clip}/clip.k64v --frames ${clip}/frames --audio ${clip}/clip.wav
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/mkclip.py ${K64_SDK_DIR}/tools/k64vid.py
          ${K64_SDK_DIR}/tools/k64adpcm.py VERBATIM)
k64_add_rom(NAME 29_video SOURCES main.c TITLE "K64 29 VIDEO" ROM_FILES clip=${clip}/clip.k64v)
