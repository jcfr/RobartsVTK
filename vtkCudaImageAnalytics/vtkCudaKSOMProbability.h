#ifndef __VTKCUDAKSOMPROBABILITY_H__
#define __VTKCUDAKSOMPROBABILITY_H__

#include "CUDA_KSOMProbability.h"
#include "vtkAlgorithm.h"
#include "vtkImageData.h"
#include "vtkImageCast.h"
#include "vtkTransform.h"
#include "vtkCudaObject.h"
#include "vtkInformation.h"
#include "vtkInformationVector.h"
#include "vtkAlgorithmOutput.h"

class vtkCudaKSOMProbability : public vtkImageAlgorithm, public vtkCudaObject
{
public:
	vtkTypeMacro( vtkCudaKSOMProbability, vtkImageAlgorithm );

	static vtkCudaKSOMProbability *New();

	void SetScale( double s );
	double GetScale() {return this->Scale;}
	
	void SetImageInput(vtkImageData* in);
	void SetMapInput(vtkImageData* in);
	void SetMaskInput(vtkImageData* in);
	void SetProbabilityInput(vtkImageData* in, int index);

	// Description:
	// If the subclass does not define an Execute method, then the task
	// will be broken up, multiple threads will be spawned, and each thread
	// will call this method. It is public so that the thread functions
	// can call this method.
	virtual int RequestData(vtkInformation *request, 
							 vtkInformationVector **inputVector, 
							 vtkInformationVector *outputVector);
	virtual int RequestInformation( vtkInformation* request,
							 vtkInformationVector** inputVector,
							 vtkInformationVector* outputVector);
	virtual int RequestUpdateExtent( vtkInformation* request,
							 vtkInformationVector** inputVector,
							 vtkInformationVector* outputVector);
	virtual int FillInputPortInformation(int i, vtkInformation* info);

protected:
	vtkCudaKSOMProbability();
	virtual ~vtkCudaKSOMProbability();
	
	void Reinitialize(int withData);
	void Deinitialize(int withData);

private:
	vtkCudaKSOMProbability operator=(const vtkCudaKSOMProbability&){}
	vtkCudaKSOMProbability(const vtkCudaKSOMProbability&){}

	double Scale;

	Kohonen_Probability_Information info;
};

#endif