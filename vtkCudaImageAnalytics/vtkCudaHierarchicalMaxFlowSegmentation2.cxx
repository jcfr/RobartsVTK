#include "vtkCudaHierarchicalMaxFlowSegmentation2.h"
#include "vtkCudaHierarchicalMaxFlowSegmentation2Task.h"
#include "vtkObjectFactory.h"
#include "vtkStreamingDemandDrivenPipeline.h"
#include "vtkTreeDFSIterator.h"

#include <assert.h>
#include <math.h>
#include <float.h>
#include <limits.h>

#include <set>
#include <list>
#include <vector>

#include "CUDA_hierarchicalmaxflow.h"
#include "vtkCudaDeviceManager.h"
#include "vtkCudaObject.h"

#define SQR(X) X*X

vtkStandardNewMacro(vtkCudaHierarchicalMaxFlowSegmentation2);

vtkCudaHierarchicalMaxFlowSegmentation2::vtkCudaHierarchicalMaxFlowSegmentation2(){

	//set algorithm mathematical parameters to defaults
	this->MaxGPUUsage = 0.75;
	this->ReportRate = 100;

	//give default GPU selection
	this->GPUsUsed.insert(0);

}

vtkCudaHierarchicalMaxFlowSegmentation2::~vtkCudaHierarchicalMaxFlowSegmentation2(){
	this->GPUsUsed.clear();
	this->MaxGPUUsageNonDefault.clear();
}

//------------------------------------------------------------//

void vtkCudaHierarchicalMaxFlowSegmentation2::AddDevice(int GPU){
	if( GPU >= 0 && GPU < vtkCudaDeviceManager::Singleton()->GetNumberOfDevices() )
		this->GPUsUsed.insert(GPU);
}

void vtkCudaHierarchicalMaxFlowSegmentation2::RemoveDevice(int GPU){
	if( this->GPUsUsed.find(GPU) != this->GPUsUsed.end() )
		this->GPUsUsed.erase(this->GPUsUsed.find(GPU));
}

bool vtkCudaHierarchicalMaxFlowSegmentation2::HasDevice(int GPU){
	return (this->GPUsUsed.find(GPU) != this->GPUsUsed.end());
}
void vtkCudaHierarchicalMaxFlowSegmentation2::ClearDevices(){
	this->GPUsUsed.clear();
}

void vtkCudaHierarchicalMaxFlowSegmentation2::SetMaxGPUUsage(double usage, int device){
	if( usage < 0.0 ) usage = 0.0;
	else if( usage > 1.0 ) usage = 1.0;
	if( device >= 0 && device < vtkCudaDeviceManager::Singleton()->GetNumberOfDevices() )
		this->MaxGPUUsageNonDefault[device] = usage;
}

double vtkCudaHierarchicalMaxFlowSegmentation2::GetMaxGPUUsage(int device){
	if( this->MaxGPUUsageNonDefault.find(device) != this->MaxGPUUsageNonDefault.end() )
		return this->MaxGPUUsageNonDefault[device];
	return this->MaxGPUUsage;
}

void vtkCudaHierarchicalMaxFlowSegmentation2::ClearMaxGPUUsage(){
	this->MaxGPUUsageNonDefault.clear();
}

//-----------------------------------------------------------------------------------------------//
//-----------------------------------------------------------------------------------------------//

int vtkCudaHierarchicalMaxFlowSegmentation2::InitializeAlgorithm(){

	//if verbose, print progress
	if( this->Debug ) vtkDebugMacro(<<"Building workers.");
    for(std::set<int>::iterator gpuIterator = GPUsUsed.begin(); gpuIterator != GPUsUsed.end(); gpuIterator++){
		double usage = this->MaxGPUUsage;
		if( this->MaxGPUUsageNonDefault.find(*gpuIterator) != this->MaxGPUUsageNonDefault.end() )
			usage = this->MaxGPUUsageNonDefault[*gpuIterator];
        Worker* newWorker = new Worker( *gpuIterator, usage, this );
        this->Workers.insert( newWorker );
        if(newWorker->NumBuffers < 8){
            vtkErrorMacro(<<"Could not allocate sufficient GPU buffers.");
            for(std::set<Task*>::iterator taskIterator = FinishedTasks.begin(); taskIterator != FinishedTasks.end(); taskIterator++)
                delete *taskIterator;
            FinishedTasks.clear();
            for(std::set<Worker*>::iterator workerIterator = Workers.begin(); workerIterator != Workers.end(); workerIterator++)
                delete *workerIterator;
            Workers.clear();
            while( CPUBuffersAcquired.size() > 0 ){
                float* tempBuffer = CPUBuffersAcquired.front();
                delete[] tempBuffer;
                CPUBuffersAcquired.pop_front();
            }
            return -1;
        }
    }

	//if verbose, print progress
	if( this->Debug ) vtkDebugMacro(<<"Find priority structures.");

	//create LIFO priority queue (priority stack) data structure
    FigureOutBufferPriorities( this->Hierarchy->GetRoot() );
	
	//add tasks in for the normal iterations (done first for dependancy reasons)
	if( this->Debug ) vtkDebugMacro(<<"Creating tasks for normal iterations.");
	NumTasksGoingToHappen = 0;
	if( this->NumberOfIterations > 0 ){
		CreateClearWorkingBufferTasks(this->Hierarchy->GetRoot());
		CreateUpdateSpatialFlowsTasks(this->Hierarchy->GetRoot());
		CreateApplySinkPotentialBranchTasks(this->Hierarchy->GetRoot());
		CreateApplySinkPotentialLeafTasks(this->Hierarchy->GetRoot());
		CreateApplySourcePotentialTask(this->Hierarchy->GetRoot());
		CreateDivideOutWorkingBufferTask(this->Hierarchy->GetRoot());
		CreateUpdateLabelsTask(this->Hierarchy->GetRoot());
		AddIterationTaskDependencies(this->Hierarchy->GetRoot());
	}
	
	//add tasks in for the initialization (done second for dependancy reasons)
	if( this->Debug ) vtkDebugMacro(<<"Creating tasks for initialization.");
	if( this->NumberOfIterations > 0 ) CreateInitializeAllSpatialFlowsToZeroTasks(this->Hierarchy->GetRoot());
	CreateInitializeLeafSinkFlowsToCapTasks(this->Hierarchy->GetRoot());
	CreateCopyMinimalLeafSinkFlowsTasks(this->Hierarchy->GetRoot());
	CreateFindInitialLabellingAndSumTasks(this->Hierarchy->GetRoot());
	CreateClearSourceWorkingBufferTask();
	CreateDivideOutLabelsTasks(this->Hierarchy->GetRoot());
	if( this->NumberOfIterations > 0 ) CreatePropogateLabelsTasks(this->Hierarchy->GetRoot());

	if( this->Debug ) vtkDebugMacro(<<"Number of tasks to be run: " << NumTasksGoingToHappen);

	return 1;
}

