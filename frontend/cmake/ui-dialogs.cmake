if(NOT TARGET OBS::idian)
  add_subdirectory("${CMAKE_SOURCE_DIR}/shared/qt/idian" "${CMAKE_BINARY_DIR}/shared/qt/idian")
endif()

target_link_libraries(obs-studio PRIVATE OBS::idian)

if(NOT TARGET OBS::properties-view)
  add_subdirectory("${CMAKE_SOURCE_DIR}/shared/properties-view" "${CMAKE_BINARY_DIR}/shared/properties-view")
endif()

target_link_libraries(obs-studio PRIVATE OBS::properties-view)

target_sources(
  obs-studio
  PRIVATE
    dialogs/LogUploadDialog.cpp
    dialogs/LogUploadDialog.hpp
    dialogs/NameDialog.cpp
    dialogs/NameDialog.hpp
    dialogs/OAuthLogin.cpp
    dialogs/OAuthLogin.hpp
    dialogs/QCiAbout.cpp
    dialogs/QCiAbout.hpp
    dialogs/QCiBasicAdvAudio.cpp
    dialogs/QCiBasicAdvAudio.hpp
    dialogs/QCiBasicFilters.cpp
    dialogs/QCiBasicFilters.hpp
    dialogs/QCiBasicInteraction.cpp
    dialogs/QCiBasicInteraction.hpp
    dialogs/QCiBasicProperties.cpp
    dialogs/QCiBasicProperties.hpp
    dialogs/QCiBasicSourceSelect.cpp
    dialogs/QCiBasicSourceSelect.hpp
    dialogs/QCiBasicTransform.cpp
    dialogs/QCiBasicTransform.hpp
    dialogs/QCiBasicVCamConfig.cpp
    dialogs/QCiBasicVCamConfig.hpp
    dialogs/QCiLogViewer.cpp
    dialogs/QCiLogViewer.hpp
    dialogs/QCiMissingFiles.cpp
    dialogs/QCiMissingFiles.hpp
    dialogs/QCiRemux.cpp
    dialogs/QCiRemux.hpp
)
