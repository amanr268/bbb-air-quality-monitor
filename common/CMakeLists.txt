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
  src/ssd1306.c
  src/font_data.c
  src/adxl345.c
  src/orient.c
  src/screen.c
  src/pms5003.c
  src/notify.c
  src/button.c
  src/led.c
  src/avg.c
  src/json.c
  src/mqtt.c
  src/aht21.c
  src/ens160.c
)
target_include_directories(aqm_common PUBLIC include)
target_link_libraries(aqm_common PUBLIC Threads::Threads rt m SQLite::SQLite3)

# MQTT is optional: without libmosquitto-dev the publisher compiles to no-ops
find_path(MOSQUITTO_INCLUDE_DIR mosquitto.h)
find_library(MOSQUITTO_LIBRARY mosquitto)
if(MOSQUITTO_INCLUDE_DIR AND MOSQUITTO_LIBRARY)
  target_compile_definitions(aqm_common PUBLIC AQM_HAVE_MQTT=1)
  target_include_directories(aqm_common PRIVATE ${MOSQUITTO_INCLUDE_DIR})
  target_link_libraries(aqm_common PUBLIC ${MOSQUITTO_LIBRARY})
  message(STATUS "MQTT: libmosquitto found")
else()
  message(WARNING "MQTT disabled: install libmosquitto-dev")
endif()
