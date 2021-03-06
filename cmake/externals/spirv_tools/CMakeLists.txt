set(EXTERNAL_NAME spirv_tools)
string(TOUPPER ${EXTERNAL_NAME} EXTERNAL_NAME_UPPER)

include(ExternalProject)
ExternalProject_Add(
  ${EXTERNAL_NAME}
  URL https://github.com/KhronosGroup/SPIRV-Tools/archive/v2018.4.zip
  URL_MD5 7a7c69cf6ff0318910b4bfbdf30bcfc9
  CONFIGURE_COMMAND CMAKE_ARGS  ${ANDROID_CMAKE_ARGS} -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE} -DSPIRV-Headers_SOURCE_DIR=${SPIRV_HEADERS_ROOT} -DCMAKE_INSTALL_PREFIX:PATH=<INSTALL_DIR>-$<CONFIG> ${EXTRA_CMAKE_FLAGS} 
  LOG_DOWNLOAD 1
  LOG_CONFIGURE 1
  LOG_BUILD 1
)

# Hide this external target (for ide users)
set_target_properties(${EXTERNAL_NAME} PROPERTIES FOLDER "hidden/externals")

ExternalProject_Get_Property(${EXTERNAL_NAME} INSTALL_DIR)
set(SUFFIXED_INSTALL_DIR "${INSTALL_DIR}-$<CONFIG>")

set(${EXTERNAL_NAME_UPPER}_INCLUDE_DIRS ${SUFFIXED_INSTALL_DIR}/include CACHE PATH "List of SPIRV-Tools include directories")

if (UNIX)
  set(LIB_PREFIX "lib")
  set(LIB_EXT "a")
elseif (WIN32)
  set(LIB_EXT "lib")
endif ()

list(APPEND ${EXTERNAL_NAME_UPPER}_LIBRARIES ${SUFFIXED_INSTALL_DIR}/lib/SPIRV-Tools-opt.${LIB_EXT})
list(APPEND ${EXTERNAL_NAME_UPPER}_LIBRARIES ${SUFFIXED_INSTALL_DIR}/lib/SPIRV-Tools-link.${LIB_EXT})
list(APPEND ${EXTERNAL_NAME_UPPER}_LIBRARIES ${SUFFIXED_INSTALL_DIR}/lib/SPIRV-Tools.${LIB_EXT})
set(${EXTERNAL_NAME_UPPER}_LIBRARIES ${${EXTERNAL_NAME_UPPER}_LIBRARIES} CACHE FILEPATH "Path to SPIRV-Tool libraries")
