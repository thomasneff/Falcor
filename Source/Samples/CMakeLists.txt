add_subdirectory(CudaInterop)
add_subdirectory(HelloDXR)
add_subdirectory(ModelViewer)
add_subdirectory(ShaderToy)
add_subdirectory(Visualization2D)

if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/SplitAO3D")
add_subdirectory(SplitAO3D)
endif()
