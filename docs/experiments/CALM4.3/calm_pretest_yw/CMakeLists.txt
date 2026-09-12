cmake_minimum_required(VERSION 3.8)
project(calm_pretest_yw)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(rmw REQUIRED)
find_package(std_msgs REQUIRED)
find_package(sensor_msgs REQUIRED)

add_executable(dds_pub src/pub_original.cpp)
ament_target_dependencies(dds_pub rclcpp rmw std_msgs sensor_msgs)

add_executable(dds_sub src/sub_original.cpp)
ament_target_dependencies(dds_sub rclcpp rmw std_msgs sensor_msgs)

install(TARGETS
  dds_pub
  dds_sub
  DESTINATION lib/${PROJECT_NAME}
)

install(PROGRAMS
  scripts/automation.py
  DESTINATION lib/${PROJECT_NAME}
  RENAME automation
)

install(PROGRAMS
  scripts/calm_loopback_optimize.py
  scripts/ddsopt_loopback_automation.py
  scripts/calm_hotspot_optimize.py
  scripts/calm_ethernet_optimize.py
  scripts/calm_gain_tune.py
  scripts/dds_wireless_optimizer.py
  scripts/run_fastdds_calm42_validation.sh
  DESTINATION lib/${PROJECT_NAME}
)

install(DIRECTORY
  config
  DESTINATION share/${PROJECT_NAME}
)

ament_package()
