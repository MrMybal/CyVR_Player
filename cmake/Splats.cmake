set(CYVR_SPLATKIT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.deps/splatkit/source" CACHE PATH "Pinned MIT SplatKit source")
set(splat_cpp "${CYVR_SPLATKIT_ROOT}/packages/splatkit-android/src/main/cpp")
set(splat_core "${CYVR_SPLATKIT_ROOT}/packages/splat-core")
set(splat_engine "${CYVR_SPLATKIT_ROOT}/packages/splatkit-engine")
set(splat_shader_headers)
foreach(shader raster/splat.vert raster/projected.vert raster/splat.frag cull/visibility.comp cull/prepare_indirect.comp selection/lod_selection.comp sort/radix_prepare.comp sort/radix_histogram.comp sort/radix_scan.comp sort/radix_scatter.comp)
    get_filename_component(name "${shader}" NAME)
    string(REPLACE "." "_" symbol "${name}")
    set(spv "${CMAKE_CURRENT_BINARY_DIR}/splat-shaders/${name}.spv")
    set(header "${CMAKE_CURRENT_BINARY_DIR}/splat-shaders/shaders/${symbol}.h")
    add_custom_command(OUTPUT "${header}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/splat-shaders/shaders"
        COMMAND "${CYVR_APP_GLSLC}" --target-env=vulkan1.1 -O "-I${splat_cpp}" "${splat_cpp}/rendering/vulkan/${shader}" -o "${spv}"
        COMMAND "${CMAKE_COMMAND}" "-DINPUT=${spv}" "-DOUTPUT=${header}" "-DSYMBOL=${symbol}" -P "${splat_cpp}/cmake/embed.cmake"
        DEPENDS "${splat_cpp}/rendering/vulkan/${shader}" VERBATIM)
    list(APPEND splat_shader_headers "${header}")
endforeach()
add_library(cyvr_splat_backend STATIC
    "${splat_cpp}/rendering/vulkan/device/Vma.cpp"
    "${splat_cpp}/rendering/vulkan/device/VulkanBuffer.cpp"
    "${splat_cpp}/rendering/vulkan/world/VulkanWorld.cpp"
    "${splat_cpp}/rendering/vulkan/raster/VulkanSplatPipeline.cpp"
    "${splat_cpp}/rendering/vulkan/cull/VulkanVisibilityPass.cpp"
    "${splat_cpp}/rendering/vulkan/selection/VulkanLodSelection.cpp"
    "${splat_cpp}/rendering/vulkan/sort/VulkanRadixSort.cpp"
    "${splat_cpp}/rendering/vulkan/frame/VulkanFrameCompute.cpp"
    "${splat_cpp}/rendering/vulkan/frame/VulkanCameraUniforms.cpp"
    "${splat_engine}/src/rendering/GpuLayout.cpp" "${splat_engine}/src/Log.cpp"
    "${splat_core}/src/formats/FrameTransform.cpp" "${splat_core}/src/math/Covariance.cpp"
    "${splat_core}/src/lod/LodTree.cpp" "${splat_core}/src/lod/LodSelection.cpp" "${splat_core}/src/math/SymmetricEigen.cpp"
    "${splat_core}/src/lod/LodFile.cpp"
    ${splat_shader_headers})
set_target_properties(cyvr_splat_backend PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(cyvr_splat_backend PUBLIC cxx_std_17)
target_compile_definitions(cyvr_splat_backend PRIVATE _USE_MATH_DEFINES)
target_include_directories(cyvr_splat_backend PUBLIC include/cyvr/player/splat_backend "${splat_cpp}" "${splat_core}/include" "${splat_engine}/include" "${CMAKE_CURRENT_SOURCE_DIR}/.deps/vma/source/include")
target_include_directories(cyvr_splat_backend PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/splat-shaders")
target_compile_options(cyvr_splat_backend PRIVATE -Wno-nullability-completeness)
if(ANDROID)
    target_link_libraries(cyvr_splat_backend PUBLIC vulkan log)
else()
    # The Windows host loads Vulkan dynamically. These backend files use the C API;
    # generate a local import library from the Khronos declarations for the test host.
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/vulkan_import.py" "${CYVR_UI_ROOT}/third_party/vulkan/include/vulkan/vulkan_core.h" "${CMAKE_CURRENT_BINARY_DIR}/vulkan-import" "${CMAKE_CXX_COMPILER}" RESULT_VARIABLE import_result)
    if(NOT import_result EQUAL 0)
        message(FATAL_ERROR "Cannot create the Vulkan host import library")
    endif()
    target_include_directories(cyvr_splat_backend PUBLIC "${CYVR_UI_ROOT}/third_party/vulkan/include")
    target_link_libraries(cyvr_splat_backend PUBLIC "${CMAKE_CURRENT_BINARY_DIR}/vulkan-import/vulkan-1.lib")
endif()
