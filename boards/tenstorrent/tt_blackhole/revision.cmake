set(BOARD_REVISIONS "p100" "p100a" "p150a" "p300")
if(NOT DEFINED BOARD_REVISION)
  set(BOARD_REVISION "p100")
else()
  if(NOT BOARD_REVISION IN_LIST BOARD_REVISIONS)
    message(FATAL_ERROR "${BOARD_REVISION} is not a valid revision for tt_blackhole. Accepted revisions: ${BOARD_REVISIONS}")
  endif()
endif()
