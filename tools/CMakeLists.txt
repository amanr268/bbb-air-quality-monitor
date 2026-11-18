add_executable(pi_demo pi_demo.c)
target_link_libraries(pi_demo PRIVATE aqm_common)

add_executable(oled_anim oled_anim.c)
target_link_libraries(oled_anim PRIVATE aqm_common m)

add_executable(autorotate autorotate.c)
target_link_libraries(autorotate PRIVATE aqm_common)

add_executable(accel_view accel_view.c)
target_link_libraries(accel_view PRIVATE aqm_common)

add_executable(accel_cal accel_cal.c)
target_link_libraries(accel_cal PRIVATE aqm_common)

add_executable(pms_read pms_read.c)
target_link_libraries(pms_read PRIVATE aqm_common)

add_executable(oled_off oled_off.c)
