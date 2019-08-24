cmake_minimum_required(VERSION 3.1)

# Default to C11
if(NOT CMAKE_C_STANDARD)
	set(CMAKE_C_STANDARD 11)
endif()
  
# Default to C++14
if(NOT CMAKE_CXX_STANDARD)
	set(CMAKE_CXX_STANDARD 17)
endif()


function(rizz__add_example_others proj_name)
	#set(CMAKE_CXX_STANDARD 17)
    string(SUBSTRING ${proj_name} 3 -1 source_file)
	file(GLOB SRC_FILES ${proj_name}/*.c*)

    if (BUNDLE AND NOT ANDROID) 
        add_executable(${proj_name} ${EXEC_TYPE} ${SRC_FILES})
    else()
        add_library(${proj_name} SHARED ${SRC_FILES})
    endif()

    if (NOT ANDROID AND NOT IOS)
        target_compile_definitions(${proj_name} PRIVATE COMPILE_DEFINITIONS -DEXAMPLES_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/other_examples")
    endif()
    
    target_link_libraries(${proj_name} PRIVATE sx)
    add_dependencies(${proj_name} rizz)

	#set_target_properties(${proj_name} PROPERTIES CXX_STANDARD 17)
	#set_target_properties(${proj_name} PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../bin/Debug")
	set_target_properties(${proj_name} PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "$(OutDir)")
	set_target_properties(${proj_name} PROPERTIES VS_DEBUGGER_COMMAND "rizz.exe")
	set_target_properties(${proj_name} PROPERTIES VS_DEBUGGER_COMMAND_ARGUMENTS "--run ${proj_name}.dll")

endfunction()

set(others_example_projects 11-ecs 12-tf)

if (BUILD_EXAMPLES AND NOT BUNDLE)
	foreach (other_example_project ${others_example_projects})
		rizz__add_example_others(${other_example_project})
		set_target_properties(${other_example_project} PROPERTIES FOLDER other_examples)
		target_include_directories(${other_example_project} PRIVATE "../others/entt/src" "../others/taskflow/include")
	endforeach()
endif()
