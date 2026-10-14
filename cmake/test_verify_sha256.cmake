# cmake -DFILE=... -DEXPECTED=... -DLABEL=... -P cmake/test_verify_sha256.cmake
if(NOT DEFINED FILE OR NOT DEFINED EXPECTED OR NOT DEFINED LABEL)
    message(FATAL_ERROR "FILE, EXPECTED, and LABEL are required")
endif()
include("${CMAKE_CURRENT_LIST_DIR}/VerifyDownload.cmake")
arbiter_require_sha256("${FILE}" "${EXPECTED}" "${LABEL}")