int vtkCudaHierarchicalMaxFlowSegmentation2::RunAlgorithm(){
	//if verbose, print progress
	if( this->Debug ) vtkDebugMacro(<<"Running tasks");
	NumMemCpies = 0;
	NumKernelRuns = 0;
	int NumTasksDone = 0;
	while( this->CurrentTasks.size() > 0 ){

		int MinWeight = INT_MAX;
		int MinUnConflictWeight = INT_MAX;
		std::vector<Task*> MinTasks;
		std::vector<Task*> MinUnConflictTasks;
		std::vector<Worker*> MinWorkers;
		std::vector<Worker*> MinUnConflictWorkers;
		for(std::set<Task*>::iterator taskIt = CurrentTasks.begin(); MinWeight > 0 && taskIt != CurrentTasks.end(); taskIt++){
			if( !(*taskIt)->CanDo() ) continue;
			
			//find if the task is conflicted and put in appropriate contest
			Worker* possibleWorker = 0;
			int conflictWeight = (*taskIt)->Conflicted(&possibleWorker);
			if( conflictWeight ){
				if( conflictWeight < MinUnConflictWeight ){
					MinUnConflictWeight = conflictWeight;
					MinUnConflictTasks.clear();
					MinUnConflictTasks.push_back(*taskIt);
					MinUnConflictWorkers.clear();
					MinUnConflictWorkers.push_back(possibleWorker);
				}else if(conflictWeight == MinUnConflictWeight){
					MinUnConflictTasks.push_back(*taskIt);
					MinUnConflictWorkers.push_back(possibleWorker);
				}
				continue;
			}
			
			if( possibleWorker ){ //only one worker can do this task
				int weight = (*taskIt)->CalcWeight(possibleWorker);
				if( weight < MinWeight ){
					MinWeight = weight;
					MinTasks.clear();
					MinTasks.push_back(*taskIt);
					MinWorkers.clear();
					MinWorkers.push_back(possibleWorker);
				}else if( weight == MinWeight ){
					MinTasks.push_back(*taskIt);
					MinWorkers.push_back(possibleWorker);
				}
			}else{ //all workers have a chance, find the emptiest one
				for(std::set<Worker*>::iterator workerIt = Workers.begin(); workerIt != Workers.end(); workerIt++){
					int weight = (*taskIt)->CalcWeight(*workerIt);
					if( weight < MinWeight ){
						MinWeight = weight;
						MinTasks.clear();
						MinTasks.push_back(*taskIt);
						MinWorkers.clear();
						MinWorkers.push_back(*workerIt);
					}else if( weight == MinWeight ){
						MinTasks.push_back(*taskIt);
						MinWorkers.push_back(*workerIt);
					}
				}
			}
		}
		
		//figure out if it is cheaper to run a conflicted or non-conflicted task
		if( MinUnConflictWeight >= MinWeight ){
			int taskIdx = rand() % MinTasks.size();
			MinTasks[taskIdx]->Perform(MinWorkers[taskIdx]);
		}else{
			int taskIdx = rand() % MinUnConflictTasks.size();
			MinUnConflictTasks[taskIdx]->UnConflict(MinUnConflictWorkers[taskIdx]);
			MinUnConflictTasks[taskIdx]->Perform(MinUnConflictWorkers[taskIdx]);
		}

		//if there are conflicts
		//update progress
		NumTasksDone++;
		if( this->Debug && NumTasksDone % ReportRate == 0 ){
			for(std::set<Worker*>::iterator workerIt = Workers.begin(); workerIt != Workers.end(); workerIt++)
				(*workerIt)->CallSyncThreads();
			vtkDebugMacro(<< "Finished " << NumTasksDone << " with " << NumMemCpies << " memory transfers.");
		}
		
	}
	if( this->Debug ) vtkDebugMacro(<< "Finished all " << NumTasksDone << " tasks with a total of " << NumMemCpies << " memory transfers.");
	assert( BlockedTasks.size() == 0 );
	
	//remove tasks
	if( this->Debug ) vtkDebugMacro(<< "Deallocating tasks" );
	for(std::set<Task*>::iterator taskIterator = FinishedTasks.begin(); taskIterator != FinishedTasks.end(); taskIterator++)
		delete *taskIterator;
	FinishedTasks.clear();

	//remove workers
	if( this->Debug ) vtkDebugMacro(<< "Deallocating workers" );
	for(std::set<Worker*>::iterator workerIterator = Workers.begin(); workerIterator != Workers.end(); workerIterator++)
		delete *workerIterator;
	Workers.clear();

	//clear old lists
	this->CurrentTasks.clear();
	this->BlockedTasks.clear();
	this->CPUInUse.clear();
	this->CPU2PriorityMap.clear();
	this->ReadOnly.clear();
	this->NoCopyBack.clear();
	this->ClearWorkingBufferTasks.clear();
	this->UpdateSpatialFlowsTasks.clear();
	this->ApplySinkPotentialBranchTasks.clear();
	this->ApplySinkPotentialLeafTasks.clear();
	this->ApplySourcePotentialTasks.clear();
	this->DivideOutWorkingBufferTasks.clear();
	this->UpdateLabelsTasks.clear();
	this->InitializeLeafSinkFlowsTasks.clear();
	this->MinimizeLeafSinkFlowsTasks.clear();
	this->PropogateLeafSinkFlowsTasks.clear();
	this->InitialLabellingSumTasks.clear();
	this->CorrectLabellingTasks.clear();
	this->PropogateLabellingTasks.clear();
	this->LastBufferUse.clear();
	this->Overwritten.clear();

	return 1;
}

