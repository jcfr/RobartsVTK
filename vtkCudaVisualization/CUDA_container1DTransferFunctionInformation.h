/** @file CUDA_container1DTransferFunctionInformation.h
 *
 *  @brief File for the volume information holding structure used for volume ray casting
 *
 *  @author John Stuart Haberl Baxter (Dr. Peter's Lab at Robarts Research Institute)
 *  @note First documented on March 27, 2011
 *
 *  @note This is primarily an internal file used by the vtkCudaVolumeInformationHandler and CUDA_renderAlgo to store and communicate constants
 *
 */

#ifndef __CUDA1DTRANSFERFUNCTIONINFORMATION_H__
#define __CUDA1DTRANSFERFUNCTIONINFORMATION_H__

#include "vector_types.h"

/** @brief A stucture located on the CUDA hardware that holds all the information required about the volume being renderered.
 *
 */
typedef struct __align__(16) {
	// The scale and shift to transform intensity and gradient to indices in the transfer functions
	float			intensityLow;			/**< Minimum intensity of the image */
	float			intensityMultiplier;	/**< Scale factor to normalize intensities to between 0 and 1 */
	unsigned int	functionSize;			/**< The size of the lookup table */

	//opague memory back for the transfer function
	cudaArray* alphaTransferArray1D;
	cudaArray* colorRTransferArray1D;
	cudaArray* colorGTransferArray1D;
	cudaArray* colorBTransferArray1D;

} cuda1DTransferFunctionInformation;

#endif
