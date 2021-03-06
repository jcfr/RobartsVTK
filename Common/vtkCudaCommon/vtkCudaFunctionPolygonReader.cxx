#include "vtkCudaFunctionPolygonReader.h"
#include "vtkObjectFactory.h"
#include <iostream>

//----------------------------------------------------------------------------

vtkStandardNewMacro(vtkCudaFunctionPolygonReader);

//----------------------------------------------------------------------------

//----------------------------------------------------------------------------
vtkCudaFunctionPolygonReader::vtkCudaFunctionPolygonReader()
{
  this->fileNameSet = false;
  this->objects.clear();
}

//----------------------------------------------------------------------------
vtkCudaFunctionPolygonReader::~vtkCudaFunctionPolygonReader()
{
  this->Clear();
}

//----------------------------------------------------------------------------
void vtkCudaFunctionPolygonReader::SetFileName( const std::string& f )
{
  this->filename = f;
  this->fileNameSet = true;
}

//----------------------------------------------------------------------------
vtkCudaFunctionPolygon* vtkCudaFunctionPolygonReader::GetOutput( unsigned int n )
{
  if( n >= this->objects.size() || n < 0 )
  {
    vtkErrorMacro("Invalid index");
    return 0;
  }
  for( std::list<vtkCudaFunctionPolygon*>::iterator it = this->objects.begin(); it != this->objects.end(); it++)
  {
    if( n == 0 )
    {
      return *it;
    }
    n--;
  }
  vtkErrorMacro("Invalid index");
  return 0;
}

//----------------------------------------------------------------------------
size_t vtkCudaFunctionPolygonReader::GetNumberOfOutputs( )
{
  return this->objects.size();
}

//----------------------------------------------------------------------------
void vtkCudaFunctionPolygonReader::Read()
{
  if( !this->fileNameSet )
  {
    vtkErrorMacro("Must set file name before reading");
    return;
  }

  //clear the old information
  this->Clear();

  //open the file stream
  this->file = new std::ifstream();
  this->file->open(this->filename.c_str(),std::ios_base::in);

  //read the objects
  int numObjects = 0;
  *(this->file) >> numObjects;
  for( int n = 0; n < numObjects; n++ )
  {
    vtkCudaFunctionPolygon* newObject = this->readTFPolygon();
    if( newObject == 0 )
    {
      break;
    }
    this->objects.push_back(newObject);
  }

  //close the file
  file->close();
  delete file;
}

//----------------------------------------------------------------------------
void vtkCudaFunctionPolygonReader::Clear()
{
  //unregister self from the objects
  size_t numObjects = this->objects.size();
  for( size_t n = 0; n < numObjects; n++ )
  {
    vtkCudaFunctionPolygon* oldObject = this->objects.front();
    this->objects.pop_front();
    oldObject->UnRegister(this);
  }

  //make sure the object pile is empty
  this->objects.clear();
}

//----------------------------------------------------------------------------
vtkCudaFunctionPolygon* vtkCudaFunctionPolygonReader::readTFPolygon()
{
  vtkCudaFunctionPolygon* e = vtkCudaFunctionPolygon::New();
  try
  {
    //load in the colour values
    float r,g,b,a;
    float ambient, diffuse, specular, specularPower;
    short identifier;
    *(this->file) >> r >> g >> b >> a;
    e->SetColour(r,g,b);
    e->SetOpacity(a);
    *(this->file) >> ambient >> diffuse >> specular >> specularPower;
    e->SetAmbient(ambient);
    e->SetDiffuse(diffuse);
    e->SetSpecular(specular);
    e->SetSpecularPower(specularPower);
    *(this->file) >> identifier;
    e->SetIdentifier( identifier );

    //load in the number of vertices
    int numVertices = 0;
    *(this->file) >> numVertices;

    //load in each vertex one by one
    for(int i = 0; i < numVertices; i++)
    {
      float intensity, gradient;
      *(this->file) >> intensity >> gradient;
      e->AddVertex(intensity, gradient);
    }

    return e;
  }
  catch( ... )
  {
    e->Delete();
    return 0;
  }
}