void vtkCudaHierarchicalMaxFlowSegmentation2::FigureOutBufferPriorities( vtkIdType currNode ){
	
	//Propogate down the tree
	int NumKids = this->Hierarchy->GetNumberOfChildren(currNode);
	for(int kid = 0; kid < NumKids; kid++)
		FigureOutBufferPriorities( this->Hierarchy->GetChild(currNode,kid) );

	//if we are the root, figure out the buffers
	if( this->Hierarchy->GetRoot() == currNode ){
		this->CPU2PriorityMap.insert(std::pair<float*,int>(sourceFlowBuffer,NumKids+2));
		this->CPU2PriorityMap.insert(std::pair<float*,int>(sourceWorkingBuffer,NumKids+3));

	//if we are a leaf, handle separately
	}else if( NumKids == 0 ){
		int Number = LeafMap[currNode];
		this->CPU2PriorityMap.insert(std::pair<float*,int>(leafDivBuffers[Number],3));
		this->CPU2PriorityMap.insert(std::pair<float*,int>(leafFlowXBuffers[Number],2));
		this->CPU2PriorityMap.insert(std::pair<float*,int>(leafFlowYBuffers[Number],2));
		this->CPU2PriorityMap.insert(std::pair<float*,int>(leafFlowZBuffers[Number],2));
		this->CPU2PriorityMap.insert(std::pair<float*,int>(leafSinkBuffers[Number],3));
		this->CPU2PriorityMap.insert(std::pair<float*,int>(leafDataTermBuffers[Number],1));
		this->CPU2PriorityMap.insert(std::pair<float*,int>(leafLabelBuffers[Number],3));
		if( leafSmoothnessTermBuffers[Number] )
			this->CPU2PriorityMap[leafSmoothnessTermBuffers[Number]]++;

	//else, we are a branch
	}else{
		int Number = BranchMap[currNode];
		this->CPU2PriorityMap.insert(std::pair<float*,int>(branchDivBuffers[Number],3));
		this->CPU2PriorityMap.insert(std::pair<float*,int>(branchFlowXBuffers[Number],2));
		this->CPU2PriorityMap.insert(std::pair<float*,int>(branchFlowYBuffers[Number],2));
		this->CPU2PriorityMap.insert(std::pair<float*,int>(branchFlowZBuffers[Number],2));
		this->CPU2PriorityMap.insert(std::pair<float*,int>(branchSinkBuffers[Number],NumKids+4));
		this->CPU2PriorityMap.insert(std::pair<float*,int>(branchLabelBuffers[Number],3));
		this->CPU2PriorityMap.insert(std::pair<float*,int>(branchWorkingBuffers[Number],NumKids+3));
		if( branchSmoothnessTermBuffers[Number] )
			this->CPU2PriorityMap[branchSmoothnessTermBuffers[Number]]++;
	}
}

void vtkCudaHierarchicalMaxFlowSegmentation2::ReturnBufferGPU2CPU(Worker* caller, float* CPUBuffer, float* GPUBuffer, cudaStream_t* stream){
	if( !CPUBuffer ) return; 
	if( ReadOnly.find(CPUBuffer) != ReadOnly.end() ) return;
	if( Overwritten[CPUBuffer] == 0 ) return;
	Overwritten[CPUBuffer] = 0;
	caller->ReserveGPU();
	LastBufferUse[CPUBuffer] = caller;
	if( NoCopyBack.find(CPUBuffer) != NoCopyBack.end() ) return;
	CUDA_CopyBufferToCPU( GPUBuffer, CPUBuffer, VolumeSize, stream);
	NumMemCpies++;
}

