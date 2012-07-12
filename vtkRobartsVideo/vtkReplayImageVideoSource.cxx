/*=========================================================================

  File: vtkReplayImageVideoSource.cxx
  Author: Chris Wedlake <cwedlake@robarts.ca>

  Language: C++
  Description: 
     
  =========================================================================

  Copyright (c) Chris Wedlake, cwedlake@robarts.ca

  Use, modification and redistribution of the software, in source or
  binary forms, are permitted provided that the following terms and
  conditions are met:

  1) Redistribution of the source code, in verbatim or modified
  form, must retain the above copyright notice, this license,
  the following disclaimer, and any notices that refer to this
  license and/or the following disclaimer.  

  2) Redistribution in binary form must include the above copyright
  notice, a copy of this license and the following disclaimer
  in the documentation or with other materials provided with the
  distribution.

  3) Modified copies of the source code must be clearly marked as such,
  and must not be misrepresented as verbatim copies of the source code.

  THE COPYRIGHT HOLDERS AND/OR OTHER PARTIES PROVIDE THE SOFTWARE "AS IS"
  WITHOUT EXPRESSED OR IMPLIED WARRANTY INCLUDING, BUT NOT LIMITED TO,
  THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
  PURPOSE.  IN NO EVENT SHALL ANY COPYRIGHT HOLDER OR OTHER PARTY WHO MAY
  MODIFY AND/OR REDISTRIBUTE THE SOFTWARE UNDER THE TERMS OF THIS LICENSE
  BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL OR CONSEQUENTIAL DAMAGES
  (INCLUDING, BUT NOT LIMITED TO, LOSS OF DATA OR DATA BECOMING INACCURATE
  OR LOSS OF PROFIT OR BUSINESS INTERRUPTION) ARISING IN ANY WAY OUT OF
  THE USE OR INABILITY TO USE THE SOFTWARE, EVEN IF ADVISED OF THE
  POSSIBILITY OF SUCH DAMAGES.


  =========================================================================*/

#include "vtkReplayImageVideoSource.h"
#include "vtkTimerLog.h"
#include "vtkObjectFactory.h"
#include "vtkCriticalSection.h"
#include "vtkUnsignedCharArray.h"
#include "vtkMutexLock.h"
#include "vtkSmartPointer.h"

#include "vtkJPEGReader.h"
#include "vtkJPEGWriter.h"
#include "vtkPNGReader.h"
#include "vtkBMPReader.h"
#include "vtkTIFFReader.h"
#include "vtkImageData.h"
#include "vtkPointData.h"

#include <vtkstd/string> 
#include <string>
#include <algorithm>

// #include <windows.h>
// #include <tchar.h> 
#include <stdio.h>
//#include <strsafe.h>


#include <vtkDirectory.h>
#include <vtkSortFileNames.h>
#include <vtkStringArray.h>

#pragma comment(lib, "User32.lib")

vtkReplayImageVideoSource* vtkReplayImageVideoSource::New()
{
  // First try to create the object from the vtkObjectFactory
  vtkObject* ret = vtkObjectFactory::CreateInstance("vtkReplayImageVideoSource");
  if(ret)
    {
      return (vtkReplayImageVideoSource*)ret;
    }
  // If the factory was unable to create the object, then create it here.
  return new vtkReplayImageVideoSource;
}

//----------------------------------------------------------------------------
vtkReplayImageVideoSource::vtkReplayImageVideoSource()
{

  this->Initialized = 0;
  this->pauseFeed = 0;
  this->currentLength = 0;

  this->vtkVideoSource::SetOutputFormat(VTK_RGB);
  this->vtkVideoSource::SetFrameBufferSize( 54 );
  this->vtkVideoSource::SetFrameRate( 15.0f );
  this->SetFrameSize(720,480,1); 
  this->imageIndex=-1;
}

//----------------------------------------------------------------------------
vtkReplayImageVideoSource::~vtkReplayImageVideoSource()
{
  this->vtkReplayImageVideoSource::ReleaseSystemResources();
  for (unsigned int i = 0; i < this->loadedData.size(); i++) {
    this->loadedData[i]->Delete();
  }
  this->loadedData.clear();
}  

