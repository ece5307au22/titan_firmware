#!/bin/bash

cd "$(dirname "$0")"
docker pull microros/micro_ros_static_library_builder:galactic
docker run -it --rm -v $(pwd):/project microros/micro_ros_static_library_builder:galactic