void vtkCudaHierarchicalMaxFlowSegmentation2::MoveBufferCPU2GPU(Worker* caller, float* CPUBuffer, float* GPUBuffer, cudaStream_t* stream){
	if( !CPUBuffer ) return; 
	caller->ReserveGPU();
	if( LastBufferUse[CPUBuffer] ) LastBufferUse[CPUBuffer]->CallSyncThreads();
	LastBufferUse[CPUBuffer] = 0;
	if( NoCopyBack.find(CPUBuffer) != NoCopyBack.end() ) return;
	CUDA_CopyBufferToGPU( GPUBuffer, CPUBuffer, VolumeSize, stream);
	NumMemCpies++;
}

//------------------------------------------------------------//
//------------------------------------------------------------//

void vtkCudaHierarchicalMaxFlowSegmentation2::CreateClearWorkingBufferTasks(vtkIdType currNode){
	int NumKids = this->Hierarchy->GetNumberOfChildren(currNode);
	for(int i = 0; i < NumKids; i++)
		CreateClearWorkingBufferTasks( this->Hierarchy->GetChild(currNode,i) );
	if( NumKids == 0 ) return;
	
	//create the new task
	Task* newTask = new Task(this,0,1,this->NumberOfIterations,currNode,Task::ClearWorkingBufferTask);
	this->ClearWorkingBufferTasks[currNode] = newTask;
	
	//modify the task accordingly
	if(currNode == this->Hierarchy->GetRoot()){
		newTask->Active = -NumLeaves; //wait for source buffer to finish being used
		newTask->AddBuffer(sourceWorkingBuffer);
	}else{
		NoCopyBack.insert(branchWorkingBuffers[BranchMap[currNode]]);
		newTask->AddBuffer(branchWorkingBuffers[BranchMap[currNode]]);
	}
}

void vtkCudaHierarchicalMaxFlowSegmentation2::CreateUpdateSpatialFlowsTasks(vtkIdType currNode){
	int NumKids = this->Hierarchy->GetNumberOfChildren(currNode);
	for(int i = 0; i < NumKids; i++)
		CreateUpdateSpatialFlowsTasks( this->Hierarchy->GetChild(currNode,i) );
	if( currNode == this->Hierarchy->GetRoot() ) return;
	
	//create the new task
	//initial Active is -(6+NumKids) if branch since 4 clear buffers, 2 init flow happen in the initialization and NumKids number of label clears
	//initial Active is -7 if leaf since 4 clear buffers, 2 init flow happen in the initialization and NumKids number of label clears
	Task* newTask = new Task(this,-(6+(NumKids?NumKids:1)),1,this->NumberOfIterations,currNode,Task::UpdateSpatialFlowsTask);
	this->UpdateSpatialFlowsTasks[currNode] = newTask;
	if(NumKids != 0){
		newTask->AddBuffer(branchSinkBuffers[BranchMap[currNode]]);
		newTask->AddBuffer(branchIncBuffers[BranchMap[currNode]]);
		newTask->AddBuffer(branchDivBuffers[BranchMap[currNode]]);
		newTask->AddBuffer(branchLabelBuffers[BranchMap[currNode]]);
		newTask->AddBuffer(branchFlowXBuffers[BranchMap[currNode]]);
        newTask->AddBuffer(branchFlowYBuffers[BranchMap[currNode]]);
        newTask->AddBuffer(branchFlowZBuffers[BranchMap[currNode]]);
        newTask->AddBuffer(branchSmoothnessTermBuffers[BranchMap[currNode]]);
	}else{
		newTask->AddBuffer(leafSinkBuffers[LeafMap[currNode]]);
		newTask->AddBuffer(leafIncBuffers[LeafMap[currNode]]);
		newTask->AddBuffer(leafDivBuffers[LeafMap[currNode]]);
		newTask->AddBuffer(leafLabelBuffers[LeafMap[currNode]]);
		newTask->AddBuffer(leafFlowXBuffers[LeafMap[currNode]]);
        newTask->AddBuffer(leafFlowYBuffers[LeafMap[currNode]]);
        newTask->AddBuffer(leafFlowZBuffers[LeafMap[currNode]]);
        newTask->AddBuffer(leafSmoothnessTermBuffers[LeafMap[currNode]]);
	}

}

void vtkCudaHierarchicalMaxFlowSegmentation2::CreateApplySinkPotentialBranchTasks(vtkIdType currNode){
	int NumKids = this->Hierarchy->GetNumberOfChildren(currNode);
	for(int i = 0; i < NumKids; i++)
		CreateApplySinkPotentialBranchTasks( this->Hierarchy->GetChild(currNode,i) );
	if( NumKids == 0 ) return;
	
	//create the new task
	if(currNode != this->Hierarchy->GetRoot()){
		Task* newTask = new Task(this,-2,2,this->NumberOfIterations,currNode,Task::ApplySinkPotentialBranchTask);
		this->ApplySinkPotentialBranchTasks[currNode] = newTask;
		newTask->AddBuffer(branchWorkingBuffers[BranchMap[currNode]]);
		newTask->AddBuffer(branchIncBuffers[BranchMap[currNode]]);
		newTask->AddBuffer(branchDivBuffers[BranchMap[currNode]]);
		newTask->AddBuffer(branchLabelBuffers[BranchMap[currNode]]);
	}
}

