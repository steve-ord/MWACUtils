
cmake_minimum_required(VERSION 2.6)
project(get_delays)
add_executable(get_delays get_delays)
find_package(CFITSIO)
find_package(SLALIB)
find_package(PSRFITS_UTILS)
find_package(FFTW3 COMPONENTS single)
message("SLALIB ${SLALIB_INCLUDE_DIR}")
include_directories(${CMAKE_INSTALL_PREFIX}/include ${PROJECT_SOURCE_DIR}/../mwac_utils/ ${POSTGRES_INCLUDE_DIR}
${SLALIB_INCLUDE_DIR} ${CFITSIO_INCLUDE_DIR} ${PSRFITS_UTILS_INCLUDE_DIR})
target_link_libraries(get_delays -L${CMAKE_INSTALL_PREFIX}/lib MWAC_Utils )
target_link_libraries(get_delays ${PSRFITS_UTILS_LIBRARY} )
target_link_libraries(get_delays ${SLALIB_LIBRARY} ${CFITSIO_LIBRARY} ${FFTW3F_LIBRARY} -lm)


install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/get_delays DESTINATION bin)

