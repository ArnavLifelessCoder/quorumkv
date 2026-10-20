foreach(test IN LISTS quorumkv_test_names)
  if(test MATCHES "Fuzz\\.")
    # The watchdog handles a post-PASS exit hang within ten seconds.
    set_tests_properties("${test}" PROPERTIES TIMEOUT 5430)
  endif()
endforeach()
