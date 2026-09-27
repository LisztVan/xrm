find_program(XRM_REWRITE_PYTHON python3)
find_program(XRM_REWRITE_TMUX tmux)
if(XRM_REWRITE_PYTHON AND XRM_REWRITE_TMUX)
  add_test(NAME xrm_component_rewrite_terminal
    COMMAND ${XRM_REWRITE_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/terminal_regression.py
      --build ${CMAKE_BINARY_DIR}
      --output ${CMAKE_BINARY_DIR}/terminal-rewrite-evidence)
  set_tests_properties(xrm_component_rewrite_terminal PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
endif()
