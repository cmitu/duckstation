add_subdirectory(fmt EXCLUDE_FROM_ALL)
disable_compiler_warnings_for_target(fmt)
add_subdirectory(stb EXCLUDE_FROM_ALL)
disable_compiler_warnings_for_target(stb)
add_subdirectory(zlib EXCLUDE_FROM_ALL)
disable_compiler_warnings_for_target(zlib)
add_subdirectory(zstd EXCLUDE_FROM_ALL)
disable_compiler_warnings_for_target(zstd)
add_subdirectory(minizip EXCLUDE_FROM_ALL)
disable_compiler_warnings_for_target(minizip)
add_subdirectory(lzma EXCLUDE_FROM_ALL)
disable_compiler_warnings_for_target(lzma)
add_subdirectory(libchdr EXCLUDE_FROM_ALL)
disable_compiler_warnings_for_target(libchdr)
add_subdirectory(xxhash EXCLUDE_FROM_ALL)
disable_compiler_warnings_for_target(xxhash)
add_subdirectory(rapidjson EXCLUDE_FROM_ALL)
add_subdirectory(glslang EXCLUDE_FROM_ALL)
disable_compiler_warnings_for_target(glslang)
add_subdirectory(imgui EXCLUDE_FROM_ALL)
disable_compiler_warnings_for_target(imgui)
add_subdirectory(simpleini EXCLUDE_FROM_ALL)
disable_compiler_warnings_for_target(simpleini)
add_subdirectory(vulkan EXCLUDE_FROM_ALL)
add_subdirectory(soundtouch EXCLUDE_FROM_ALL)
disable_compiler_warnings_for_target(soundtouch)
add_subdirectory(googletest EXCLUDE_FROM_ALL)
add_subdirectory(cpuinfo EXCLUDE_FROM_ALL)
disable_compiler_warnings_for_target(cpuinfo)
add_subdirectory(fast_float EXCLUDE_FROM_ALL)
add_subdirectory(reshadefx EXCLUDE_FROM_ALL)
disable_compiler_warnings_for_target(reshadefx)
add_subdirectory(rcheevos EXCLUDE_FROM_ALL)
disable_compiler_warnings_for_target(rcheevos)

if(ENABLE_CUBEB)
  add_subdirectory(cubeb EXCLUDE_FROM_ALL)
  disable_compiler_warnings_for_target(cubeb)
  disable_compiler_warnings_for_target(speex)
endif()

if(ENABLE_OPENGL)
  add_subdirectory(glad EXCLUDE_FROM_ALL)
  disable_compiler_warnings_for_target(glad)
endif()

if(ENABLE_DISCORD_PRESENCE)
  add_subdirectory(discord-rpc EXCLUDE_FROM_ALL)
  disable_compiler_warnings_for_target(discord-rpc)
endif()

if(${CPU_ARCH} STREQUAL "aarch32" OR ${CPU_ARCH} STREQUAL "aarch64")
  add_subdirectory(vixl EXCLUDE_FROM_ALL)
  disable_compiler_warnings_for_target(vixl)
endif()

if(${CPU_ARCH} STREQUAL "riscv64")
  add_subdirectory(biscuit EXCLUDE_FROM_ALL)
  disable_compiler_warnings_for_target(biscuit)
  add_subdirectory(riscv-disas EXCLUDE_FROM_ALL)
  disable_compiler_warnings_for_target(riscv-disas)
endif()

if(WIN32)
  add_subdirectory(d3d12ma EXCLUDE_FROM_ALL)
  disable_compiler_warnings_for_target(d3d12ma)
  add_subdirectory(winpixeventruntime EXCLUDE_FROM_ALL)
endif()

if(APPLE)
  add_subdirectory(spirv-cross EXCLUDE_FROM_ALL)
  disable_compiler_warnings_for_target(spirv-cross)
endif()