//----------------------------------------------------------------------------
void vtkReplayImageVideoSource::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os,indent); 
}

//----------------------------------------------------------------------------
void vtkReplayImageVideoSource::Initialize()
{
  if (this->Initialized) 
    {
      return;
    }
 

  // Initialization worked
  this->Initialized = 1;
  
  // Update frame buffer  to reflect any changes
  this->UpdateFrameBuffer();
}  

//----------------------------------------------------------------------------
void vtkReplayImageVideoSource::ReleaseSystemResources()
{
  this->Initialized = 0;
}

void vtkReplayImageVideoSource::InternalGrab()
{

  if (this->loadedData.size() == 0)
    {
      return;
    }



  // get a thread lock on the frame buffer
  this->FrameBufferMutex->Lock();

  if (this->AutoAdvance)
    {
      this->AdvanceFrameBuffer(1);
      if (this->FrameIndex + 1 < this->FrameBufferSize)
	{
	  this->FrameIndex++;
	}
    }


  int index = this->FrameBufferIndex % this->FrameBufferSize;
  while (index < 0)
    {
      index += this->FrameBufferSize;
    }

  this->imageIndex = ++this->imageIndex % this->loadedData.size();
  


  void *buffer = this->loadedData[this->imageIndex]->GetScalarPointer();

  unsigned char *ptr = reinterpret_cast<vtkUnsignedCharArray *>(this->FrameBuffer[index])->GetPointer(0);

  //int ImageSize = (this->FrameBufferExtent[1]-this->FrameBufferExtent[0])*(this->FrameBufferExtent[3]-this->FrameBufferExtent[2]);

  memcpy(ptr, buffer, this->NumberOfScalarComponents*(this->FrameSize[0]-1)*(this->FrameSize[1]-1));



  this->FrameBufferTimeStamps[index] = vtkTimerLog::GetUniversalTime();

  if (this->FrameCount++ == 0)
    {
      this->StartTimeStamp = this->FrameBufferTimeStamps[index];
    }

  this->Modified();

  this->FrameBufferMutex->Unlock();
}

//----------------------------------------------------------------------------
// platform-independent sleep function
static inline void vtkSleep(double duration)
{
  duration = duration; // avoid warnings
  // sleep according to OS preference
#ifdef _WIN32
  Sleep((int)(1000*duration));
#elif defined(__FreeBSD__) || defined(__linux__) || defined(sgi)
  struct timespec sleep_time, dummy;
  sleep_time.tv_sec = (int)duration;
  sleep_time.tv_nsec = (int)(1000000000*(duration-sleep_time.tv_sec));
  nanosleep(&sleep_time,&dummy);
#endif
}

//----------------------------------------------------------------------------
// Sleep until the specified absolute time has arrived.
// You must pass a handle to the current thread.  
// If '0' is returned, then the thread was aborted before or during the wait.
static int vtkThreadSleep(vtkMultiThreader::ThreadInfo *data, double time)
{
  // loop either until the time has arrived or until the thread is ended
  for (int i = 0;; i++)
    {
      double remaining = time - vtkTimerLog::GetUniversalTime();

      // check to see if we have reached the specified time
      if (remaining <= 0)
	{
	  if (i == 0)
	    {
	      vtkGenericWarningMacro("Dropped a video frame.");
	    }
	  return 1;
	}
      // check the ActiveFlag at least every 0.1 seconds
      if (remaining > 0.1)
	{
	  remaining = 0.1;
	}

      // check to see if we are being told to quit 
      data->ActiveFlagLock->Lock();
      int activeFlag = *(data->ActiveFlag);
      data->ActiveFlagLock->Unlock();

      if (activeFlag == 0)
	{
	  break;
	}

      vtkSleep(remaining);
    }

  return 0;
}

