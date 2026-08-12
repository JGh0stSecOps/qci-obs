cmake_minimum_required(VERSION 3.28...3.30)

option(ENABLE_VIRTUALCAM "Build OBS Virtualcam" ON)

if(NOT ENABLE_VIRTUALCAM)
  target_disable_feature(mac-virtualcam "macOS Virtual Camera support")
  return()
endif()

target_enable_feature(mac-virtualcam "macOS Virtual Camera support")

# --- QCi fork: virtual camera identity guard (configure-time, fatal) ------------------------
#
# These three UUIDs are baked into the CMIO camera extension's Info.plist
# (src/camera-extension/cmake/macos/Info.plist.in) and into the obs-plugin's Info.plist
# (src/obs-plugin/cmake/macos/Info.plist.in). They are how macOS identifies the virtual
# camera device and its source/sink streams.
#
# WHY THIS IS FATAL AND NOT A WARNING:
# The system-extension request delegate in src/obs-plugin/plugin-main.mm returns
# OSSystemExtensionReplacementActionReplace *unconditionally*. It never inspects the
# incumbent extension's version, team, or signature. So if this fork ships an identity that
# an already-installed camera extension is using, activating the fork does not error and does
# not prompt a conflict: it silently REPLACES the working camera the operator depends on,
# system-wide, for every app that consumes it. Recovery means re-registering the displaced
# extension, and in the cases where the OS has cached the stale identity it means booting to
# recovery and disabling SIP. That is an unacceptable outcome for a build-time typo, so the
# configure fails and no colliding artifact is ever produced, signed, or activated.
#
# WHAT IS FORBIDDEN, AND WHY THIS LIST IS NOT JUST UPSTREAM'S:
#
# The first three are upstream OBS Studio's, live on this machine as
# com.obsproject.obs-studio.mac-camera-extension (team 2MMRE5MTB8, [activated enabled]); they are
# the exact values this fork inherited before the re-mint. `system_profiler SPCameraDataType`
# confirms 7626645E-... is the Unique ID of the "OBS Virtual Camera" device right now.
#   7626645E-4425-469E-9D8B-97E0FA59AC75 - upstream OBSCameraDeviceUUID: the device itself.
#       Sharing it means both extensions claim the same CMIO device -> straight takeover.
#   A8D7B8AA-65AD-4D21-9C42-66480DBFA8E1 - upstream OBSCameraSourceUUID: the stream apps
#       read frames from. Sharing it hijacks what every camera consumer sees.
#   A3F16177-7044-4DD8-B900-72E2419F7A9A - upstream OBSCameraSinkUUID: the stream OBS pushes
#       frames into. Sharing it cross-wires the two OBS builds' frame paths.
#
# The rest belong to com.qci.QCiCam.Extension, the operator's OWN separate virtual camera, also
# [activated enabled] on this machine. Listing them is not symmetry for its own sake -- it is the
# case that matters MORE than upstream's. QCiCam is signed by team V46GH2Q974, the same team as
# this fork. A same-identifier system extension from a DIFFERENT team is the collision Apple's own
# validation rejects; a same-identifier extension from the SAME team is precisely the case macOS
# permits to replace its incumbent, which is when the unconditional
# OSSystemExtensionReplacementActionReplace in src/obs-plugin/plugin-main.mm gets to act. So the
# three entries above largely duplicate a protection the OS already provides, and these are the
# ones that are genuinely ours to enforce. Values read from `system_profiler SPCameraDataType`
# and from the live extension binary on 2026-08-12:
#   6B8A6A5E-1C2D-4E3F-9A70-0C10CA000001 - QCiCam "QCi Virtual Camera" device
#   7F3C9A21-4D5E-4B8A-9C10-F10CCA400001 - QCiCam "QCi FlowCam" device
#   7F3C9A21-4D5E-4B8A-9C10-F10CCA400002 - QCiCam source stream
#   7F3C9A21-4D5E-4B8A-9C10-F10CCA400003 - QCiCam sink stream
#
# Empty is equally forbidden: an unset UUID configure_file()s into an empty Info.plist key,
# which is not a distinct identity at all, and upstream's soft AUTHOR_WARNING for that case
# merely return()s and silently drops the extension from the build.
#
# LIMIT OF THIS CHECK, stated plainly: it is a static list of identities known on 2026-08-12. It
# cannot know about a camera installed tomorrow. It is a guard against pasting an identifier from
# a neighbouring project -- the mistake that actually happens -- not a proof of global uniqueness.
# Add to QCI_EXTRA_FORBIDDEN_VIRTUALCAM_UUIDS (a cache variable, no code edit needed) if another
# virtual camera lands on the machine; `system_profiler SPCameraDataType` lists the live ones.
#
# If you are legitimately re-minting, run `uuidgen` three times and set all three in
# CMakePresets.json. Never copy a UUID from another project, and never reuse one across the
# three roles.
set(QCI_EXTRA_FORBIDDEN_VIRTUALCAM_UUIDS
    ""
    CACHE STRING
    "Additional UUIDs the virtual camera identity guard must reject (semicolon-separated)."
)