void vtkCudaHierarchicalMaxFlowSegmentation2::CreateApplySinkPotentialLeafTasks(vtkIdType currNode){
	int NumKids = this->Hierarchy->GetNumberOfChildren(currNode);
	for(int i = 0; i < NumKids; i++)
		CreateApplySinkPotentialLeafTasks( this->Hierarchy->GetChild(currNode,i) );
	if( NumKids != 0 ) return;
	
	//create the new task
	Task* newTask = new Task(this,-1,1,this->NumberOfIterations,currNode,Task::ApplySinkPotentialLeafTask);
	this->ApplySinkPotentialLeafTasks[currNode] = newTask;
	newTask->AddBuffer(leafSinkBuffers[LeafMap[currNode]]);
	newTask->AddBuffer(leafIncBuffers[LeafMap[currNode]]);
	newTask->AddBuffer(leafDivBuffers[LeafMap[currNode]]);
	newTask->AddBuffer(leafLabelBuffers[LeafMap[currNode]]);
	newTask->AddBuffer(leafDataTermBuffers[LeafMap[currNode]]);
}

void vtkCudaHierarchicalMaxFlowSegmentation2::CreateDivideOutWorkingBufferTask(vtkIdType currNode){
	int NumKids = this->Hierarchy->GetNumberOfChildren(currNode);
	for(int i = 0; i < NumKids; i++)
		CreateDivideOutWorkingBufferTask( this->Hierarchy->GetChild(currNode,i) );
	if( NumKids == 0 ) return;
	
	//create the new task
	Task* newTask = new Task(this,-(NumKids+1),NumKids+1,this->NumberOfIterations,currNode,Task::DivideOutWorkingBufferTask);
	this->DivideOutWorkingBufferTasks[currNode] = newTask;
	if( currNode != this->Hierarchy->GetRoot() ){
		newTask->AddBuffer(branchWorkingBuffers[BranchMap[currNode]]);
		newTask->AddBuffer(branchSinkBuffers[BranchMap[currNode]]);
	}else{
		newTask->AddBuffer(sourceWorkingBuffer);
		newTask->AddBuffer(sourceFlowBuffer);
	}
}

void vtkCudaHierarchicalMaxFlowSegmentation2::CreateApplySourcePotentialTask(vtkIdType currNode){
	int NumKids = this->Hierarchy->GetNumberOfChildren(currNode);
	for(int i = 0; i < NumKids; i++)
		CreateApplySourcePotentialTask( this->Hierarchy->GetChild(currNode,i) );
	if( currNode == this->Hierarchy->GetRoot() ) return;
	vtkIdType parentNode = this->Hierarchy->GetParent(currNode);

	//find appropriate working buffer
	float* workingBuffer = 0;
	if( parentNode == this->Hierarchy->GetRoot() ) workingBuffer = sourceWorkingBuffer;
	else workingBuffer = branchWorkingBuffers[BranchMap[parentNode]];

	//create the new task
	Task* newTask = new Task(this,-2,2,this->NumberOfIterations,currNode,Task::ApplySourcePotentialTask);
	this->ApplySourcePotentialTasks[currNode] = newTask;
	newTask->AddBuffer(workingBuffer);
	if(NumKids != 0){
		newTask->AddBuffer(branchSinkBuffers[BranchMap[currNode]]);
		newTask->AddBuffer(branchDivBuffers[BranchMap[currNode]]);
		newTask->AddBuffer(branchLabelBuffers[BranchMap[currNode]]);
	}else{
		newTask->AddBuffer(leafSinkBuffers[LeafMap[currNode]]);
		newTask->AddBuffer(leafDivBuffers[LeafMap[currNode]]);
		newTask->AddBuffer(leafLabelBuffers[LeafMap[currNode]]);
	}
}

void vtkCudaHierarchicalMaxFlowSegmentation2::CreateUpdateLabelsTask(vtkIdType currNode){
	int NumKids = this->Hierarchy->GetNumberOfChildren(currNode);
	for(int i = 0; i < NumKids; i++)
		CreateUpdateLabelsTask( this->Hierarchy->GetChild(currNode,i) );
	if( currNode == this->Hierarchy->GetRoot() ) return;
	
	//find appropriate number of repetitions
	int NumReps = NumKids ? this->NumberOfIterations-1: this->NumberOfIterations;

	//create the new task
	Task* newTask = new Task(this,-2,2,NumReps,currNode,Task::UpdateLabelsTask);
	this->UpdateLabelsTasks[currNode] = newTask;
	if(NumKids != 0){
		newTask->AddBuffer(branchSinkBuffers[BranchMap[currNode]]);
		newTask->AddBuffer(branchIncBuffers[BranchMap[currNode]]);
		newTask->AddBuffer(branchDivBuffers[BranchMap[currNode]]);
		newTask->AddBuffer(branchLabelBuffers[BranchMap[currNode]]);
	}else{
		newTask->AddBuffer(leafSinkBuffers[LeafMap[currNode]]);
		newTask->AddBuffer(leafIncBuffers[LeafMap[currNode]]);
		newTask->AddBuffer(leafDivBuffers[LeafMap[currNode]]);
		newTask->AddBuffer(leafLabelBuffers[LeafMap[currNode]]);
	}
}

