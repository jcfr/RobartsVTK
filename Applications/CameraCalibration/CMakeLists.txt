FIND_PACKAGE(PlusLib REQUIRED)
INCLUDE(${PlusLib_USE_FILE})

IF(NOT PLUS_USE_MMF_VIDEO)
  MESSAGE(STATUS "CameraCalibration requires PLUS_USE_MMF_VIDEO and it is not available.")
  return()
ENDIF()

PROJECT( CameraCalibration )

SET(CameraCalib_APPTYPE MACOSX_BUNDLE)
IF(WIN32)
  SET(CameraCalib_APPTYPE WIN32)
ENDIF()

ADD_EXECUTABLE( ${PROJECT_NAME} ${CameraCalib_APPTYPE}
  main.cpp
  CameraCalibrationMainWindow.h
  CameraCalibrationMainWindow.cpp
  CameraCalibrationMainWidget.h
  CameraCalibrationMainWidget.cpp
  CameraCalibrationMainWidget.ui
  OpenCVCameraCapture.h
  OpenCVCameraCapture.cpp
  QScaledView.h
  QScaledView.cpp
  Threads/QComputeThread.h
  Threads/QComputeThread.cpp
  Threads/QCaptureThread.h
  Threads/QCaptureThread.cpp
  ${PLUSLIB_WIDGETS_QRC}
)
IF(WIN32)
  # From PLUS, use MSMF capture library to query webcams
  TARGET_LINK_LIBRARIES( ${PROJECT_NAME} MfVideoCaptureLibrary )
ENDIF()

target_include_directories(${PROJECT_NAME} PUBLIC 
  ${CMAKE_CURRENT_BINARY_DIR} 
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/Threads
  ${OpenCV_INCLUDE_DIRS}
  )
TARGET_LINK_LIBRARIES( ${PROJECT_NAME}
  opencv_core
  opencv_imgproc
  opencv_videoio
  opencv_highgui
  opencv_features2d
  opencv_calib3d
  vtkPlusDataCollection
  vtkCommonCore
  vtkRenderingCore
  vtkGUISupportQt
  PlusWidgets
  vtkRenderingGL2PS${VTK_RENDERING_BACKEND}
  ${OPENGL_LIBRARIES}
  Qt5::Concurrent
  Qt5::Widgets 
  Qt5::Core 
  Qt5::Gui 
  Qt5::Xml
)