# Each forbidden UUID carries the name of who owns it, so the failure names the actual conflict.
# An error that says "this belongs to upstream OBS Studio" when the value in fact belongs to
# com.qci.QCiCam.Extension sends the reader looking in the wrong place; the owner is the single
# most useful thing the message can carry.
set(_qci_forbidden_virtualcam_uuids "")

macro(_qci_forbid_uuid uuid owner)
  string(TOUPPER "${uuid}" _qci_forbid_key)
  list(APPEND _qci_forbidden_virtualcam_uuids "${_qci_forbid_key}")
  set("_qci_uuid_owner_${_qci_forbid_key}" "${owner}")
  unset(_qci_forbid_key)
endmacro()

_qci_forbid_uuid("7626645E-4425-469E-9D8B-97E0FA59AC75" "upstream OBS Studio's camera extension (device)")
_qci_forbid_uuid("A8D7B8AA-65AD-4D21-9C42-66480DBFA8E1" "upstream OBS Studio's camera extension (source stream)")
_qci_forbid_uuid("A3F16177-7044-4DD8-B900-72E2419F7A9A" "upstream OBS Studio's camera extension (sink stream)")
_qci_forbid_uuid("6B8A6A5E-1C2D-4E3F-9A70-0C10CA000001" "com.qci.QCiCam.Extension, device \"QCi Virtual Camera\"")
_qci_forbid_uuid("7F3C9A21-4D5E-4B8A-9C10-F10CCA400001" "com.qci.QCiCam.Extension, device \"QCi FlowCam\"")
_qci_forbid_uuid("7F3C9A21-4D5E-4B8A-9C10-F10CCA400002" "com.qci.QCiCam.Extension (source stream)")
_qci_forbid_uuid("7F3C9A21-4D5E-4B8A-9C10-F10CCA400003" "com.qci.QCiCam.Extension (sink stream)")

foreach(_qci_extra_uuid IN LISTS QCI_EXTRA_FORBIDDEN_VIRTUALCAM_UUIDS)
  string(STRIP "${_qci_extra_uuid}" _qci_extra_uuid)
  if(NOT _qci_extra_uuid STREQUAL "")
    _qci_forbid_uuid("${_qci_extra_uuid}" "an entry in QCI_EXTRA_FORBIDDEN_VIRTUALCAM_UUIDS")
  endif()
endforeach()
unset(_qci_extra_uuid)

