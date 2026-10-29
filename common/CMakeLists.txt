find_package(Threads REQUIRED)

add_library(aqm_common STATIC
  src/log.c
  src/ring.c
  src/shm.c
  src/sig.c
)
target_include_directories(aqm_common PUBLIC include)
target_link_libraries(aqm_common PUBLIC Threads::Threads rt)
