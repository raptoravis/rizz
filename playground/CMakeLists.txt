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
	file(GLOB SRC_FILES ${proj_name}/*.*)

    if (BUNDLE AND NOT ANDROID) 
        add_executable(${proj_name} ${EXEC_TYPE} ${SRC_FILES})
    else()
        add_library(${proj_name} SHARED ${SRC_FILES})
    endif()

    if (NOT ANDROID AND NOT IOS)
        target_compile_definitions(${proj_name} PRIVATE COMPILE_DEFINITIONS -DEXAMPLES_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
    endif()
    
    target_link_libraries(${proj_name} PRIVATE sx)
    add_dependencies(${proj_name} rizz)

	#set_target_properties(${proj_name} PROPERTIES CXX_STANDARD 17)
	#set_target_properties(${proj_name} PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../bin/Debug")
	set_target_properties(${proj_name} PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "$(OutDir)")
	set_target_properties(${proj_name} PROPERTIES VS_DEBUGGER_COMMAND "rizz.exe")
	set_target_properties(${proj_name} PROPERTIES VS_DEBUGGER_COMMAND_ARGUMENTS "--run ${proj_name}.dll")

	# compile default shaders
    if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${proj_name}/${source_file}.vert AND 
        EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${proj_name}/${source_file}.frag)
        set(shaders ${proj_name}/${source_file}.vert 
                    ${proj_name}/${source_file}.frag)

        set_source_files_properties(${shaders} PROPERTIES GLSLCC_OUTPUT_DIRECTORY "assets/shaders")

		if (CMAKE_BUILD_TYPE AND CMAKE_BUILD_TYPE STREQUAL "Debug")
			set_source_files_properties(${shaders} PROPERTIES GLSLCC_COMPILE_FLAGS "--debug-bin")
		endif()

        glslcc_target_compile_shaders_sgs(${proj_name} "${shaders}")
    endif()

    if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${proj_name}/${source_file}.comp)
        set(shaders ${proj_name}/${source_file}.comp)

        set_source_files_properties(${shaders} PROPERTIES GLSLCC_OUTPUT_DIRECTORY "assets/shaders")

		if (CMAKE_BUILD_TYPE AND CMAKE_BUILD_TYPE STREQUAL "Debug")
			set_source_files_properties(${shaders} PROPERTIES GLSLCC_COMPILE_FLAGS "--debug-bin")
		endif()

		set(output_compute "${proj_name}-compute")
        glslcc_target_compile_shaders_sgs("${proj_name}" "${shaders}")
    endif()
endfunction()

set(others_example_projects sandbox pg-ecs pg-tf pg-ecsminigame pg-cs)
#set(others_example_projects pg-cs)

if (BUILD_EXAMPLES AND NOT BUNDLE)
	foreach (other_example_project ${others_example_projects})
		rizz__add_example_others(${other_example_project})
		set_target_properties(${other_example_project} PROPERTIES FOLDER playground)
		target_include_directories(${other_example_project} PRIVATE "../others/entt/src" "../others/taskflow/include" "../3rdparty")
	endforeach()
endif()
