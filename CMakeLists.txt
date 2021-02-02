cmake_minimum_required (VERSION 2.8)

project(main)



set(SOURCE_FILES ikcp.c kcpsession.cpp kcpserver.cpp test.cpp)

add_executable(main test.cpp ${SOURCE_FILES})
