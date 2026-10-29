foreach(app sense capture convert compare report)
  add_executable(aqm_${app} ${app}/main.c)
  target_link_libraries(aqm_${app} PRIVATE aqm_common)
endforeach()
