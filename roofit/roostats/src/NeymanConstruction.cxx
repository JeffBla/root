// @(#)root/roostats:$Id$
// Author: Kyle Cranmer   January 2009

/*************************************************************************
 * Copyright (C) 1995-2008, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

/** \class RooStats::NeymanConstruction
    \ingroup Roostats

NeymanConstruction is a concrete implementation of the NeymanConstruction
interface that, as the name suggests, performs a NeymanConstruction. It produces
a RooStats::PointSetInterval, which is a concrete implementation of the
ConfInterval interface.

The Neyman Construction is not a uniquely defined statistical technique, it
requires that one specify an ordering rule or ordering principle, which is
usually incoded by choosing a specific test statistic and limits of integration
(corresponding to upper/lower/central limits). As a result, this class must be
configured with the corresponding information before it can produce an interval.
Common configurations, such as the Feldman-Cousins approach, can be enforced by
other light weight classes.

The Neyman Construction considers every point in the parameter space
independently, no assumptions are made that the interval is connected or of a
particular shape. As a result, the PointSetInterval class is used to represent
the result. The user indicate which points in the parameter space to perform
the construction by providing a PointSetInterval instance with the desired points.

This class is fairly light weight, because the choice of parameter points to be
considered is factorized and so is the creation of the sampling distribution of
the test statistic (which is done by a concrete class implementing the
DistributionCreator interface). As a result, this class basically just drives the
construction by:

  - using a DistributionCreator to create the SamplingDistribution of a user-
    defined test statistic for each parameter point of interest,
  - defining the acceptance region in the data by finding the thresholds on the
    test statistic such that the integral of the sampling distribution is of the
    appropriate size and consistent with the limits of integration
    (eg. upper/lower/central limits),
  - and finally updating the PointSetInterval based on whether the value of the
    test statistic evaluated on the data are in the acceptance region.

*/

#include "RooStats/NeymanConstruction.h"

#include "RooStats/RooStatsUtils.h"

#include "RooStats/PointSetInterval.h"

#include "RooStats/SamplingDistribution.h"
#include "RooStats/ToyMCSampler.h"
#include "RooStats/ModelConfig.h"

#include "RooMsgService.h"
#include "RooGlobalFunc.h"

#include "RooDataSet.h"
#include "TFile.h"
#include "TMath.h"
#include "TH1F.h"


using namespace RooFit;
using namespace RooStats;
using std::endl, std::string;


////////////////////////////////////////////////////////////////////////////////
/// default constructor

NeymanConstruction::NeymanConstruction(RooAbsData& data, ModelConfig& model):
   fSize(0.05),
   fData(data),
   fModel(model),
   fTestStatSampler(nullptr),
   fPointsToTest(nullptr),
   fLeftSideFraction(0),
   fConfBelt(nullptr),  // constructed with tree data
   fAdaptiveSampling(false),
   fAdditionalNToysFactor(1.),
   fSaveBeltToFile(false),
   fCreateBelt(false)

{
//   fWS = new RooWorkspace();
//   fOwnsWorkspace = true;
//   fDataName = "";
//   fPdfName = "";
}

////////////////////////////////////////////////////////////////////////////////
/// default constructor
///  if(fOwnsWorkspace && fWS) delete fWS;
///  if(fConfBelt) delete fConfBelt;

NeymanConstruction::~NeymanConstruction() {
}

////////////////////////////////////////////////////////////////////////////////
/// Main interface to get a RooStats::ConfInterval.
/// It constructs a RooStats::SetInterval.