set(_qci_seen_virtualcam_uuids "")
foreach(_qci_uuid_var IN ITEMS VIRTUALCAM_DEVICE_UUID VIRTUALCAM_SOURCE_UUID VIRTUALCAM_SINK_UUID)
  string(STRIP "${${_qci_uuid_var}}" _qci_uuid_value)
  string(TOUPPER "${_qci_uuid_value}" _qci_uuid_upper)

  if(_qci_uuid_value STREQUAL "")
    message(
      FATAL_ERROR
      "QCi virtual camera identity guard: ${_qci_uuid_var} is empty.\n"
      "The camera extension would ship an Info.plist with no distinct identity. "
      "Mint one with `uuidgen` and set it in CMakePresets.json."
    )
  endif()

  check_uuid("${_qci_uuid_value}" _qci_uuid_valid)
  if(NOT _qci_uuid_valid)
    message(
      FATAL_ERROR
      "QCi virtual camera identity guard: ${_qci_uuid_var} is not a valid UUID "
      "(got '${_qci_uuid_value}'). Mint one with `uuidgen`."
    )
  endif()

  if(_qci_uuid_upper IN_LIST _qci_forbidden_virtualcam_uuids)
    message(
      FATAL_ERROR
      "QCi virtual camera identity guard: ${_qci_uuid_var} is set to '${_qci_uuid_value}', "
      "which already belongs to ${_qci_uuid_owner_${_qci_uuid_upper}}.\n"
      "That camera is installed and activated on this machine. Two CMIO devices or streams "
      "claiming one identity is not an error macOS reports -- the second one shadows or "
      "cross-wires the first, so every application that opens 'the' camera may get this build's "
      "frames instead of the ones it asked for. Where the collision is a system extension of the "
      "SAME signing team, the delegate in plugins/mac-virtualcam/src/obs-plugin/plugin-main.mm "
      "returns OSSystemExtensionReplacementActionReplace unconditionally, so activating this "
      "would take over that camera system-wide -- recoverable only by re-registering extensions, "
      "and sometimes only with SIP disabled.\n"
      "Mint a fresh UUID with `uuidgen` and set it in CMakePresets.json. Refusing to configure."
    )
  endif()

  if(_qci_uuid_upper IN_LIST _qci_seen_virtualcam_uuids)
    message(
      FATAL_ERROR
      "QCi virtual camera identity guard: ${_qci_uuid_var} duplicates another "
      "VIRTUALCAM_*_UUID ('${_qci_uuid_value}'). The device, source and sink must be three "
      "distinct identities. Mint a fresh UUID with `uuidgen`."
    )
  endif()
  list(APPEND _qci_seen_virtualcam_uuids "${_qci_uuid_upper}")

  # Ship the value that was actually checked. The three Info.plist templates interpolate
  # ${VIRTUALCAM_*_UUID} verbatim (src/camera-extension/cmake/macos/Info.plist.in,
  # src/obs-plugin/cmake/macos/Info.plist.in), so without this write-back the guard validates the
  # STRIPPED string and configure_file() then bakes in the RAW one. A padded-but-valid UUID passed
  # every check above and landed in OBSCameraDeviceUUID with leading whitespace, where
  # CFUUIDCreateFromString returns NULL and the camera is simply never found -- reported by the
  # guard as OK. This is a plain (non-cache) set, so it shadows the preset's cache entry for this
  # directory and everything below it, which is every consumer of these variables.
  set(${_qci_uuid_var} "${_qci_uuid_value}")
endforeach()

foreach(_qci_forbidden_uuid IN LISTS _qci_forbidden_virtualcam_uuids)
  unset("_qci_uuid_owner_${_qci_forbidden_uuid}")
endforeach()
unset(_qci_forbidden_uuid)

unset(_qci_uuid_var)
unset(_qci_uuid_value)
unset(_qci_uuid_upper)
unset(_qci_uuid_valid)
unset(_qci_seen_virtualcam_uuids)
unset(_qci_forbidden_virtualcam_uuids)
# --- end virtual camera identity guard -----------------------------------------------------

add_library(obs-mach-protocol INTERFACE)
add_library(OBS::mach-protocol ALIAS obs-mach-protocol)

