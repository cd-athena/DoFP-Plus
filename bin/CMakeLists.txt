# Copyright (c) 2017 - 2021 LiteSpeed Technologies Inc.  See LICENSE.
LIST(APPEND LIBS ${EVENT_LIB})

IF(MSVC)
    FIND_LIBRARY(PCRE_LIB pcre)
    IF(PCRE_LIB)
        MESSAGE(STATUS "Found pcre: ${PCRE_LIB}")
        LIST(APPEND LIBS ${PCRE_LIB})
    ELSE()
        MESSAGE(STATUS "pcre not found: http_server won't work")
    ENDIF()
    FIND_LIBRARY(PCREPOSIX_LIB pcreposix)
    IF(PCREPOSIX_LIB)
        MESSAGE(STATUS "Found pcreposix: ${PCREPOSIX_LIB}")
        LIST(APPEND LIBS ${PCREPOSIX_LIB})
    ELSE()
        MESSAGE(STATUS "pcreposix not found: http_server won't work")
    ENDIF()
    LIST(APPEND LIBS ws2_32)
    LIST(APPEND LIBS iphlpapi)
ENDIF()

IF(MSVC)
    SET(GETOPT_C ../wincompat/getopt.c)
ENDIF()
add_executable(http_server_dofp http_server_dofp.c prog.c test_common.c test_cert.c ${GETOPT_C})
add_executable(http_server http_server.c prog.c test_common.c test_cert.c ${GETOPT_C})
IF(NOT MSVC)   #   TODO: port MD5 server and client to Windows
add_executable(md5_server md5_server.c prog.c test_common.c test_cert.c ${GETOPT_C})
add_executable(md5_client md5_client.c prog.c test_common.c test_cert.c ${GETOPT_C})
ENDIF()
add_executable(echo_server echo_server.c prog.c test_common.c test_cert.c ${GETOPT_C})
add_executable(echo_client echo_client.c prog.c test_common.c test_cert.c ${GETOPT_C})
add_executable(duck_server duck_server.c prog.c test_common.c test_cert.c ${GETOPT_C})
add_executable(duck_client duck_client.c prog.c test_common.c test_cert.c ${GETOPT_C})
add_executable(perf_client perf_client.c prog.c test_common.c test_cert.c ${GETOPT_C})
add_executable(perf_server perf_server.c prog.c test_common.c test_cert.c ${GETOPT_C})


IF (NOT MSVC)

add_executable(http_client_priority
    http_client_priority.c
    prog.c
    test_common.c
    test_cert.c
)

add_executable(http_client_dofp
    http_client_dofp.c
    abr_max_j.c
    abr_max_min_j.c
    abr_max_min_j_norm.c
    abr_quality_instability.c
    prog.c
    test_common.c
    test_cert.c
)

add_executable(http_client_dofp_python
    http_client_dofp_python.c
    abr_max_j.c
    abr_max_min_j.c
    abr_max_min_j_norm.c
    prog.c
    test_common.c
    test_cert.c
)

#MSVC
ELSE()

add_executable(http_client_priority
    http_client_priority.c
    prog.c
    test_common.c
    test_cert.c
    ../wincompat/getopt.c
    ../wincompat/getopt1.c
)

add_executable(http_client_dofp
    http_client_dofp.c
    abr_max_j.c
    abr_max_min_j.c
    abr_max_min_j_norm.c
    abr_quality_instability.c
    prog.c
    test_common.c
    test_cert.c
    ../wincompat/getopt.c
    ../wincompat/getopt1.c
)

add_executable(http_client_dofp_python
    http_client_dofp_python.c
    abr_max_j.c
    abr_max_min_j.c
    abr_max_min_j_norm.c
    prog.c
    test_common.c
    test_cert.c
	../wincompat/getopt.c
    ../wincompat/getopt1.c
)

ENDIF()

TARGET_LINK_LIBRARIES(http_client_dofp ${LIBS})
TARGET_LINK_LIBRARIES(http_client_dofp_python ${LIBS})
TARGET_LINK_LIBRARIES(http_client_priority ${LIBS})
TARGET_LINK_LIBRARIES(http_server_dofp ${LIBS})
TARGET_LINK_LIBRARIES(http_server ${LIBS})
IF(NOT MSVC)
TARGET_LINK_LIBRARIES(md5_server  ${LIBS})
TARGET_LINK_LIBRARIES(md5_client  ${LIBS})
ENDIF()
TARGET_LINK_LIBRARIES(echo_server ${LIBS})
TARGET_LINK_LIBRARIES(echo_client ${LIBS})
TARGET_LINK_LIBRARIES(duck_server ${LIBS})
TARGET_LINK_LIBRARIES(duck_client ${LIBS})
TARGET_LINK_LIBRARIES(perf_client ${LIBS})
TARGET_LINK_LIBRARIES(perf_server ${LIBS})


INCLUDE(CheckFunctionExists)
CHECK_FUNCTION_EXISTS(sendmmsg HAVE_SENDMMSG)
CHECK_FUNCTION_EXISTS(recvmmsg HAVE_RECVMMSG)
CHECK_FUNCTION_EXISTS(open_memstream HAVE_OPEN_MEMSTREAM)


INCLUDE(CheckSymbolExists)

CHECK_SYMBOL_EXISTS(
    IP_MTU_DISCOVER
    "netinet/in.h"
    HAVE_IP_MTU_DISCOVER
)

CHECK_SYMBOL_EXISTS(
    IP_DONTFRAG
    "netinet/in.h"
    HAVE_IP_DONTFRAG
)

CHECK_SYMBOL_EXISTS(
    preadv
    "sys/uio.h"
    HAVE_PREADV
)

INCLUDE(CheckIncludeFiles)

IF (MSVC AND PCRE_LIB)
FIND_PATH(EVENT_INCLUDE_DIR NAMES pcreposix.h)
IF (EVENT_INCLUDE_DIR)
    MESSAGE(STATUS "found pcreposix.h")
    SET(HAVE_REGEX 1)
ELSE()
    MESSAGE(FATAL_ERROR "event2/event.h was not found")
ENDIF()
ELSE()
CHECK_INCLUDE_FILES(regex.h HAVE_REGEX)
ENDIF()

CONFIGURE_FILE(${CMAKE_CURRENT_SOURCE_DIR}/test_config.h.in ${CMAKE_CURRENT_SOURCE_DIR}/test_config.h)