PointSetInterval* NeymanConstruction::GetInterval() const {

  TFile* f=nullptr;
  if(fSaveBeltToFile){
    //coverity[FORWARD_NULL]
    oocoutI(f,Contents) << "NeymanConstruction saving ConfidenceBelt to file SamplingDistributions.root" << std::endl;
    f = new TFile("SamplingDistributions.root","recreate");
  }

  Int_t npass = 0;
  RooArgSet* point;

  // strange problems when using snapshots.
  //  RooArgSet* fPOI = (RooArgSet*) fModel.GetParametersOfInterest()->snapshot();
  RooArgSet* fPOI = new RooArgSet(*fModel.GetParametersOfInterest());

  RooDataSet* pointsInInterval = new RooDataSet("pointsInInterval",
                   "points in interval",
                  *(fPointsToTest->get(0)) );

  // loop over points to test
  for(Int_t i=0; i<fPointsToTest->numEntries(); ++i){
     // get a parameter point from the list of points to test.
    point = const_cast<RooArgSet*>(fPointsToTest->get(i));//->clone("temp");

    // set parameters of interest to current point
    fPOI->assign(*point);

    // set test stat sampler to use this point
    fTestStatSampler->SetParametersForTestStat(*fPOI);

     // get the value of the test statistic for this data set
    double thisTestStatistic = fTestStatSampler->EvaluateTestStatistic(fData, *fPOI );
    /*
    std::cout << "NC CHECK: " << i << std::endl;
    point->Print();
    fPOI->Print("v");
    fData.Print();
    std::cout <<"thisTestStatistic = " << thisTestStatistic << std::endl;
    */

    // find the lower & upper thresholds on the test statistic that
    // define the acceptance region in the data

    SamplingDistribution* samplingDist=nullptr;
    double sigma;
    double upperEdgeOfAcceptance;
    double upperEdgeMinusSigma;
    double upperEdgePlusSigma;
    double lowerEdgeOfAcceptance;
    double lowerEdgeMinusSigma;
    double lowerEdgePlusSigma;
    Int_t additionalMC=0;

    // the adaptive sampling algorithm wants at least one toy event to be outside
    // of the requested pvalue including the sampling variation.  That leads to an equation
    // N-1 = (1-alpha)N + Z sqrt(N - (1-alpha)N) // for upper limit and
    // 1   = alpha N - Z sqrt(alpha N)  // for lower limit
    //
    // solving for N gives:
    // N = 1/alpha * [3/2 + sqrt(5)] for Z = 1 (which is used currently)
    // thus, a good guess for the first iteration of events is N=3.73/alpha~4/alpha
    // should replace alpha here by smaller tail probability: eg. alpha*Min(leftsideFrac, 1.-leftsideFrac)
    // totalMC will be incremented by 2 before first call, so initiated it at half the value
    Int_t totalMC = (Int_t) (2./fSize/std::min(fLeftSideFraction,1.-fLeftSideFraction));
    if(fLeftSideFraction==0. || fLeftSideFraction ==1.){
      totalMC = (Int_t) (2./fSize);
    }
    // use control
    double tmc = double(totalMC)*fAdditionalNToysFactor;
    totalMC = (Int_t) tmc;

    ToyMCSampler* toyMCSampler = dynamic_cast<ToyMCSampler*>(fTestStatSampler);
    if(fAdaptiveSampling && toyMCSampler) {
      do{
   // this will be executed first, then while conditioned checked
   // as an exit condition for the loop.

   // the next line is where most of the time will be spent
   // generating the sampling dist of the test statistic.
   additionalMC = 2*totalMC; // grow by a factor of two
   samplingDist =
     toyMCSampler->AppendSamplingDistribution(*point,
                     samplingDist,
                     additionalMC);
        if (!samplingDist) {
           oocoutE(nullptr,Eval) << "Neyman Construction: error generating sampling distribution" << std::endl;
           return nullptr;
        }
   totalMC=samplingDist->GetSize();

   //cout << "without sigma upper = " <<
   //samplingDist->InverseCDF( 1. - ((1.-fLeftSideFraction) * fSize) ) << std::endl;

   sigma = 1;
   upperEdgeOfAcceptance =
     samplingDist->InverseCDF( 1. - ((1.-fLeftSideFraction) * fSize) ,
                sigma, upperEdgePlusSigma);
   sigma = -1;
   samplingDist->InverseCDF( 1. - ((1.-fLeftSideFraction) * fSize) ,
              sigma, upperEdgeMinusSigma);

   sigma = 1;
   lowerEdgeOfAcceptance =
     samplingDist->InverseCDF( fLeftSideFraction * fSize ,
                sigma, lowerEdgePlusSigma);
   sigma = -1;
   samplingDist->InverseCDF( fLeftSideFraction * fSize ,
              sigma, lowerEdgeMinusSigma);

   ooccoutD(samplingDist,Eval) << "NeymanConstruction: "
        << "total MC = " << totalMC
        << " this test stat = " << thisTestStatistic << std::endl
        << " upper edge -1sigma = " << upperEdgeMinusSigma
        << ", upperEdge = "<<upperEdgeOfAcceptance
        << ", upper edge +1sigma = " << upperEdgePlusSigma << std::endl
        << " lower edge -1sigma = " << lowerEdgeMinusSigma
        << ", lowerEdge = "<<lowerEdgeOfAcceptance
        << ", lower edge +1sigma = " << lowerEdgePlusSigma << std::endl;
      } while((
         (thisTestStatistic <= upperEdgeOfAcceptance &&
          thisTestStatistic > upperEdgeMinusSigma)
         || (thisTestStatistic >= upperEdgeOfAcceptance &&
        thisTestStatistic < upperEdgePlusSigma)
         || (thisTestStatistic <= lowerEdgeOfAcceptance &&
        thisTestStatistic > lowerEdgeMinusSigma)
         || (thisTestStatistic >= lowerEdgeOfAcceptance &&
        thisTestStatistic < lowerEdgePlusSigma)
      ) && (totalMC < 100./fSize)
         ) ; // need ; here
    } else {
      // the next line is where most of the time will be spent
      // generating the sampling dist of the test statistic.
      samplingDist = fTestStatSampler->GetSamplingDistribution(*point);
      if (!samplingDist) {
         oocoutE(nullptr,Eval) << "Neyman Construction: error generating sampling distribution" << std::endl;
         return nullptr;
      }

      lowerEdgeOfAcceptance =
   samplingDist->InverseCDF( fLeftSideFraction * fSize );
      upperEdgeOfAcceptance =
   samplingDist->InverseCDF( 1. - ((1.-fLeftSideFraction) * fSize) );
    }

    // add acceptance region to ConfidenceBelt
    if(fConfBelt && fCreateBelt){
      //      std::cout << "conf belt set " << fConfBelt << std::endl;
      fConfBelt->AddAcceptanceRegion(*point, i,
                 lowerEdgeOfAcceptance,
                 upperEdgeOfAcceptance);
    }

    // printout some debug info
    ooccoutP(samplingDist,Eval) << "NeymanConstruction: Prog: "<< i+1<<"/"<<fPointsToTest->numEntries()
            << " total MC = " << samplingDist->GetSize()
            << " this test stat = " << thisTestStatistic << std::endl;
    ooccoutP(samplingDist,Eval) << " ";
    for (auto const *myarg : static_range_cast<RooRealVar *> (*point)){
      ooccoutP(samplingDist,Eval) << myarg->GetName() << "=" << myarg->getVal() << " ";
    }
    ooccoutP(samplingDist,Eval) << "[" << lowerEdgeOfAcceptance << ", "
             << upperEdgeOfAcceptance << "] " << " in interval = " <<
      (thisTestStatistic >= lowerEdgeOfAcceptance && thisTestStatistic <= upperEdgeOfAcceptance)
         << std::endl << std::endl;

    // Check if this data is in the acceptance region
    if(thisTestStatistic >= lowerEdgeOfAcceptance && thisTestStatistic <= upperEdgeOfAcceptance) {
      // if so, set this point to true
      //      fPointsToTest->add(*point, 1.);  // this behaves differently for Hist and DataSet
      pointsInInterval->add(*point);
      ++npass;
    }

    if(fSaveBeltToFile){
      //write to file
      samplingDist->Write();
      string tmpName = "hist_";
      tmpName+=samplingDist->GetName();
      TH1F h{tmpName.c_str(),"",500,0.,5.};
      for(int ii=0; ii<samplingDist->GetSize(); ++ii){
        h.Fill(samplingDist->GetSamplingDistribution().at(ii) );
      }
      h.Write();
    }

    delete samplingDist;
    //    delete point; // from dataset
  }
  oocoutI(pointsInInterval,Eval) << npass << " points in interval" << std::endl;

  // create an interval based pointsInInterval
  PointSetInterval* interval
    = new PointSetInterval("ClassicalConfidenceInterval", *pointsInInterval);


  if(fSaveBeltToFile){
    //   write belt to file
    fConfBelt->Write();

    f->Close();
  }

  delete f;
  //delete data;
  return interval;
}