void vtkCudaHierarchicalMaxFlowSegmentation2::AddIterationTaskDependencies(vtkIdType currNode){
	int NumKids = this->Hierarchy->GetNumberOfChildren(currNode);
	for(int i = 0; i < NumKids; i++)
		AddIterationTaskDependencies( this->Hierarchy->GetChild(currNode,i) );

	if( NumKids == 0 ){
		vtkIdType parNode = this->Hierarchy->GetParent(currNode);
		this->UpdateSpatialFlowsTasks[currNode]->AddTaskToSignal(this->ApplySinkPotentialLeafTasks[currNode]);
		this->ApplySinkPotentialLeafTasks[currNode]->AddTaskToSignal(this->ApplySourcePotentialTasks[currNode]);
		this->ApplySourcePotentialTasks[currNode]->AddTaskToSignal(this->DivideOutWorkingBufferTasks[parNode]);
		this->ApplySourcePotentialTasks[currNode]->AddTaskToSignal(this->UpdateLabelsTasks[currNode]);
		this->UpdateLabelsTasks[currNode]->AddTaskToSignal(this->UpdateSpatialFlowsTasks[currNode]);
	}else if( currNode == this->Hierarchy->GetRoot() ){
		this->ClearWorkingBufferTasks[currNode]->AddTaskToSignal(this->DivideOutWorkingBufferTasks[currNode]);
		for(int i = 0; i < NumKids; i++)
			this->ClearWorkingBufferTasks[currNode]->AddTaskToSignal(this->ApplySourcePotentialTasks[this->Hierarchy->GetChild(currNode,i)]);
		this->DivideOutWorkingBufferTasks[currNode]->AddTaskToSignal(this->ClearWorkingBufferTasks[currNode]);
		for(int i = 0; i < NumKids; i++)
			this->DivideOutWorkingBufferTasks[currNode]->AddTaskToSignal(this->UpdateLabelsTasks[this->Hierarchy->GetChild(currNode,i)]);
	}else{
		vtkIdType parNode = this->Hierarchy->GetParent(currNode);
		this->ClearWorkingBufferTasks[currNode]->AddTaskToSignal(this->ApplySinkPotentialBranchTasks[currNode]);
		for(int i = 0; i < NumKids; i++)
			this->ClearWorkingBufferTasks[currNode]->AddTaskToSignal(this->ApplySourcePotentialTasks[this->Hierarchy->GetChild(currNode,i)]);
		this->UpdateSpatialFlowsTasks[currNode]->AddTaskToSignal(this->ApplySinkPotentialBranchTasks[currNode]);
		this->ApplySinkPotentialBranchTasks[currNode]->AddTaskToSignal(this->DivideOutWorkingBufferTasks[currNode]);
		this->DivideOutWorkingBufferTasks[currNode]->AddTaskToSignal(this->ApplySourcePotentialTasks[currNode]);
		this->DivideOutWorkingBufferTasks[currNode]->AddTaskToSignal(this->ClearWorkingBufferTasks[currNode]);
		for(int i = 0; i < NumKids; i++)
			this->DivideOutWorkingBufferTasks[currNode]->AddTaskToSignal(this->UpdateLabelsTasks[this->Hierarchy->GetChild(currNode,i)]);
		this->ApplySourcePotentialTasks[currNode]->AddTaskToSignal(this->DivideOutWorkingBufferTasks[parNode]);
		this->ApplySourcePotentialTasks[currNode]->AddTaskToSignal(this->UpdateLabelsTasks[currNode]);
		this->UpdateLabelsTasks[currNode]->AddTaskToSignal(this->UpdateSpatialFlowsTasks[currNode]);
	}
}

