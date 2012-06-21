/** @file vtkCudaDeviceManager.h
 *
 *  @brief Header file defining a singleton class to manage cards and stream interleaving
 *
 *  @author John Stuart Haberl Baxter (Dr. Peter's Lab at Robarts Research Institute)
 *  @note First documented on June 12, 2012
 *
 */

#ifndef __VTKCUDADEVICEMANAGER_H__
#define __VTKCUDADEVICEMANAGER_H__

#include "vtkObject.h"
#include "vtkMutexLock.h"
#include "vector_types.h"
#include <map>

class vtkCudaObject;

class vtkCudaDeviceManager : public vtkObject
{
public:

	vtkTypeMacro( vtkCudaDeviceManager, vtkObject );

	static vtkCudaDeviceManager* Singleton();

	int GetNumberOfDevices();
	bool GetDevice(vtkCudaObject* caller, int device);
	bool ReturnDevice(vtkCudaObject* caller, int device);
	bool GetStream(vtkCudaObject* caller, cudaStream_t** stream, int device);
	bool ReturnStream(vtkCudaObject* caller, cudaStream_t* stream, int device);

	bool SynchronizeStream( cudaStream_t* stream );
	bool ReserveGPU( cudaStream_t* stream );
	
	int QueryDeviceForObject( vtkCudaObject* object );
	int QueryDeviceForStream( cudaStream_t* stream );

protected:

private:
	vtkCudaDeviceManager();
	~vtkCudaDeviceManager();
	vtkCudaDeviceManager operator=(const vtkCudaDeviceManager&); /**< not implemented */
	vtkCudaDeviceManager(const vtkCudaDeviceManager&); /**< not implemented */
	
	void DestroyEmptyStream( cudaStream_t* stream );

	std::map<cudaStream_t*,int> StreamToDeviceMap;
	std::multimap<vtkCudaObject*,int> ObjectToDeviceMap;
	std::multimap<cudaStream_t*, vtkCudaObject*> StreamToObjectMap;

	static vtkCudaDeviceManager* singletonManager;

	vtkMutexLock* regularLock;

};

#endif /* __VTKCUDADEVICEMANAGER_H__ */
