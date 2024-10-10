cmake_minimum_required(VERSION 3.22.1)
project(container-extractor VERSION 0.0.1 LANGUAGES NONE)

add_subdirectory(interpreter)

install(TARGETS   interpreter DESTINATION ./fs/bin)
install(DIRECTORY extractor/. DESTINATION . USE_SOURCE_PERMISSIONS)

set(CPACK_GENERATOR "TGZ")
set(CPACK_PACKAGE_DIRECTORY "packages")
include(CPack)