void vtkCudaHierarchicalMaxFlowSegmentation2::CreateInitializeAllSpatialFlowsToZeroTasks(vtkIdType currNode){
	int NumKids = this->Hierarchy->GetNumberOfChildren(currNode);
	for(int i = 0; i < NumKids; i++)
		CreateInitializeAllSpatialFlowsToZeroTasks( this->Hierarchy->GetChild(currNode,i) );
	
	//modify the task accordingly
	if( NumKids == 0 ){
		Task* newTask1 = new Task(this,0,1,1,currNode,Task::ClearBufferInitially);
		Task* newTask2 = new Task(this,0,1,1,currNode,Task::ClearBufferInitially);
		Task* newTask3 = new Task(this,0,1,1,currNode,Task::ClearBufferInitially);
		Task* newTask4 = new Task(this,0,1,1,currNode,Task::ClearBufferInitially);
		newTask1->AddTaskToSignal(this->UpdateSpatialFlowsTasks[currNode]);
		newTask2->AddTaskToSignal(this->UpdateSpatialFlowsTasks[currNode]);
		newTask3->AddTaskToSignal(this->UpdateSpatialFlowsTasks[currNode]);
		newTask4->AddTaskToSignal(this->UpdateSpatialFlowsTasks[currNode]);
		newTask1->AddBuffer(this->leafDivBuffers[LeafMap[currNode]]);
		newTask2->AddBuffer(this->leafFlowXBuffers[LeafMap[currNode]]);
		newTask3->AddBuffer(this->leafFlowYBuffers[LeafMap[currNode]]);
		newTask4->AddBuffer(this->leafFlowZBuffers[LeafMap[currNode]]);
	}else if(currNode != this->Hierarchy->GetRoot()){
		Task* newTask1 = new Task(this,0,1,1,currNode,Task::ClearBufferInitially);
		Task* newTask2 = new Task(this,0,1,1,currNode,Task::ClearBufferInitially);
		Task* newTask3 = new Task(this,0,1,1,currNode,Task::ClearBufferInitially);
		Task* newTask4 = new Task(this,0,1,1,currNode,Task::ClearBufferInitially);
		newTask1->AddTaskToSignal(this->UpdateSpatialFlowsTasks[currNode]);
		newTask2->AddTaskToSignal(this->UpdateSpatialFlowsTasks[currNode]);
		newTask3->AddTaskToSignal(this->UpdateSpatialFlowsTasks[currNode]);
		newTask4->AddTaskToSignal(this->UpdateSpatialFlowsTasks[currNode]);
		newTask1->AddBuffer(this->branchDivBuffers[BranchMap[currNode]]);
		newTask2->AddBuffer(this->branchFlowXBuffers[BranchMap[currNode]]);
		newTask3->AddBuffer(this->branchFlowYBuffers[BranchMap[currNode]]);
		newTask4->AddBuffer(this->branchFlowZBuffers[BranchMap[currNode]]);
	}
}

void vtkCudaHierarchicalMaxFlowSegmentation2::CreateInitializeLeafSinkFlowsToCapTasks(vtkIdType currNode){
	int NumKids = this->Hierarchy->GetNumberOfChildren(currNode);
	for(int i = 0; i < NumKids; i++)
		CreateInitializeLeafSinkFlowsToCapTasks( this->Hierarchy->GetChild(currNode,i) );
	if( NumKids > 0 ) return;
	
	if( LeafMap[currNode] != 0 ){
		Task* newTask1 = new Task(this,0,1,1,currNode,Task::InitializeLeafFlows);
		Task* newTask2 = new Task(this,-2,1,1,currNode,Task::MinimizeLeafFlows);
		InitializeLeafSinkFlowsTasks.insert(std::pair<int,Task*>(LeafMap[currNode],newTask1));
		MinimizeLeafSinkFlowsTasks.insert(std::pair<int,Task*>(LeafMap[currNode],newTask2));
		newTask1->AddBuffer(this->leafSinkBuffers[LeafMap[currNode]]);
		newTask1->AddBuffer(this->leafDataTermBuffers[LeafMap[currNode]]);
		newTask2->AddBuffer(this->leafSinkBuffers[0]);
		newTask2->AddBuffer(this->leafSinkBuffers[LeafMap[currNode]]);
		newTask1->AddTaskToSignal(newTask2);
		if( InitializeLeafSinkFlowsTasks.find(0) != InitializeLeafSinkFlowsTasks.end() )
			InitializeLeafSinkFlowsTasks[0]->AddTaskToSignal(newTask2);
	}else{
		Task* newTask1 = new Task(this,0,1,1,currNode,Task::InitializeLeafFlows);
		InitializeLeafSinkFlowsTasks.insert(std::pair<int,Task*>(0,newTask1));
		newTask1->AddBuffer(this->leafSinkBuffers[0]);
		newTask1->AddBuffer(this->leafDataTermBuffers[0]);
		for( std::map<int,Task*>::iterator it = MinimizeLeafSinkFlowsTasks.begin();
			 it != this->MinimizeLeafSinkFlowsTasks.end(); it++)
			newTask1->AddTaskToSignal(it->second);
	}

}

void vtkCudaHierarchicalMaxFlowSegmentation2::CreateCopyMinimalLeafSinkFlowsTasks(vtkIdType currNode){
	int NumKids = this->Hierarchy->GetNumberOfChildren(currNode);
	for(int i = 0; i < NumKids; i++)
		CreateCopyMinimalLeafSinkFlowsTasks( this->Hierarchy->GetChild(currNode,i) );

	Task* newTask1 = new Task(this,-((int)this->MinimizeLeafSinkFlowsTasks.size()),1,1,currNode,Task::PropogateLeafFlows);
	PropogateLeafSinkFlowsTasks.insert(std::pair<vtkIdType,Task*>(currNode,newTask1));
	if( currNode != this->Hierarchy->GetRoot() ) newTask1->AddTaskToSignal(this->UpdateSpatialFlowsTasks[currNode]);
	for(int i = 0; i < NumKids; i++)
		newTask1->AddTaskToSignal(this->UpdateSpatialFlowsTasks[this->Hierarchy->GetChild(currNode,i)]);
	newTask1->AddBuffer(this->leafSinkBuffers[0]);
	for( std::map<int,Task*>::iterator it = this->MinimizeLeafSinkFlowsTasks.begin(); it != this->MinimizeLeafSinkFlowsTasks.end(); it++)
		it->second->AddTaskToSignal(newTask1);

	if( this->Hierarchy->GetRoot() == currNode )
		newTask1->AddBuffer(this->sourceFlowBuffer);
	else if( NumKids > 0 )
		newTask1->AddBuffer(this->branchSinkBuffers[BranchMap[currNode]]);
	else
		newTask1->AddBuffer(this->leafSinkBuffers[LeafMap[currNode]]);

}

