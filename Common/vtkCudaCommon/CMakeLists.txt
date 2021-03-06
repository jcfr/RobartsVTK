PROJECT(vtkCudaCommon)

SET ( ${PROJECT_NAME}_SRCS
  CudaObject.cxx
  vtkCudaDeviceManager.cxx
  vtkCudaMemoryTexture.cxx
  vtkCuda2DTransferFunction.cxx
  vtkCudaFunctionObject.cxx
  vtkCudaFunctionPolygon.cxx
  vtkCudaFunctionPolygonReader.cxx
  vtkCudaFunctionPolygonWriter.cxx
  vtkCudaCommon.cxx
)

IF( MSVC OR ${CMAKE_GENERATOR} MATCHES "Xcode")
  SET ( ${PROJECT_NAME}_HDRS 
    CudaObject.h
    CudaCommon.h
    vtkCudaDeviceManager.h
    vtkCudaMemoryTexture.h
    vtkCuda2DTransferFunction.h
    vtkCudaFunctionObject.h
    vtkCudaFunctionPolygon.h
    vtkCudaFunctionPolygonReader.h
    vtkCudaFunctionPolygonWriter.h
    vtkCudaCommon.h
  )
ENDIF()

# Enable other dependencies to include the files necessary for cuda builds
SET(${PROJECT_NAME}_INCLUDE_DIRS 
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_BINARY_DIR} 
  CACHE INTERNAL "${PROJECT_NAME}_INCLUDE_DIRS")

# -----------------------------------------------------------------
# Build the library
ADD_LIBRARY(${PROJECT_NAME} ${${PROJECT_NAME}_HDRS} ${${PROJECT_NAME}_SRCS})
target_include_directories(${PROJECT_NAME} PUBLIC 
  ${${PROJECT_NAME}_INCLUDE_DIRS}
  ${VTK_INCLUDE_DIRS} 
  ${CUDA_INCLUDE_DIRS}
  )
target_link_libraries(${PROJECT_NAME} PUBLIC 
  ${CUDA_LIBRARIES}
  ${OPENGL_LIBRARIES}
  vtkCommonCore
  vtkRendering${VTK_RENDERING_BACKEND}
  vtkglew
  vtkRenderingFreeType
  vtkInteractionStyle
  )
GENERATE_EXPORT_DIRECTIVE_FILE(${PROJECT_NAME})