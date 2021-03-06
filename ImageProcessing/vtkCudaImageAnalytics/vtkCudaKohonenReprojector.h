#ifndef __VTKCUDAKOHONENREPROJECTOR_H__
#define __VTKCUDAKOHONENREPROJECTOR_H__

#include "vtkCudaImageAnalyticsExport.h"

#include "CudaObject.h"
#include "CUDA_kohonenreprojector.h"
#include "vtkImageAlgorithm.h"

class vtkAlgorithmOutput;
class vtkImageCast;
class vtkImageData;
class vtkInformation;
class vtkInformationVector;
class vtkTransform;

class vtkCudaImageAnalyticsExport vtkCudaKohonenReprojector : public vtkImageAlgorithm, public CudaObject
{
public:
  vtkTypeMacro(vtkCudaKohonenReprojector, vtkImageAlgorithm);

  static vtkCudaKohonenReprojector* New();

  // Description:
  // If the subclass does not define an Execute method, then the task
  // will be broken up, multiple threads will be spawned, and each thread
  // will call this method. It is public so that the thread functions
  // can call this method.
  virtual int RequestData(vtkInformation* request,
                          vtkInformationVector** inputVector,
                          vtkInformationVector* outputVector);
  virtual int RequestInformation(vtkInformation* request,
                                 vtkInformationVector** inputVector,
                                 vtkInformationVector* outputVector);
  virtual int RequestUpdateExtent(vtkInformation* request,
                                  vtkInformationVector** inputVector,
                                  vtkInformationVector* outputVector);

protected:
  vtkCudaKohonenReprojector();
  virtual ~vtkCudaKohonenReprojector();

  virtual void Reinitialize(bool withData = false);
  virtual void Deinitialize(bool withData = false);

private:
  vtkCudaKohonenReprojector operator=(const vtkCudaKohonenReprojector&);
  vtkCudaKohonenReprojector(const vtkCudaKohonenReprojector&);

  Kohonen_Reprojection_Information info;
};

#endif