target_sources(obs-mach-protocol INTERFACE src/common/MachProtocol.h src/common/data/placeholder.png)
target_include_directories(obs-mach-protocol INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src/common")

add_subdirectory(src/obs-plugin)
add_subdirectory(src/dal-plugin)
add_subdirectory(src/camera-extension)

# --- QCi fork: camera extension identity, handed to the obs-plugin ----------------------------
#
# THIS IS THE IDENTIFIER THE UUID GUARD ABOVE DOES NOT COVER, AND IT IS THE PRIMARY ONE.
#
# +[OSSystemExtensionRequest activationRequestForExtension:] takes a BUNDLE IDENTIFIER, and
# -request:actionForReplacingExtension:withExtension: fires only when an extension with THAT
# identifier is already installed. The three VIRTUALCAM_*_UUIDs never enter macOS's replacement
# decision at all -- they are what CMIO matches devices and streams on, one layer further down.
# So the guard above hard-fails on the secondary identifiers and, until this block, let the
# identifier that actually triggers the unconditional Replace through unchecked. Typing
# com.obsproject.obs-studio.mac-camera-extension into src/camera-extension/CMakeLists.txt
# configured, built and signed without a word.
#
# It was also written twice with no link between the copies: once as PRODUCT_BUNDLE_IDENTIFIER on
# the mac-camera-extension target, and once as a string literal in
# src/obs-plugin/plugin-main.mm. Either half could drift alone. If plugin-main.mm's copy had kept
# upstream's value through the rename, this fork would submit activation requests naming STOCK
# OBS'S extension; if the CMake copy had, the fork would ship an extension under stock OBS's
# identity. Both are silent. So the value is read back off the target that actually carries it --
# the same discipline the DAL block below applies, applied to the more dangerous identifier -- and
# handed to the consumer as a compile definition. One place to change it, no literal that can drift.
#
# The display name is asserted here too. It is not a machine identifier, but on a machine where
# stock OBS is the ROLLBACK, two rows called "OBS Virtual Camera" in System Settings > Camera
# Extensions is an operator staring at an uninstall button with no way to tell which one it points
# at. An identity you cannot distinguish in the UI is not a distinct identity.
if(NOT TARGET mac-camera-extension)
  message(
    FATAL_ERROR
    "QCi camera extension identity: target mac-camera-extension does not exist.\n"
    "src/camera-extension/CMakeLists.txt return()s early when a VIRTUALCAM_*_UUID is unset or "
    "malformed -- but the identity guard above already fails hard on both of those, so reaching "
    "here means something else dropped the extension from the build. src/obs-plugin/plugin-main.mm "
    "would then be compiled to submit an activation request for an extension this build does not "
    "produce. Refusing to configure."
  )
endif()

get_target_property(_qci_ext_bundle_id mac-camera-extension XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER)
get_target_property(_qci_ext_display_name mac-camera-extension XCODE_ATTRIBUTE_INFOPLIST_KEY_CFBundleDisplayName)

# get_target_property() leaves "<var>-NOTFOUND" when a property is unset, and CMake's if() treats
# any value ending in -NOTFOUND (and the empty string) as false, so one test covers both.
if(NOT _qci_ext_bundle_id)
  message(
    FATAL_ERROR
    "QCi camera extension identity: mac-camera-extension does not set PRODUCT_BUNDLE_IDENTIFIER.\n"
    "That identifier is what src/obs-plugin/plugin-main.mm passes to "
    "+[OSSystemExtensionRequest activationRequestForExtension:]. Set it in "
    "plugins/mac-virtualcam/src/camera-extension/CMakeLists.txt. Refusing to configure."
  )
endif()

if(NOT _qci_ext_display_name)
  message(
    FATAL_ERROR
    "QCi camera extension identity: mac-camera-extension does not set "
    "INFOPLIST_KEY_CFBundleDisplayName.\n"
    "OBSCameraProviderSource.swift reads CFBundleDisplayName out of the built bundle to name the "
    "CMIO device, and fatalErrors without it -- the extension would crash on launch rather than "
    "publish a camera. Set it in plugins/mac-virtualcam/src/camera-extension/CMakeLists.txt. "
    "Refusing to configure."
  )
endif()

if(_qci_ext_bundle_id STREQUAL "com.obsproject.obs-studio.mac-camera-extension")
  message(
    FATAL_ERROR
    "QCi camera extension identity: mac-camera-extension claims upstream OBS Studio's system "
    "extension identifier 'com.obsproject.obs-studio.mac-camera-extension'.\n"
    "That extension is installed and activated on this machine (team 2MMRE5MTB8). macOS keys "
    "system-extension replacement on the identifier, and the delegate in "
    "plugins/mac-virtualcam/src/obs-plugin/plugin-main.mm returns "
    "OSSystemExtensionReplacementActionReplace unconditionally, so activating this would attempt a "
    "silent system-wide takeover of the operator's working virtual camera -- the exact outcome the "
    "UUID guard at the top of this file exists to prevent, via the identifier the UUIDs do not "
    "control. Give mac-camera-extension a distinct PRODUCT_BUNDLE_IDENTIFIER. Refusing to configure."
  )
endif()

string(TOUPPER "${_qci_ext_display_name}" _qci_ext_display_name_upper)
if(_qci_ext_display_name_upper STREQUAL "OBS VIRTUAL CAMERA")
  message(
    FATAL_ERROR
    "QCi camera extension identity: mac-camera-extension's CFBundleDisplayName is "
    "'${_qci_ext_display_name}', which is byte-for-byte the name upstream OBS Studio's extension is "
    "live under on this machine (`systemextensionsctl list`, and `system_profiler "
    "SPCameraDataType`).\n"
    "Both would appear as identically named rows in System Settings > General > Login Items & "
    "Extensions > Camera Extensions -- a pane whose only controls are enable, disable and uninstall "
    "-- and as two identically named devices in every camera picker. Stock OBS is this migration's "
    "rollback; a rollback the operator cannot pick out of a list is not a rollback. Give it a "
    "distinct INFOPLIST_KEY_CFBundleDisplayName. Refusing to configure."
  )
endif()

target_compile_definitions(
  mac-virtualcam
  PRIVATE OBS_CAMERA_EXTENSION_BUNDLE_ID="${_qci_ext_bundle_id}"
)

message(STATUS "QCi camera extension identity: ${_qci_ext_bundle_id} (\"${_qci_ext_display_name}\")")

unset(_qci_ext_bundle_id)
unset(_qci_ext_display_name)
unset(_qci_ext_display_name_upper)
# --- end camera extension identity -------------------------------------------------------------

# --- QCi fork: legacy DAL plug-in identity, handed to the obs-plugin --------------------------
#
# /Library/CoreMediaIO/Plug-Ins/DAL is a SHARED, root-owned, system-wide directory. Stock OBS's
# plug-in, this fork's, and any other vendor's all live in it side by side, and
# src/obs-plugin/plugin-main.mm reaches into it with `rm -rf` under
# `do shell script ... with administrator privileges`. That code must therefore address exactly
# the one bundle THIS build produces and installs — never a directory, never a name it merely
# assumes. Upstream hardcoded "obs-mac-virtualcam.plugin" at four call sites; renaming
# obs-dal-plugin without touching them silently retargets all four at the stock OBS install
# sitting next to us, which means the fork would delete another application's plug-in as root.
#
# So the name and identifier are read back off the obs-dal-plugin target itself, after its
# subdirectory has been processed, and handed to the consumer as compile definitions. There is
# exactly one place to change them (src/dal-plugin/CMakeLists.txt) and no literal can drift from
# it. Values are read with get_target_property rather than $<TARGET_...> generator expressions so
# that the sanity checks below can run at configure time, before anything is built or signed.
# get_target_property() leaves "<var>-NOTFOUND" when a property is unset, and CMake's if() treats
# any value ending in -NOTFOUND (and the empty string) as false, so one test covers both. An unset
# value here is not a cosmetic defect: it would leave plugin-main.mm composing a root `rm -rf`
# against a path this build does not own.
foreach(_qci_dal_property IN ITEMS OUTPUT_NAME BUNDLE_EXTENSION XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER)
  get_target_property(_qci_dal_value obs-dal-plugin ${_qci_dal_property})

  if(NOT _qci_dal_value)
    message(
      FATAL_ERROR
      "QCi DAL plug-in identity: obs-dal-plugin does not set ${_qci_dal_property}.\n"
      "plugins/mac-virtualcam/src/obs-plugin/plugin-main.mm deletes a bundle out of the shared "
      "system directory /Library/CoreMediaIO/Plug-Ins/DAL as root, and derives which bundle from "
      "this property. Set it in plugins/mac-virtualcam/src/dal-plugin/CMakeLists.txt. "
      "Refusing to configure."
    )
  endif()
endforeach()
unset(_qci_dal_property)
unset(_qci_dal_value)

get_target_property(_qci_dal_output_name obs-dal-plugin OUTPUT_NAME)
get_target_property(_qci_dal_bundle_extension obs-dal-plugin BUNDLE_EXTENSION)
get_target_property(_qci_dal_bundle_id obs-dal-plugin XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER)

# PREFIX is "" on obs-dal-plugin, so this is exactly the bundle directory CMake emits.
set(_qci_dal_bundle_name "${_qci_dal_output_name}.${_qci_dal_bundle_extension}")

# Upstream's own values. Shipping either would put this fork's install/uninstall back on top of
# the stock OBS plug-in in that shared directory.
#
# The name comparison is CASE-FOLDED because the volume it protects is case-insensitive:
# `diskutil info /` reports APFS, and on this machine a file created as
# "obs-mac-virtualcam.plugin" is reached by "OBS-Mac-VirtualCam.plugin". A case-varied OUTPUT_NAME
# passed a case-SENSITIVE STREQUAL and then resolved, on disk, to exactly stock OBS's path -- at
# which point plugin-main.mm's root `cp -R` writes over the stock bundle. (The root `rm -rf` stays
# safe either way: installed_dal_plugin_is_ours() reads the incumbent's CFBundleIdentifier and
# refuses. The write had no such gate.) Note the UUID guard above already case-folds via TOUPPER;
# this comparison did not, and it is the one guarding an elevated filesystem write.
string(TOUPPER "${_qci_dal_bundle_name}" _qci_dal_bundle_name_upper)
if(_qci_dal_bundle_name_upper STREQUAL "OBS-MAC-VIRTUALCAM.PLUGIN")
  message(
    FATAL_ERROR
    "QCi DAL plug-in identity: obs-dal-plugin would install as '${_qci_dal_bundle_name}', which is "
    "upstream OBS Studio's filename 'obs-mac-virtualcam.plugin' in the shared system directory "
    "/Library/CoreMediaIO/Plug-Ins/DAL -- case-insensitively, and that directory is on a "
    "case-insensitive APFS volume, so the two resolve to the SAME path. Installing or uninstalling "
    "this fork's camera would then overwrite or delete the stock OBS installation's plug-in, as "
    "root. Give obs-dal-plugin a distinct OUTPUT_NAME. Refusing to configure."
  )
endif()

string(TOUPPER "${_qci_dal_bundle_id}" _qci_dal_bundle_id_upper)
if(_qci_dal_bundle_id_upper STREQUAL "COM.OBSPROJECT.OBS-MAC-VIRTUALCAM")
  message(
    FATAL_ERROR
    "QCi DAL plug-in identity: obs-dal-plugin claims upstream OBS Studio's bundle identifier "
    "'com.obsproject.obs-mac-virtualcam'. That identifier is what plugin-main.mm checks before it "
    "deletes a bundle as root, so claiming it makes the stock OBS plug-in indistinguishable from "
    "this fork's. Give obs-dal-plugin a distinct PRODUCT_BUNDLE_IDENTIFIER. Refusing to configure."
  )
endif()

target_compile_definitions(
  mac-virtualcam
  PRIVATE
    OBS_DAL_PLUGIN_BUNDLE_NAME="${_qci_dal_bundle_name}"
    OBS_DAL_PLUGIN_BUNDLE_ID="${_qci_dal_bundle_id}"
)

unset(_qci_dal_output_name)
unset(_qci_dal_bundle_extension)
unset(_qci_dal_bundle_id)
unset(_qci_dal_bundle_id_upper)
unset(_qci_dal_bundle_name)
unset(_qci_dal_bundle_name_upper)
# --- end legacy DAL plug-in identity ---------------------------------------------------------