//----------------------------------------------------------------------------
// this function runs in an alternate thread to asyncronously grab frames
static void *vtkReplayImageVideoSourceRecordThread(vtkMultiThreader::ThreadInfo *data)
{
  vtkReplayImageVideoSource *self = (vtkReplayImageVideoSource *)(data->UserData);
  
  double startTime = vtkTimerLog::GetUniversalTime();
  double rate = self->GetFrameRate();
  int frame = 0;

  do
    {
      self->InternalGrab();
      frame++;
    }
  while (vtkThreadSleep(data, startTime + frame/rate));

  return NULL;
}

//----------------------------------------------------------------------------
// Set the source to grab frames continuously.
// You should override this as appropriate for your device.  
void vtkReplayImageVideoSource::Record()
{
  // We don't actually record data.
  return;
}

//----------------------------------------------------------------------------
// this function runs in an alternate thread to 'play the tape' at the
// specified frame rate.
static void *vtkReplayImageVideoSourcePlayThread(vtkMultiThreader::ThreadInfo *data)
{
  vtkVideoSource *self = (vtkVideoSource *)(data->UserData);
 
  double startTime = vtkTimerLog::GetUniversalTime();
  double rate = self->GetFrameRate();
  int frame = 0;

  do
    {
      self->Seek(1);
      frame++;
    }
  while (vtkThreadSleep(data, startTime + frame/rate));

  return NULL;
}
 
//----------------------------------------------------------------------------
// Set the source to play back recorded frames.
// You should override this as appropriate for your device.  
void vtkReplayImageVideoSource::Play()
{
  if (this->Recording)
    {
      this->Stop();
    }

  if (!this->Playing)
    {
      this->Initialize();

      this->Playing = 1;
      this->Modified();
      this->PlayerThreadId = 
	this->PlayerThreader->SpawnThread((vtkThreadFunctionType)\
					  &vtkReplayImageVideoSourcePlayThread,this);
    }
}

//----------------------------------------------------------------------------
// Stop continuous grabbing or playback.  You will have to override this
// if your class overrides Play() and Record()
void vtkReplayImageVideoSource::Stop()
{
  if (this->Playing || this->Recording)
    {
      this->PlayerThreader->TerminateThread(this->PlayerThreadId);
      this->PlayerThreadId = -1;
      this->Playing = 0;
      this->Recording = 0;
      this->Modified();
    }
} 

void vtkReplayImageVideoSource::Pause() {
  this->pauseFeed = 1;
}

void vtkReplayImageVideoSource::UnPause() {
  this->pauseFeed = 0;
}

void vtkReplayImageVideoSource::LoadFile(char * filename)
{

  std::cout << filename << std::endl;
  
  std::string str(filename);
  std::string ext = ".png";
  
  for(unsigned int i=0; i<str.length(); i++)
    {
      if(str[i] == '.')
	{
	  for(unsigned int j = i; j<str.length(); j++)
	    {
	      ext += str[j];
	    }
	  //return p = ext.c_str();
	  break;
	}
    }
  
  std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);


  ext = ".png";


  vtkImageData * data = vtkImageData::New();
  
  vtkSmartPointer<vtkImageReader2> reader;
  
  if (ext == ".jpg")
    {
      reader = vtkSmartPointer<vtkJPEGReader>::New();
    }
  else if (ext == ".png")
    {
      reader = vtkSmartPointer<vtkPNGReader>::New();
    }
  else if (ext == ".bmp")
    {
      reader = vtkSmartPointer<vtkBMPReader>::New();
		
    }
  else if (ext == ".tiff")
    {
      reader = vtkSmartPointer<vtkTIFFReader>::New();
    }
  else 
    {
      return;
    }

  if (reader->CanReadFile(filename)) {
    reader->SetFileName(filename);
    reader->Update();
    reader->Modified();
    reader->GetOutput()->Update();
  } else {
    cout << "can't read" << endl;
    return;
  }

  int extents[6];
  reader->GetOutput()->GetExtent(extents);
  // if (extents[1]-extents[0]+1 != this->FrameSize[0] ||
  // 	extents[3]-extents[2]+1 != this->FrameSize[1] ||
  // 	extents[5]-extents[4]+1 != this->FrameSize[2] )
  // {
  // 	vtkErrorMacro("Unable to open file as size doesn't match video source");
  // 	return;
  // }
  data->DeepCopy(reader->GetOutput());

  this->loadedData.push_back(data);


}


