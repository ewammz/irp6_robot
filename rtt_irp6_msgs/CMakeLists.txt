cmake_minimum_required(VERSION 2.8.3)
project(rtt_irp6_msgs)

find_package(catkin REQUIRED COMPONENTS rtt_roscomm)

ros_generate_rtt_typekit(irp6_msgs)
ros_generate_rtt_service_proxies(irp6_msgs)

orocos_generate_package(
  DEPENDS irp6_msgs
  DEPENDS_TARGETS rtt_roscomm 
)
