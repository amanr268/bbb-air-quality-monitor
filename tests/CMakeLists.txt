foreach(t smoke ring shm aqi store orient ssd1306 pms5003 button avg json modbus)
  add_executable(test_${t} test_${t}.c)
  target_link_libraries(test_${t} PRIVATE aqm_common)
  add_test(NAME ${t} COMMAND test_${t})
endforeach()

# ENS160 / AHT21 drivers against simulated chips (i2c-dev calls wrapped)
add_executable(test_i2c_sensors test_i2c_sensors.c)
target_link_libraries(test_i2c_sensors PRIVATE aqm_common m
  "-Wl,--wrap=read,--wrap=write,--wrap=ioctl,--wrap=__ioctl_time64")
add_test(NAME i2c_sensors COMMAND test_i2c_sensors)

# the real `sense` program on simulated ENS160/AHT21 chips (manual integration runs)
add_executable(aqm_sense_fakei2c ../apps/sense/main.c fake_i2c.c)
target_link_libraries(aqm_sense_fakei2c PRIVATE aqm_common
  "-Wl,--wrap=read,--wrap=write,--wrap=ioctl,--wrap=__ioctl_time64")
