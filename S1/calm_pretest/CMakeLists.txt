cmake_minimum_required(VERSION 3.8)
project(calm_pretest)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(std_msgs REQUIRED)

add_executable(calm_pub src/calm_pub.cpp)
ament_target_dependencies(calm_pub rclcpp std_msgs)

add_executable(calm_sub src/calm_sub.cpp)
ament_target_dependencies(calm_sub rclcpp std_msgs)

add_executable(calm_automation src/calm_automation.cpp)

install(TARGETS
  calm_pub
  calm_sub
  calm_automation
  DESTINATION lib/${PROJECT_NAME}
)

if(BUILD_TESTING)
  find_package(ament_lint_auto REQUIRED)
  ament_lint_auto_find_test_dependencies()
endif()

ament_package()
