cmake_minimum_required(VERSION 3.8)
project(livox_laser_simulation)

## Compile as C++11, supported in ROS Kinetic and newer
#add_compile_options(-std=c++17)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

#find_package(catkin REQUIRED COMPONENTS
#  roscpp
#  tf
#)

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(rclcpp_components REQUIRED)
find_package(gazebo REQUIRED)
find_package(gazebo_ros REQUIRED)
find_package(sensor_msgs REQUIRED)
find_package(tf2_ros REQUIRED)
find_package(ament_index_cpp REQUIRED)
include_directories(${GAZEBO_INCLUDE_DIRS})

#To solve the error which gazebo does not handle the include file well, we must add this line.
include_directories(
  /usr/include/gazebo-11/gazebo
  include
)

link_directories(${GAZEBO_LIBRARY_DIRS})

add_library(livox_laser_simulation SHARED
  src/livox_points_plugin.cpp
  src/livox_ode_multiray_shape.cpp
)
target_link_libraries(livox_laser_simulation
  RayPlugin
)
target_include_directories(livox_laser_simulation
  PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
ament_target_dependencies(livox_laser_simulation
  rclcpp
  rclcpp_components
  sensor_msgs
  tf2_ros
  gazebo_ros
  ament_index_cpp
)
ament_export_targets(export_livox_laser_simulation)
install(TARGETS
  livox_laser_simulation
  EXPORT export_livox_laser_simulation
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
  INCLUDES DESTINATION include
)

install (DIRECTORY
  urdf
  meshes
  worlds
  launch
  scan_mode
  rviz
  DESTINATION share/${PROJECT_NAME}
)

ament_package()

#This is designed for whose proto installed in system is not 2.6. We can install the version of proto in local dir
#include_directories(/home/lfc/proto/include/)
#link_directories(/home/lfc/proto/lib/)

#target_link_libraries(livox_laser_simulation libprotobuf.so.9)
