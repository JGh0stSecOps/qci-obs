include(cmake/feature-sparkle.cmake)

target_sources(
  obs-studio
  PRIVATE
    dialogs/QCiPermissions.cpp
    dialogs/QCiPermissions.hpp
    forms/QCiPermissions.ui
    utility/CrashHandler_MacOS.mm
    utility/NativeEventFilter.cpp
    utility/platform-osx.mm
    utility/system-info-macos.mm
)
target_compile_options(obs-studio PRIVATE -Wno-quoted-include-in-framework-header -Wno-comma)

list(APPEND _frontend_objcxx_compile_options -fobjc-arc -fmodules -fcxx-modules)

set_property(
  SOURCE utility/platform-osx.mm utility/CrashHandler_MacOS.mm
  APPEND
  PROPERTY COMPILE_OPTIONS ${_frontend_objcxx_compile_options}
)

if(CMAKE_C_COMPILER_VERSION VERSION_GREATER_EQUAL 14.0.3)
  target_compile_options(obs-studio PRIVATE -Wno-error=unqualified-std-cast-call)
endif()

target_link_libraries(
  obs-studio
  PRIVATE
    "$<LINK_LIBRARY:FRAMEWORK,AppKit.framework>"
    "$<LINK_LIBRARY:FRAMEWORK,ApplicationServices.framework>"
    "$<LINK_LIBRARY:FRAMEWORK,AVFoundation.framework>"
)
