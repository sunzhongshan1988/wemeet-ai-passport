cmake_minimum_required(VERSION 3.16)

# 固件版本:显示在设备"设备"页,并随每次云端同步上报(PC 设备管理页据此判断是否需要升级)。
set(PROJECT_VER "0.1.0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# 工程名即固件产物名;tools/、CI 与发布流程都按此名查找镜像,改名需同步修改它们。
project(FoloToy-AI-Passport)