void vtkCudaHierarchicalMaxFlowSegmentation2::CreateFindInitialLabellingAndSumTasks(vtkIdType currNode){
	int NumKids = this->Hierarchy->GetNumberOfChildren(currNode);
	for(int i = 0; i < NumKids; i++)
		CreateFindInitialLabellingAndSumTasks( this->Hierarchy->GetChild(currNode,i) );
	if( NumKids > 0 ) return;

	Task* newTask1 = new Task(this,-1,1,1,currNode,Task::InitializeLeafLabels);
	Task* newTask2 = new Task(this,-2,1,1,currNode,Task::AccumulateLabels);
	this->PropogateLeafSinkFlowsTasks[currNode]->AddTaskToSignal(newTask1);
	newTask1->AddTaskToSignal(newTask2);
	this->InitialLabellingSumTasks.insert(std::pair<vtkIdType,Task*>(currNode,newTask2));
	newTask1->AddBuffer(this->leafSinkBuffers[LeafMap[currNode]]);
	newTask1->AddBuffer(this->leafDataTermBuffers[LeafMap[currNode]]);
	newTask1->AddBuffer(this->leafLabelBuffers[LeafMap[currNode]]);
	newTask2->AddBuffer(this->sourceWorkingBuffer);
	newTask2->AddBuffer(this->leafLabelBuffers[LeafMap[currNode]]);
}

void vtkCudaHierarchicalMaxFlowSegmentation2::CreateClearSourceWorkingBufferTask(){
	Task* newTask = new Task(this,0,1,1,this->Hierarchy->GetRoot(),Task::ClearBufferInitially);
	newTask->AddBuffer(this->sourceWorkingBuffer);
	for( std::map<vtkIdType,Task*>::iterator it = InitialLabellingSumTasks.begin(); it != InitialLabellingSumTasks.end(); it++)
		newTask->AddTaskToSignal(it->second);
}

void vtkCudaHierarchicalMaxFlowSegmentation2::CreateDivideOutLabelsTasks(vtkIdType currNode){
	int NumKids = this->Hierarchy->GetNumberOfChildren(currNode);
	for(int i = 0; i < NumKids; i++)
		CreateDivideOutLabelsTasks( this->Hierarchy->GetChild(currNode,i) );
	if( NumKids > 0 ) return;
	
	Task* newTask1 = new Task(this,-(int)InitialLabellingSumTasks.size(),1,1,currNode,Task::CorrectLabels);
	this->CorrectLabellingTasks[currNode] = newTask1;
	for(std::map<vtkIdType,Task*>::iterator taskIt = InitialLabellingSumTasks.begin(); taskIt != InitialLabellingSumTasks.end(); taskIt++)
		taskIt->second->AddTaskToSignal(newTask1);
	newTask1->AddBuffer(this->sourceWorkingBuffer);
	newTask1->AddBuffer(this->leafLabelBuffers[LeafMap[currNode]]);
	newTask1->AddTaskToSignal(this->UpdateSpatialFlowsTasks[currNode]);
	newTask1->AddTaskToSignal(this->ClearWorkingBufferTasks[this->Hierarchy->GetRoot()]);
}

void vtkCudaHierarchicalMaxFlowSegmentation2::CreatePropogateLabelsTasks(vtkIdType currNode){
	int NumKids = this->Hierarchy->GetNumberOfChildren(currNode);
	for(int i = 0; i < NumKids; i++)
		CreatePropogateLabelsTasks( this->Hierarchy->GetChild(currNode,i) );
	if( currNode == this->Hierarchy->GetRoot() || NumKids == 0 ) return;
	
	//clear the current buffer
	Task* newTask1 = new Task(this,0,1,1,currNode,Task::ClearBufferInitially);
	newTask1->AddBuffer(this->branchLabelBuffers[BranchMap[currNode]]);

	//accumulate from children
	for(int i = 0; i < NumKids; i++){
		vtkIdType child = this->Hierarchy->GetChild(currNode,i);
		Task* newTask2 = new Task(this,-1,1,1,child,Task::AccumulateLabels);
		this->PropogateLabellingTasks[child] = newTask2;
		newTask1->AddTaskToSignal(newTask2);
		newTask2->AddBuffer(this->branchLabelBuffers[BranchMap[currNode]]);
		if( this->Hierarchy->IsLeaf(child) ){
			newTask2->Active--;
			newTask2->AddBuffer(this->leafLabelBuffers[LeafMap[child]]);
			this->CorrectLabellingTasks[child]->AddTaskToSignal(newTask2);
		}else{
			newTask2->AddBuffer(this->branchLabelBuffers[BranchMap[child]]);
			int NumKids2 = this->Hierarchy->GetNumberOfChildren(child);
			for(int i2 = 0; i2 < NumKids2; i2++)
				this->PropogateLabellingTasks[this->Hierarchy->GetChild(child,i2)]->AddTaskToSignal(newTask2);
		}
		newTask2->AddTaskToSignal(this->UpdateSpatialFlowsTasks[currNode]);
	}

}
