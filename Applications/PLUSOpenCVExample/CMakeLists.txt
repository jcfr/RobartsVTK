FIND_PACKAGE(PlusLib REQUIRED)
INCLUDE(${PlusLib_USE_FILE})

IF(NOT PLUS_USE_MMF_VIDEO)
  MESSAGE(STATUS "PLUSOpenCVExample requires PLUS_USE_MMF_VIDEO and it is not available.")
  return()
ENDIF()

PROJECT(PLUSOpenCVExample)

FIND_PACKAGE(OpenCV REQUIRED)

SET(${PROJECT_NAME}_APPTYPE MACOSX_BUNDLE)
IF(WIN32)
  SET(${PROJECT_NAME}_APPTYPE WIN32)
ENDIF()

ADD_EXECUTABLE( ${PROJECT_NAME} ${${PROJECT_NAME}_APPTYPE}
  main.cpp
  PLUSOpenCVExampleMainWindow.h
  PLUSOpenCVExampleMainWindow.cpp
  PLUSOpenCVExampleMainWindow.ui
  ${PLUSLIB_WIDGETS_QRC}
)

target_include_directories(${PROJECT_NAME} PRIVATE
  ${CMAKE_CURRENT_BINARY_DIR} 
  ${CMAKE_CURRENT_SOURCE_DIR}
  )
TARGET_LINK_LIBRARIES( ${PROJECT_NAME} PRIVATE
  opencv_core
  opencv_imgproc
  opencv_videoio
  opencv_highgui
  opencv_features2d
  opencv_calib3d
  PlusWidgets
  vtkPlusCommon
  vtkPlusDataCollection
  vtkCommonCore
  vtkRenderingCore
  vtkGUISupportQt
  vtkRenderingGL2PS${VTK_RENDERING_BACKEND}
  Qt5::Widgets 
  Qt5::Core 
  Qt5::Gui 
)