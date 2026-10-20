# kestrel64-sdk — ROM helper.
#
#   k64_add_rom(NAME mygame SOURCES main.c ... [TITLE "MY GAME"] [CODE NMGE]
#               [SAVE eeprom4k|eeprom16k|sram256k|sram768k|sram1m|flash1m]
#               [UCODE name=path.S ...] [TEXTURES ...] [FILES ...] [SOUNDS ...] [NO_LOGO])
#
# Produces build/<...>/<NAME>.z64 (+ .elf, .map, .bin).

function(k64_add_rom)
  cmake_parse_arguments(R "NO_LOGO" "NAME;TITLE;CODE;SAVE;ROMFS" "ROMFS_PACK;SOURCES;LIBS;TEXTURES;ROM_TEXTURES;FILES;ROM_FILES;MODELS;IMPOSTORS;SOUNDS;FONTS;MUSIC;PACKED;UCODE" ${ARGN})
  if(NOT R_TITLE)
    string(TOUPPER "${R_NAME}" R_TITLE)
  endif()
  if(NOT R_CODE)
    set(R_CODE NK6E)
  endif()
  set(save_opt "")
  if(R_SAVE)
    set(save_opt --save ${R_SAVE})
  endif()
  set(elf ${R_NAME}.elf)
  set(logo_obj "")
  if(K64_LOGO AND NOT R_NO_LOGO)
    set(logo_obj $<TARGET_OBJECTS:k64_logo>)
  endif()
  add_executable(${elf} ${R_SOURCES} $<TARGET_OBJECTS:k64_crt0> ${logo_obj})
  target_link_libraries(${elf} PRIVATE ${R_LIBS} k64)
  target_link_options(${elf} PRIVATE
    -T ${K64_SDK_DIR}/src/boot/k64.ld
    -Map ${CMAKE_CURRENT_BINARY_DIR}/${R_NAME}.map)
  set_target_properties(${elf} PROPERTIES LINK_DEPENDS ${K64_SDK_DIR}/src/boot/k64.ld)

  set(bin ${CMAKE_CURRENT_BINARY_DIR}/${R_NAME}.bin)
  set(rom ${CMAKE_CURRENT_BINARY_DIR}/${R_NAME}.z64)
  add_custom_command(TARGET ${elf} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${elf}> ${bin}
    COMMAND ${Python3_EXECUTABLE} ${K64_SDK_DIR}/tools/k64rom.py ${bin} -o ${rom}
            --elf $<TARGET_FILE:${elf}>
            --title "${R_TITLE}" --code ${R_CODE} ${save_opt}
    BYPRODUCTS ${bin} ${rom}
    VERBATIM)
  k64_add_assets(${elf} TEXTURES ${R_TEXTURES} ROM_TEXTURES ${R_ROM_TEXTURES}
                 FILES ${R_FILES} ROM_FILES ${R_ROM_FILES} MODELS ${R_MODELS} IMPOSTORS ${R_IMPOSTORS} SOUNDS ${R_SOUNDS} FONTS ${R_FONTS} MUSIC ${R_MUSIC}
                 PACKED ${R_PACKED} ROMFS ${R_ROMFS} ROMFS_PACK ${R_ROMFS_PACK})
  foreach(u ${R_UCODE})
    string(REGEX MATCH "^([A-Za-z0-9_]+)=(.+)$" _m "${u}")
    k64_add_ucode(${elf} ${CMAKE_MATCH_1} ${CMAKE_MATCH_2})
  endforeach()
endfunction()