int vtkReplayImageVideoSource::LoadFolder(char * folder, char * filetype)
{

  char* fullPath = new char[1024];
  vtkDirectory *dir = vtkDirectory::New();
  char buf[1024];

  
  //dir->GetCurrentWorkingDirectory(buf,1024);
  //fullPath = strcpy (fullPath, buf);
  //fullPath = strcat (fullPath, "/");
  //fullPath = strcat (fullPath, folder);
  //fullPath = strcat (fullPath, "/");

  fullPath = strcpy( fullPath, folder );
  fullPath = strcat( fullPath, "/" );
  
  int hFind = dir->Open(fullPath);

  if(hFind != 1){
    return -1;
  }

  vtkSortFileNames *sort = vtkSortFileNames::New();
  sort->SetInputFileNames(dir->GetFiles());
  sort->SkipDirectoriesOn();
  sort->NumericSortOn();
  
  for(int i = 0; i < sort->GetFileNames()->GetNumberOfValues(); i++){
  
    char *file = new char[1024];
    file = strcpy(file, fullPath);
    file = strcat(file, sort->GetFileNames()->GetValue(i));
    //std::cout << file << std::endl;
  
    this->LoadFile(file);
  }

  return 0;
}

// int vtkReplayImageVideoSource::LoadFolder(char * folder, char * filetype)
// {

//   char* fullPath = new char[1024];
//   vtkDirectory *dir = vtkDirectory::New();
//   char buf[1024];
//   fullPath = strcpy (fullPath, dir->GetCurrentWorkingDirectory(buf,1024));
//   fullPath = strcat (fullPath, "/");
//   fullPath = strcat (fullPath, folder);
//   fullPath = strcat (fullPath, "/");

//   int hFind = dir->Open(fullPath);

//   std::cout << "DEBUG!" << hFind << "Folder: " << fullPath << std::endl;

//   if(hFind == 1){
//     return -1;
//   }

//   vtkSortFileNames *sort = vtkSortFileNames::New();
//   sort->SetInputFileNames(dir->GetFiles());
//   sort->SkipDirectoriesOn();
//   sort->NumericSortOn();
  
  


//   std::cout << "DEBUG!" << std::endl;

//   for(int i = 0; i < sort->GetFileNames()->GetNumberOfValues(); i++){
  

//     char* tmpPath = strcat(fullPath, sort->GetFileNames()->GetValue(i) );
  
//   //  this->LoadFile(fullFilePath);
//   }

// }


// int vtkReplayImageVideoSource::LoadFolder2(char * folder, char * filetype)
// {
// 	WIN32_FIND_DATA ffd;
// 	TCHAR szDir[MAX_PATH];
// 	TCHAR fullFilePath[MAX_PATH];
// 	HANDLE hFind = INVALID_HANDLE_VALUE;
//     DWORD dwError=0;

// 	StringCchCopy(szDir, MAX_PATH, folder);
// 	StringCchCat(szDir, MAX_PATH, TEXT("\\*"));

// 	hFind = FindFirstFile(szDir, &ffd);

// 	if (INVALID_HANDLE_VALUE == hFind) 
// 	{
// 		vtkWarningMacro("Error Opening Folder");
// 		return dwError;
// 	} 

// 	do
// 	{
// 		if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
// 		{
// 			continue; //skip directories
// 		}
// 		else
// 		{
// 			StringCchCopy(fullFilePath, MAX_PATH, folder);
// 			StringCchCat(fullFilePath, MAX_PATH, ffd.cFileName);
// 			this->LoadFile(fullFilePath);
// 		}
// 	}
// 	while (FindNextFile(hFind, &ffd) != 0);

// 	FindClose(hFind);
// 	return dwError;
// }



void vtkReplayImageVideoSource::Clear()
{

}

void vtkReplayImageVideoSource::SetClipRegion(int x0, int x1, int y0, int y1, 
					      int z0, int z1)
{
  return;
}
