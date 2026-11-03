find_package(Threads REQUIRED)
find_package(SQLite3 REQUIRED)

add_library(aqm_common STATIC
  src/log.c
  src/ring.c
  src/shm.c
  src/sig.c
  src/proc.c
  src/aqi.c
  src/alarm.c
  src/calib.c
  src/store.c
)
target_include_directories(aqm_common PUBLIC include)
target_link_libraries(aqm_common PUBLIC Threads::Threads rt SQLite::SQLite3)