# k64_add_ucode(<target> NAME src.S)
# Assembles RSP microcode with tools/k64rsp.py and links it into <target> (resident, .rodata):
#   extern const u8 NAME_text[], NAME_text_end[], NAME_data[], NAME_data_end[];
#   extern const u8 NAME_ovl[];  (overlays, NAME_OVL_<X>_OFF / _SIZE in NAME.h)
#   #include "NAME.h"  (sizes + .global labels as NAME_<LABEL>)
# Also writes ucode/NAME.lst (IMEM address -> source line; used by tools/prof_rsp.py).
function(k64_add_ucode target name src)
  get_filename_component(src ${src} ABSOLUTE)
  get_filename_component(srcdir ${src} DIRECTORY)
  set(outdir ${CMAKE_CURRENT_BINARY_DIR}/ucode)
  set(rsp ${K64_SDK_DIR}/tools/k64rsp.py)
  set(bake ${K64_SDK_DIR}/tools/k64bake.py)
  string(TOUPPER ${name} uname)
  file(GLOB incs ${srcdir}/*.inc ${K64_SDK_DIR}/src/rsp/*.inc)
  add_custom_command(OUTPUT ${outdir}/${name}.text.bin ${outdir}/${name}.data.bin ${outdir}/${name}.h
                            ${outdir}/${name}.ovl.bin
                            ${outdir}/${name}.lst
    COMMAND ${Python3_EXECUTABLE} ${rsp} ${src} -o ${outdir}/${name}
            --header ${outdir}/${name}.h --prefix ${uname} -I ${srcdir} -I ${K64_SDK_DIR}/src/rsp
            --listing ${outdir}/${name}.lst
    DEPENDS ${src} ${rsp} ${incs} VERBATIM)
  set(asm ${outdir}/${name}_ucode.S)
  add_custom_command(OUTPUT ${asm}
    COMMAND ${Python3_EXECUTABLE} ${bake} embed ${asm} --section rodata
            ${name}_text=${outdir}/${name}.text.bin ${name}_data=${outdir}/${name}.data.bin
            ${name}_ovl=${outdir}/${name}.ovl.bin
    DEPENDS ${outdir}/${name}.text.bin ${outdir}/${name}.data.bin ${outdir}/${name}.ovl.bin ${bake} VERBATIM)
  set_source_files_properties(${asm} PROPERTIES OBJECT_DEPENDS "${outdir}/${name}.text.bin;${outdir}/${name}.data.bin;${outdir}/${name}.ovl.bin")
  target_sources(${target} PRIVATE ${asm} ${outdir}/${name}.h)
  target_include_directories(${target} PRIVATE ${outdir})
endfunction()

# k64_add_assets(<elf-target>
#     [TEXTURES name=path.png:format ...]     -> baked .k64t, resident (.rodata)
#     [ROM_TEXTURES name=path.png:format ...] -> baked .k64t, ROM-resident (PI DMA on demand)
#     [SOUNDS name=path.wav[:adpcm options] ...] -> VADPCM .k64a, resident (k64_adpcm_load)
#     [FONTS name=path.ttf[:font options] ...] -> .k64f, resident (k64_font_from_blob),
#                                                options e.g. "--size 16 --chars 32-126"
#     [IMPOSTORS name=model.glb[:impostor options] ...] -> .k64i, resident (k64_impostor_init),
#                                                options e.g. "--views 8 --cell 64x32"
#     [MUSIC name=song.mid[:music options] ...] -> .k64q, resident (k64_music_play),
#                                                options e.g. "--loop"
#     [FILES name=path ...] [ROM_FILES name=path ...]
#     [PACKED name=path ...]                  -> .k64z (tools/k64pack.py), ROM-resident
#                                                (k64_unpack_rom); path.png:format and
#                                                path.glb:options are baked first)
#     [ROMFS dir [ROMFS_PACK glob ...]]       -> one .k64fs image (tools/k64fs.py) of the
#                                                tree, ROM-resident, read by path through
#                                                k64/romfs.h; files matching a glob packed
# Each asset is reachable from C as `extern const u8 name[], name_end[];`.
function(k64_add_assets elf)
  cmake_parse_arguments(A "" "ROMFS" "ROMFS_PACK;TEXTURES;ROM_TEXTURES;FILES;ROM_FILES;MODELS;IMPOSTORS;SOUNDS;FONTS;MUSIC;PACKED" ${ARGN})
  set(outdir ${CMAKE_CURRENT_BINARY_DIR}/assets)
  set(bake ${K64_SDK_DIR}/tools/k64bake.py)
  foreach(kind RAM ROM)
    set(pairs "")
    set(deps "")
    if(kind STREQUAL RAM)
      set(texs ${A_TEXTURES})
      set(files ${A_FILES})
    else()
      set(texs ${A_ROM_TEXTURES})
      set(files ${A_ROM_FILES})
    endif()
    foreach(t ${texs})
      string(REGEX MATCH "^([A-Za-z0-9_]+)=(.+):([a-z0-9]+)$" _m "${t}")
      set(name ${CMAKE_MATCH_1})
      get_filename_component(src ${CMAKE_MATCH_2} ABSOLUTE)
      set(fmt ${CMAKE_MATCH_3})
      set(out ${outdir}/${name}.k64t)
      add_custom_command(OUTPUT ${out}
        COMMAND ${Python3_EXECUTABLE} ${bake} tex ${src} ${out} --format ${fmt}
        DEPENDS ${src} ${bake} VERBATIM)
      list(APPEND pairs "${name}=${out}")
      list(APPEND deps ${out})
    endforeach()
    # MODELS name=model.glb[:k64bake model options separated by ';' or spaces] (RAM only:
    # the loader patches the blob in place)
    if(kind STREQUAL RAM)
      foreach(mdl ${A_MODELS})
        string(REGEX MATCH "^([A-Za-z0-9_]+)=(.+\.glb)(:(.*))?$" _m "${mdl}")
        set(name ${CMAKE_MATCH_1})
        get_filename_component(src ${CMAKE_MATCH_2} ABSOLUTE)
        separate_arguments(opts UNIX_COMMAND "${CMAKE_MATCH_4}")
        set(out ${outdir}/${name}.k64m)
        add_custom_command(OUTPUT ${out}
          COMMAND ${Python3_EXECUTABLE} ${bake} model ${src} ${out} ${opts}
          DEPENDS ${src} ${bake} ${K64_SDK_DIR}/tools/k64model.py VERBATIM)
        list(APPEND pairs "${name}=${out}")
        list(APPEND deps ${out})
      endforeach()
      # IMPOSTORS name=model.glb[:k64bake impostor options] -> .k64i (k64_impostor_init)
      foreach(imp ${A_IMPOSTORS})
        string(REGEX MATCH "^([A-Za-z0-9_]+)=(.+\.glb)(:(.*))?$" _m "${imp}")
        set(name ${CMAKE_MATCH_1})
        get_filename_component(src ${CMAKE_MATCH_2} ABSOLUTE)
        separate_arguments(opts UNIX_COMMAND "${CMAKE_MATCH_4}")
        set(out ${outdir}/${name}.k64i)
        add_custom_command(OUTPUT ${out}
          COMMAND ${Python3_EXECUTABLE} ${bake} impostor ${src} ${out} ${opts}
          DEPENDS ${src} ${bake} ${K64_SDK_DIR}/tools/k64impostor.py ${K64_SDK_DIR}/tools/k64model.py VERBATIM)
        list(APPEND pairs "${name}=${out}")
        list(APPEND deps ${out})
      endforeach()
      # SOUNDS name=sound.wav[:k64bake adpcm options] -> VADPCM .k64a (k64_adpcm_load)
      foreach(snd ${A_SOUNDS})
        string(REGEX MATCH "^([A-Za-z0-9_]+)=(.+\.wav)(:(.*))?$" _m "${snd}")
        set(name ${CMAKE_MATCH_1})
        get_filename_component(src ${CMAKE_MATCH_2} ABSOLUTE)
        separate_arguments(opts UNIX_COMMAND "${CMAKE_MATCH_4}")
        set(out ${outdir}/${name}.k64a)
        add_custom_command(OUTPUT ${out}
          COMMAND ${Python3_EXECUTABLE} ${bake} adpcm ${src} ${out} ${opts}
          DEPENDS ${src} ${bake} ${K64_SDK_DIR}/tools/k64adpcm.py VERBATIM)
        list(APPEND pairs "${name}=${out}")
        list(APPEND deps ${out})
      endforeach()
      # MUSIC name=song.mid[:k64bake music options] -> .k64q (k64_music_play)
      foreach(mus ${A_MUSIC})
        string(REGEX MATCH "^([A-Za-z0-9_]+)=(.+\.midi?)(:(.*))?$" _m "${mus}")
        set(name ${CMAKE_MATCH_1})
        get_filename_component(src ${CMAKE_MATCH_2} ABSOLUTE)
        separate_arguments(opts UNIX_COMMAND "${CMAKE_MATCH_4}")
        set(out ${outdir}/${name}.k64q)
        add_custom_command(OUTPUT ${out}
          COMMAND ${Python3_EXECUTABLE} ${bake} music ${src} ${out} ${opts}
          DEPENDS ${src} ${bake} ${K64_SDK_DIR}/tools/k64music.py VERBATIM)
        list(APPEND pairs "${name}=${out}")
        list(APPEND deps ${out})
      endforeach()
      # FONTS name=font.ttf|otf[:k64bake font options]
      foreach(fnt ${A_FONTS})
        string(REGEX MATCH "^([A-Za-z0-9_]+)=(.+\.[ot]tf)(:(.*))?$" _m "${fnt}")
        set(name ${CMAKE_MATCH_1})
        get_filename_component(src ${CMAKE_MATCH_2} ABSOLUTE)
        separate_arguments(opts UNIX_COMMAND "${CMAKE_MATCH_4}")
        set(out ${outdir}/${name}.k64f)
        add_custom_command(OUTPUT ${out}
          COMMAND ${Python3_EXECUTABLE} ${bake} font ${src} ${out} ${opts}
          DEPENDS ${src} ${bake} ${K64_SDK_DIR}/tools/k64font.py VERBATIM)
        list(APPEND pairs "${name}=${out}")
        list(APPEND deps ${out})
      endforeach()
    endif()
    if(kind STREQUAL ROM AND A_ROMFS)
      get_filename_component(fsdir ${A_ROMFS} ABSOLUTE)
      file(GLOB_RECURSE fsfiles CONFIGURE_DEPENDS ${fsdir}/*)
      set(fsopts "")
      foreach(g ${A_ROMFS_PACK})
        list(APPEND fsopts --pack ${g})
      endforeach()
      set(out ${outdir}/romfs.k64fs)
      add_custom_command(OUTPUT ${out}
        COMMAND ${Python3_EXECUTABLE} ${K64_SDK_DIR}/tools/k64fs.py ${out} ${fsdir} ${fsopts}
        DEPENDS ${fsfiles} ${K64_SDK_DIR}/tools/k64fs.py ${K64_SDK_DIR}/tools/k64pack.py VERBATIM)
      list(APPEND pairs "k64_romfs=${out}")
      list(APPEND deps ${out})
    endif()
    if(kind STREQUAL ROM)
      set(pack ${K64_SDK_DIR}/tools/k64pack.py)
      foreach(p ${A_PACKED})
        # name=file.png:format, name=file.glb[:options] or name=any_file (drive letters
        # contain ':' too, so the extension anchors the split)
        if(p MATCHES "^([A-Za-z0-9_]+)=(.+\.(png|glb))(:(.*))?$")
          set(opts "${CMAKE_MATCH_5}")
        else()
          string(REGEX MATCH "^([A-Za-z0-9_]+)=(.+)$" _m "${p}")
          set(opts "")
        endif()
        set(name ${CMAKE_MATCH_1})
        get_filename_component(src ${CMAKE_MATCH_2} ABSOLUTE)
        set(in ${src})
        if(src MATCHES "\.png$")
          set(in ${outdir}/${name}.k64t)
          add_custom_command(OUTPUT ${in}
            COMMAND ${Python3_EXECUTABLE} ${bake} tex ${src} ${in} --format ${opts}
            DEPENDS ${src} ${bake} VERBATIM)
        elseif(src MATCHES "\.glb$")
          separate_arguments(mopts UNIX_COMMAND "${opts}")
          set(in ${outdir}/${name}.k64m)
          add_custom_command(OUTPUT ${in}
            COMMAND ${Python3_EXECUTABLE} ${bake} model ${src} ${in} ${mopts}
            DEPENDS ${src} ${bake} ${K64_SDK_DIR}/tools/k64model.py VERBATIM)
        endif()
        set(out ${outdir}/${name}.k64z)
        add_custom_command(OUTPUT ${out}
          COMMAND ${Python3_EXECUTABLE} ${pack} ${in} ${out}
          DEPENDS ${in} ${pack} VERBATIM)
        list(APPEND pairs "${name}=${out}")
        list(APPEND deps ${out})
      endforeach()
    endif()
    foreach(f ${files})
      string(REGEX MATCH "^([A-Za-z0-9_]+)=(.+)$" _m "${f}")
      get_filename_component(src ${CMAKE_MATCH_2} ABSOLUTE)
      list(APPEND pairs "${CMAKE_MATCH_1}=${src}")
      list(APPEND deps ${src})
    endforeach()
    if(pairs)
      string(TOLOWER ${kind} k)
      set(asm ${outdir}/${elf}_${k}_assets.S)
      if(kind STREQUAL RAM)
        set(sec rodata)
      else()
        set(sec rom)
      endif()
      add_custom_command(OUTPUT ${asm}
        COMMAND ${Python3_EXECUTABLE} ${bake} embed ${asm} --section ${sec} ${pairs}
        DEPENDS ${deps} ${bake} VERBATIM)
      set_source_files_properties(${asm} PROPERTIES OBJECT_DEPENDS "${deps}")
      target_sources(${elf} PRIVATE ${asm})
    endif()
  endforeach()
endfunction()
