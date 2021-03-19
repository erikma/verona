include(${CMAKE_CURRENT_LIST_DIR}/common.cmake)

set(ACTUAL_DUMP ${CMAKE_CURRENT_BINARY_DIR}/${TEST_NAME})

PrepareTest(VERONAC_FLAGS EXPECTED_DUMP ACTUAL_DUMP)

CheckStatus(
  COMMAND ${VERONA_MLIR} ${TEST_FILE} -o ${ACTUAL_DUMP}/mlir.txt
  EXPECTED_STATUS 0)

if(EXPECTED_DUMP)
  CheckDump(${EXPECTED_DUMP} ${ACTUAL_DUMP})
endif()