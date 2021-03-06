/*------------------------------------------------------------------------*/
/*  Copyright 2014 Sandia Corporation.                                    */
/*  This software is released under the license detailed                  */
/*  in the file, LICENSE, which is located in the top-level Nalu          */
/*  directory structure                                                   */
/*------------------------------------------------------------------------*/


#include <DataProbePostProcessing.h>
#include <FieldTypeDef.h>
#include <NaluParsing.h>
#include <NaluEnv.h>
#include <Realm.h>

// stk_util
#include <stk_util/parallel/ParallelReduce.hpp>

// stk_mesh/base/fem
#include <stk_mesh/base/BulkData.hpp>
#include <stk_mesh/base/Entity.hpp>
#include <stk_mesh/base/Field.hpp>
#include <stk_mesh/base/GetBuckets.hpp>
#include <stk_mesh/base/Selector.hpp>
#include <stk_mesh/base/MetaData.hpp>
#include <stk_mesh/base/Part.hpp>

// stk_io
#include <stk_io/IossBridge.hpp>

// basic c++
#include <stdexcept>
#include <string>
#include <fstream>
#include <iomanip>
#include <algorithm>

namespace sierra{
namespace nalu{

//==========================================================================
// Class Definition
//==========================================================================
// DataProbePostProcessing - post process
//==========================================================================
//--------------------------------------------------------------------------
//-------- constructor -----------------------------------------------------
//--------------------------------------------------------------------------
DataProbePostProcessing::DataProbePostProcessing(
  Realm &realm,
  const YAML::Node &node)
  : realm_(realm),
    outputFreq_(10)
{
  // load the data
  load(node);
}

//--------------------------------------------------------------------------
//-------- destructor ------------------------------------------------------
//--------------------------------------------------------------------------
DataProbePostProcessing::~DataProbePostProcessing()
{
  // delete info?
}

//--------------------------------------------------------------------------
//-------- load ------------------------------------------------------------
//--------------------------------------------------------------------------
void
DataProbePostProcessing::load(
  const YAML::Node & y_node)
{
  // output for results
  const YAML::Node *y_dataProbe = y_node.FindValue("data_probes");
  if (y_dataProbe) {
    NaluEnv::self().naluOutputP0() << "DataProbePostProcessing::load" << std::endl;

    const YAML::Node *y_specs = expect_sequence(*y_dataProbe, "specifications", false);
    if (y_specs) {

      // each specification can have multiple probes
      for (size_t ispec = 0; ispec < y_specs->size(); ++ispec) {
        const YAML::Node &y_spec = (*y_specs)[ispec];

        DataProbeSpecInfo *probeSpec = new DataProbeSpecInfo();
        dataProbeSpecInfo_.push_back(probeSpec);

        DataProbeInfo *probeInfo = new DataProbeInfo();
        probeSpec->dataProbeInfo_.push_back(probeInfo);
        
        // name; will serve as the transfer name
        const YAML::Node *theName = y_spec.FindValue("name");
        if ( theName )
          *theName >> probeSpec->xferName_;
        else
          throw std::runtime_error("DataProbePostProcessing: no name provided");

        // extract the set of from target names; each spec is homogeneous in this respect
        const YAML::Node &fromTargets = y_spec["from_target_part"];
        if (fromTargets.Type() == YAML::NodeType::Scalar) {
          probeSpec->fromTargetNames_.resize(1);
          fromTargets >> probeSpec->fromTargetNames_[0];
        }
        else {
          probeSpec->fromTargetNames_.resize(fromTargets.size());
          for (size_t i=0; i < fromTargets.size(); ++i) {
            fromTargets[i] >> probeSpec->fromTargetNames_[i];
          }
        }

        // extract the type of probe, e.g., line of site, plane, etc
        const YAML::Node *y_loss = expect_sequence(y_spec, "line_of_site_specifications", false);
        if (y_loss) {

          // l-o-s is active..
          probeInfo->isLineOfSite_ = true;
          
          // extract and save number of probes
          const int numProbes = y_loss->size();
          probeInfo->numProbes_ = numProbes;

          // resize everything...
          probeInfo->partName_.resize(numProbes);
          probeInfo->processorId_.resize(numProbes);
          probeInfo->numPoints_.resize(numProbes);
          probeInfo->tipCoordinates_.resize(numProbes);
          probeInfo->tailCoordinates_.resize(numProbes);
          probeInfo->nodeVector_.resize(numProbes);
          probeInfo->part_.resize(numProbes);

          // deal with processors... Distribute each probe over subsequent procs
          const int numProcs = NaluEnv::self().parallel_size();
          const int probePerProc = numProcs > numProbes ? 1 : numProbes / numProcs;

          for (size_t ilos = 0; ilos < y_loss->size(); ++ilos) {
            const YAML::Node &y_los = (*y_loss)[ilos];

            // processor id; distribute los equally over the number of processors
            probeInfo->processorId_[ilos] = (ilos + probePerProc)/probePerProc - 1;

            // name; which is the part name of choice
            const YAML::Node *nameNode = y_los.FindValue("name");
            if ( nameNode )
              *nameNode >> probeInfo->partName_[ilos];
            else
              throw std::runtime_error("DataProbePostProcessing: lacking the name");

            // number of points
            const YAML::Node *numPoints = y_los.FindValue("number_of_points");
            if ( numPoints )
              *numPoints >> probeInfo->numPoints_[ilos];
            else
              throw std::runtime_error("DataProbePostProcessing: lacking number of points");

            // coordinates; tip
            const YAML::Node *tipCoord = y_los.FindValue("tip_coordinates");
            if ( tipCoord )
              *tipCoord >> probeInfo->tipCoordinates_[ilos];
            else
              throw std::runtime_error("DataProbePostProcessing: lacking tip coordinates");

            // coordinates; tail
            const YAML::Node *tailCoord = y_los.FindValue("tail_coordinates");
            if ( tailCoord )
              *tailCoord >> probeInfo->tailCoordinates_[ilos];
            else
              throw std::runtime_error("DataProbePostProcessing: lacking tail coordinates");
        
          }
        }
        else {
          throw std::runtime_error("DataProbePostProcessing: only supports line_of_site_specifications");
        }
        
        // extract the output variables
        const YAML::Node *y_outputs = expect_sequence(y_spec, "output_variables", false);
        if (y_outputs) {
          for (size_t ioutput = 0; ioutput < y_outputs->size(); ++ioutput) {
            const YAML::Node &y_output = (*y_outputs)[ioutput];
  
            // find the name, size and type
            const YAML::Node *fieldNameNode = y_output.FindValue("field_name");
            const YAML::Node *fieldSizeNode = y_output.FindValue("field_size");
    
            if ( NULL == fieldNameNode ) 
              throw std::runtime_error("DataProbePostProcessing::load() Sorry, field name must be provided");
            
            if ( NULL == fieldSizeNode ) 
              throw std::runtime_error("DataProbePostProcessing::load() Sorry, field size must be provided");
            
            // extract data
            std::string fieldName;
            int fieldSize;
            *fieldNameNode >> fieldName;
            *fieldSizeNode >> fieldSize;
            
            // push to probeInfo
            std::pair<std::string, int> fieldInfoPair = std::make_pair(fieldName + "_probe", fieldSize);
            probeInfo->fieldInfo_.push_back(fieldInfoPair);
          }
        }
      }
    }
  }
}

//--------------------------------------------------------------------------
//-------- setup -----------------------------------------------------------
//--------------------------------------------------------------------------
void
DataProbePostProcessing::setup()
{
  // objective: declare the part, register the fields; must be before populate_mesh()

  stk::mesh::MetaData &metaData = realm_.meta_data();

  // first, declare the part
  for ( size_t idps = 0; idps < dataProbeSpecInfo_.size(); ++idps ) {

    DataProbeSpecInfo *probeSpec = dataProbeSpecInfo_[idps];

    for ( size_t k = 0; k < probeSpec->dataProbeInfo_.size(); ++k ) {
    
      DataProbeInfo *probeInfo = probeSpec->dataProbeInfo_[k];
          
      // loop over probes... one part per probe
      for ( int j = 0; j < probeInfo->numProbes_; ++j ) {
        // extract name
        std::string partName = probeInfo->partName_[j];
        // declare the part and push it to info; make the part available as a nodeset
        probeInfo->part_[j] = &metaData.declare_part(partName, stk::topology::NODE_RANK);
        stk::io::put_io_part_attribute(*probeInfo->part_[j]);
      }
    }
  }
  
  // second, register the fields
  const int nDim = metaData.spatial_dimension();
  for ( size_t idps = 0; idps < dataProbeSpecInfo_.size(); ++idps ) {

    DataProbeSpecInfo *probeSpec = dataProbeSpecInfo_[idps];

    for ( size_t k = 0; k < probeSpec->dataProbeInfo_.size(); ++k ) {
    
      DataProbeInfo *probeInfo = probeSpec->dataProbeInfo_[k];
          
      // loop over probes... register all fields within the ProbInfo on each part
      for ( int j = 0; j < probeInfo->numProbes_; ++j ) {
        // extract the part
        stk::mesh::Part *probePart = probeInfo->part_[j];
        // everyone needs coordinates to be registered
        VectorFieldType *coordinates 
          =  &(metaData.declare_field<VectorFieldType>(stk::topology::NODE_RANK, "coordinates"));
        stk::mesh::put_field(*coordinates, *probePart, nDim);
        // now the general set of fields for this probe
        for ( size_t j = 0; j < probeInfo->fieldInfo_.size(); ++j ) 
          register_field(probeInfo->fieldInfo_[j].first, probeInfo->fieldInfo_[j].second, metaData, probePart);
      }
    }
  }
}

//--------------------------------------------------------------------------
//-------- initialize ------------------------------------------------------
//--------------------------------------------------------------------------
void
DataProbePostProcessing::initialize()
{
  // objective: generate the ids, declare the entity(s) and register the fields; 
  // *** must be after populate_mesh() ***
  stk::mesh::BulkData &bulkData = realm_.bulk_data();
  stk::mesh::MetaData &metaData = realm_.meta_data();

  std::vector<std::string> toPartNameVec;
  std::vector<std::string> fromPartNameVec;

  for ( size_t idps = 0; idps < dataProbeSpecInfo_.size(); ++idps ) {

    DataProbeSpecInfo *probeSpec = dataProbeSpecInfo_[idps];

    for ( size_t k = 0; k < probeSpec->dataProbeInfo_.size(); ++k ) {
    
      DataProbeInfo *probeInfo = probeSpec->dataProbeInfo_[k];
          
      for ( int j = 0; j < probeInfo->numProbes_; ++j ) {

        // extract some things off of the probeInfo
        stk::mesh::Part *probePart = probeInfo->part_[j];
        const int numPoints = probeInfo->numPoints_[j];
        const int processorId  = probeInfo->processorId_[j];
   
        // generate new ids
        std::vector<stk::mesh::EntityId> availableNodeIds(numPoints);
        bulkData.generate_new_ids(stk::topology::NODE_RANK, numPoints, availableNodeIds);

        // reference to the nodeVector
        std::vector<stk::mesh::Entity> &nodeVec = probeInfo->nodeVector_[j];

        // size the vector of nodes
        if ( processorId == NaluEnv::self().parallel_rank()) {    
          nodeVec.resize(numPoints);
        }

        // declare the nodes
        bulkData.modification_begin();
        for (int i = 0; i < numPoints; ++i) {
          if ( processorId == NaluEnv::self().parallel_rank()) {    
            stk::mesh::Entity theNode = bulkData.declare_entity(stk::topology::NODE_RANK, availableNodeIds[i], *probePart);
            nodeVec[i] = theNode;
          }
        }
        bulkData.modification_end();
      }
    }
  }
  
  // populate values for coord; probe stays the same place
  // *** so worry about mesh motion (if the probe moves around)**
  VectorFieldType *coordinates = metaData.get_field<VectorFieldType>(stk::topology::NODE_RANK, "coordinates");

  const int nDim = metaData.spatial_dimension();
  for ( size_t idps = 0; idps < dataProbeSpecInfo_.size(); ++idps ) {

    DataProbeSpecInfo *probeSpec = dataProbeSpecInfo_[idps];

    for ( size_t k = 0; k < probeSpec->dataProbeInfo_.size(); ++k ) {
    
      DataProbeInfo *probeInfo = probeSpec->dataProbeInfo_[k];
          
      for ( int j = 0; j < probeInfo->numProbes_; ++j ) {

        // reference to the nodeVector
        std::vector<stk::mesh::Entity> &nodeVec = probeInfo->nodeVector_[j];
        
        // populate the coordinates
        double dx[3] = {};
        
        std::vector<double> tipC(nDim);
        tipC[0] = probeInfo->tipCoordinates_[j].x_;
        tipC[1] = probeInfo->tipCoordinates_[j].y_;
        
        std::vector<double> tailC(nDim);
        tailC[0] = probeInfo->tailCoordinates_[j].x_;
        tailC[1] = probeInfo->tailCoordinates_[j].y_;
        if ( nDim > 2) {
          tipC[2] = probeInfo->tipCoordinates_[j].z_;
          tailC[2] = probeInfo->tailCoordinates_[j].z_;
        }
        
        const int numPoints = probeInfo->numPoints_[j];
        for ( int j = 0; j < nDim; ++j )
          dx[j] = (tipC[j] - tailC[j])/(double)(numPoints-1);
        
        // now populate the coordinates; can use a simple loop rather than buckets
        for ( size_t j = 0; j < nodeVec.size(); ++j ) {
          stk::mesh::Entity node = nodeVec[j];
          double * coords = stk::mesh::field_data(*coordinates, node );
          for ( int i = 0; i < nDim; ++i )
            coords[i] = tailC[i] + j*dx[i];
        }
      }
    }
    // create the transfer; manage it locally
  }

  create_inactive_selector();
}
  
//--------------------------------------------------------------------------
//-------- register_field --------------------------------------------------
//--------------------------------------------------------------------------
void
DataProbePostProcessing::register_field(
  const std::string fieldName,
  const int fieldSize,
  stk::mesh::MetaData &metaData,
  stk::mesh::Part *part)
{
  stk::mesh::FieldBase *toField 
    = &(metaData.declare_field< stk::mesh::Field<double, stk::mesh::SimpleArrayTag> >(stk::topology::NODE_RANK, fieldName));
  stk::mesh::put_field(*toField, *part, fieldSize);
}

//--------------------------------------------------------------------------
//-------- create_inactive_selector ----------------------------------------
//--------------------------------------------------------------------------
void
DataProbePostProcessing::create_inactive_selector()
{
  for ( size_t idps = 0; idps < dataProbeSpecInfo_.size(); ++idps ) {

    DataProbeSpecInfo *probeSpec = dataProbeSpecInfo_[idps];

    for ( size_t k = 0; k < probeSpec->dataProbeInfo_.size(); ++k ) {
    
      DataProbeInfo *probeInfo = probeSpec->dataProbeInfo_[k];
          
      // loop over probes... one part per probe
      for ( int j = 0; j < probeInfo->numProbes_; ++j ) {
        allTheParts_.push_back(probeInfo->part_[j]);
      }
    }
  }

  inactiveSelector_ = stk::mesh::selectUnion(allTheParts_);
}
  
//--------------------------------------------------------------------------
//-------- review ----------------------------------------------------------
//--------------------------------------------------------------------------
void
DataProbePostProcessing::review(
  const DataProbeInfo *probeInfo)
{
  // may or may not want this
}

//--------------------------------------------------------------------------
//-------- execute ---------------------------------------------------------
//--------------------------------------------------------------------------
void
DataProbePostProcessing::execute()
{
  // only do work if this is an output step
  const double currentTime = realm_.get_current_time();
  const int timeStepCount = realm_.get_time_step_count();
  const bool isOutput = timeStepCount % outputFreq_ == 0;

  if ( isOutput ) {
    // execute transfer...

    // provide the results
    provide_average(currentTime, timeStepCount);
  }
}

//--------------------------------------------------------------------------
//-------- provide_average -------------------------------------------------
//--------------------------------------------------------------------------
void
DataProbePostProcessing::provide_average(
  const double currentTime,
  const double timeStepCount)
{ 
  stk::mesh::MetaData &metaData = realm_.meta_data();

  std::cout << "DataProbePostProcessing::provide_average() at current time/timeStepCount: " 
            << currentTime <<"/"<< timeStepCount << std::endl;
  
  for ( size_t idps = 0; idps < dataProbeSpecInfo_.size(); ++idps ) {

    DataProbeSpecInfo *probeSpec = dataProbeSpecInfo_[idps];

    std::cout << " ...will proceed with specification name: " 
              << probeSpec->xferName_ << std::endl;
    
    for ( size_t k = 0; k < probeSpec->dataProbeInfo_.size(); ++k ) {
    
      DataProbeInfo *probeInfo = probeSpec->dataProbeInfo_[k];
          
      for ( int inp = 0; inp < probeInfo->numProbes_; ++inp ) {

        std::cout << std::endl;
        std::cout << " .......................... and probe name: "  
                  << probeInfo->partName_[inp] << std::endl;
        std::cout << std::endl;

        // reference to the nodeVector
        std::vector<stk::mesh::Entity> &nodeVec = probeInfo->nodeVector_[inp];
      
        const int numPoints = probeInfo->numPoints_[inp];
      
        // loop over fields
        for ( size_t ifi = 0; ifi < probeInfo->fieldInfo_.size(); ++ifi ) {
          const std::string fieldName = probeInfo->fieldInfo_[ifi].first;
          const int fieldSize = probeInfo->fieldInfo_[ifi].second;
          
          const stk::mesh::FieldBase *theField = metaData.get_field(stk::topology::NODE_RANK, fieldName);
      
          // now populate the coordinates; can use a simple loop rather than buckets
          std::vector<double> meanValue(fieldSize, 0.0);
          for ( size_t inv = 0; inv < nodeVec.size(); ++inv ) {
            stk::mesh::Entity node = nodeVec[inv];
            double * theF = (double*)stk::mesh::field_data(*theField, node );
            for ( int ifs = 0; ifs < fieldSize; ++ifs )
              meanValue[ifs] += theF[ifs];
          }
          
          for ( int ifs = 0; ifs < fieldSize; ++ifs ) {
            std::cout << "Mean value for " << fieldName << "[" << ifs << "] is: " << meanValue[ifs]/numPoints << std::endl; 
          }
        }
      }
    }
  }
}

//--------------------------------------------------------------------------
//-------- get_inactive_selector -------------------------------------------
//--------------------------------------------------------------------------
stk::mesh::Selector &
DataProbePostProcessing::get_inactive_selector()
{
  return inactiveSelector_;
}
 
} // namespace nalu
} // namespace Sierra
