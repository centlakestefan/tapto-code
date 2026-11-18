# The Windows debugger engine the windebug plugin ships with: Microsoft's
# redistributable DbgEng and SymSrv NuGet packages (the Windows SDK licence,
# https://aka.ms/WinSDKLicenseURL), into plugins/windebug-engine beside the
# plugin, in the build tree and the install. With them the plugin opens a dump
# with symbols downloaded on a machine without the Debugging Tools for
# Windows. The extension DLLs (!analyze, the kernel extensions) are in neither
# package: those come with the Debugging Tools, which the plugin prefers when
# installed.
#
# Fetched at configure time, each package checked against its SHA-256, and
# kept in the build tree, so a reconfigure does not download again.
# -DTAPTO_WINDEBUG_ENGINE=OFF leaves them out, and the plugin uses the Debugging
# Tools or System32's engine.

option(TAPTO_WINDEBUG_ENGINE "Ship Microsoft's redistributable debugger engine with the windebug plugin" ON)

set(_windebug_engine_version "20260319.1511.0")
set(_windebug_engine_packages
  "Microsoft.Debugging.Platform.DbgEng|875678516f9ceed4a1c8b9b106d165106fcaa38723ad7c584c47b95b231600de|dbgcore.dll,dbgeng.dll,dbghelp.dll,dbgmodel.dll,msdia140.dll"
  "Microsoft.Debugging.Platform.SymSrv|8d24440267581038de0101b36c0d75daf7f063d86f3d6adca61038c236df7ce7|symsrv.dll")

function(tapto_windebug_engine)
  if(NOT WIN32 OR NOT TAPTO_WINDEBUG_ENGINE)
    return()
  endif()
  # The packages' folder for the architecture being built.
  if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    set(_arch x86)
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$" OR CMAKE_GENERATOR_PLATFORM STREQUAL "ARM64")
    set(_arch arm64)
  else()
    set(_arch amd64)
  endif()

  set(_cache "${CMAKE_BINARY_DIR}/_deps/windebug-engine")
  set(_out "${CMAKE_BINARY_DIR}/plugins/windebug-engine")
  set(_files "")
  foreach(_entry IN LISTS _windebug_engine_packages)
    string(REPLACE "|" ";" _entry "${_entry}")
    list(GET _entry 0 _id)
    list(GET _entry 1 _sha)
    list(GET _entry 2 _dlls)
    string(REPLACE "," ";" _dlls "${_dlls}")
    string(TOLOWER "${_id}" _lid)
    set(_pkg "${_cache}/${_lid}.${_windebug_engine_version}.nupkg")
    set(_dir "${_cache}/${_lid}.${_windebug_engine_version}")
    if(NOT EXISTS "${_dir}/.extracted")
      message(STATUS "windebug plugin: fetching ${_id} ${_windebug_engine_version}")
      file(DOWNLOAD
        "https://api.nuget.org/v3-flatcontainer/${_lid}/${_windebug_engine_version}/${_lid}.${_windebug_engine_version}.nupkg"
        "${_pkg}" EXPECTED_HASH SHA256=${_sha} STATUS _st TLS_VERIFY ON)
      list(GET _st 0 _rc)
      if(NOT _rc EQUAL 0)
        list(GET _st 1 _msg)
        message(WARNING "windebug plugin: ${_id} could not be fetched (${_msg}); the plugin will use the "
                        "Debugging Tools for Windows or System32's engine. -DTAPTO_WINDEBUG_ENGINE=OFF silences this.")
        return()
      endif()
      file(REMOVE_RECURSE "${_dir}")
      file(ARCHIVE_EXTRACT INPUT "${_pkg}" DESTINATION "${_dir}" PATTERNS "content/${_arch}/*")
      file(TOUCH "${_dir}/.extracted")
    endif()
    foreach(_dll IN LISTS _dlls)
      set(_src "${_dir}/content/${_arch}/${_dll}")
      if(NOT EXISTS "${_src}")
        message(WARNING "windebug plugin: ${_id} has no content/${_arch}/${_dll}")
        return()
      endif()
      list(APPEND _files "${_src}")
    endforeach()
  endforeach()

  file(COPY ${_files} DESTINATION "${_out}")
  install(FILES ${_files} DESTINATION "${TAPTO_PLUGIN_INSTALL_DIR}/windebug-engine")
  message(STATUS "windebug plugin: Microsoft's debugger engine ${_windebug_engine_version} (${_arch}) in plugins/windebug-engine")
endfunction()
