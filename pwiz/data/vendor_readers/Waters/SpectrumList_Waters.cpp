//
// $Id$
//
//
// Original author: Matt Chambers <matt.chambers .@. vanderbilt.edu>
//
// Copyright 2009 Vanderbilt University - Nashville, TN 37232
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#define PWIZ_SOURCE


#include "SpectrumList_Waters.hpp"


#ifdef PWIZ_READER_WATERS
#include "pwiz/utility/misc/SHA1Calculator.hpp"
#include "pwiz/utility/misc/Filesystem.hpp"
#include "pwiz/utility/misc/Std.hpp"
#include "Reader_Waters_Detail.hpp"
#include <boost/spirit/include/karma.hpp>
#include "boost/foreach_field.hpp"


namespace pwiz {
namespace msdata {
namespace detail {

using namespace Waters;

SpectrumList_Waters::SpectrumList_Waters(MSData& msd, RawDataPtr rawdata, const Reader::Config& config)
    : msd_(msd), rawdata_(rawdata), config_(config)
{
    createIndex();
}


PWIZ_API_DECL size_t SpectrumList_Waters::size() const
{
    return size_;
}


PWIZ_API_DECL const SpectrumIdentity& SpectrumList_Waters::spectrumIdentity(size_t index) const
{
    if (index>size_)
        throw runtime_error("[SpectrumList_Waters::spectrumIdentity()] Bad index: "
                            + lexical_cast<string>(index));
    return index_[index];
}


PWIZ_API_DECL size_t SpectrumList_Waters::find(const string& id) const
{
    map<string, size_t>::const_iterator scanItr = idToIndexMap_.find(id);
    if (scanItr == idToIndexMap_.end())
        return size_;
    return scanItr->second;
}

PWIZ_API_DECL SpectrumPtr SpectrumList_Waters::spectrum(size_t index, bool getBinaryData) const
{
    return spectrum(index, getBinaryData ? DetailLevel_FullData : DetailLevel_FullMetadata, 0.0, 0.0, 0.0, pwiz::util::IntegerSet());
}

PWIZ_API_DECL SpectrumPtr SpectrumList_Waters::spectrum(size_t index, DetailLevel detailLevel) const
{
    return spectrum(index, detailLevel, 0.0, 0.0, 0.0, pwiz::util::IntegerSet());
}

PWIZ_API_DECL SpectrumPtr SpectrumList_Waters::spectrum(size_t index, bool getBinaryData, const pwiz::util::IntegerSet& msLevelsToCentroid) const
{
    return spectrum(index, getBinaryData ? DetailLevel_FullData : DetailLevel_FullMetadata, 0.0, 0.0, 0.0, msLevelsToCentroid);
}

PWIZ_API_DECL SpectrumPtr SpectrumList_Waters::spectrum(size_t index, DetailLevel detailLevel, const pwiz::util::IntegerSet& msLevelsToCentroid) const
{
    return spectrum(index, detailLevel, 0.0, 0.0, 0.0, msLevelsToCentroid);
}

PWIZ_API_DECL SpectrumPtr SpectrumList_Waters::spectrum(size_t index, bool getBinaryData, double lockmassMzPosScans, double lockmassMzNegScans, double lockmassTolerance, const pwiz::util::IntegerSet& msLevelsToCentroid) const
{
    return spectrum(index, getBinaryData ? DetailLevel_FullData : DetailLevel_FullMetadata, lockmassMzPosScans, lockmassMzNegScans, lockmassTolerance, msLevelsToCentroid);
}

PWIZ_API_DECL SpectrumPtr SpectrumList_Waters::spectrum(size_t index, DetailLevel detailLevel, double lockmassMzPosScans, double lockmassMzNegScans, double lockmassTolerance, const pwiz::util::IntegerSet& msLevelsToCentroid) const
{
    boost::lock_guard<boost::mutex> lock(readMutex);  // lock_guard will unlock mutex when out of scope or when exception thrown (during destruction)
    if (index >= size_)
        throw runtime_error("[SpectrumList_Waters::spectrum()] Bad index: "
                            + lexical_cast<string>(index));

    // allocate a new Spectrum
    SpectrumPtr result(new Spectrum);
    if (!result.get())
        throw runtime_error("[SpectrumList_Waters::spectrum()] Allocation error.");

    IndexEntry& ie = index_[index];

    result->index = ie.index;
    result->id = ie.id;
    int scanStatIndex = ie.block >= 0 ? ie.block : ie.scan; // ion mobility scans get stats based on block index

    // TODO: fix Serializer_mzXML so it supports non-default sourceFilePtrs
    //size_t sourceFileIndex = ie.functionPtr->getFunctionNumber() - 1;
    //result->sourceFilePtr = msd_.fileDescription.sourceFilePtrs[sourceFileIndex];

    /*float laserAimX = pExScanStats_->LaserAimXPos;
    float laserAimY = pExScanStats_->LaserAimYPos;
    //if (scanInfo->ionizationType() == IonizationType_MALDI)
    {
        result->spotID = (format("%dx%d") % laserAimX % laserAimY).str();
    }*/

    result->scanList.set(MS_no_combination);
    result->scanList.scans.push_back(Scan());
    Scan& scan = result->scanList.scans[0];
    scan.instrumentConfigurationPtr = msd_.run.defaultInstrumentConfigurationPtr;

    //scan.instrumentConfigurationPtr =
        //findInstrumentConfiguration(msd_, translate(scanInfo->massAnalyzerType()));

    int msLevel;
    CVID spectrumType;
    translateFunctionType(WatersToPwizFunctionType(rawdata_->Info.GetFunctionType(ie.function)), msLevel, spectrumType);

    result->set(spectrumType);
    result->set(MS_ms_level, msLevel);

    scan.set(MS_preset_scan_configuration, ie.function+1);

    if (detailLevel == DetailLevel_InstantMetadata)
        return result;

    scan.set(MS_scan_start_time, rawdata_->Info.GetRetentionTime(ie.function, scanStatIndex), UO_minute);

    boost::weak_ptr<RawData> binaryDataSource = rawdata_;
    if (msLevelsToCentroid.contains(msLevel))
    {
        rawdata_->Centroid();
        binaryDataSource = rawdata_->CentroidRawDataFile();
    }

    PwizPolarityType polarityType = WatersToPwizPolarityType(rawdata_->Info.GetIonMode(ie.function));
    if (polarityType != PolarityType_Unknown)
        result->set(translate(polarityType));

    double lockmassMz = (polarityType == PolarityType_Negative) ? lockmassMzNegScans : lockmassMzPosScans;
    if (lockmassMz != 0.0)
    {
        if (!rawdata_->ApplyLockMass(lockmassMz, lockmassTolerance)) // TODO: if false (cannot apply lockmass), log a warning
            warn_once("[SpectrumList_Waters] failed to apply lockmass correction");
    }
    else
        rawdata_->RemoveLockMass();

    if (rawdata_->Info.IsContinuum(ie.function))
        result->set(MS_profile_spectrum);
    else
        result->set(MS_centroid_spectrum);
    
    // block >= 0 is ion mobility
    if (ie.block < 0)
    {
        // scanStats values don't match the ion mobility data arrays
        // CONSIDER: in the ion mobility case, get these values from the actual data arrays
        result->set(MS_base_peak_m_z, rawdata_->GetScanStat<double>(ie.function, ie.scan, MassLynxScanItem::BASE_PEAK_MASS));
        result->set(MS_base_peak_intensity, rawdata_->GetScanStat<double>(ie.function, ie.scan, MassLynxScanItem::BASE_PEAK_INTENSITY));
        result->set(MS_total_ion_current, rawdata_->GetScanStat<double>(ie.function, ie.scan, MassLynxScanItem::TOTAL_ION_CURRENT));
        result->defaultArrayLength = rawdata_->GetScanStat<int>(ie.function, ie.scan, MassLynxScanItem::PEAKS_IN_SCAN);
    }
    else
    {
        result->set(MS_total_ion_current, rawdata_->TicByFunctionIndex()[ie.function].at(ie.block));
        result->defaultArrayLength = 0;
    }

    float minMZ, maxMZ;
    rawdata_->Info.GetAcquisitionMassRange(ie.function, minMZ, maxMZ);
    scan.scanWindows.push_back(ScanWindow(minMZ, maxMZ, MS_m_z));

    if (detailLevel < DetailLevel_FastMetadata)
        return result;

    // block >= 0 is ion mobility
    if (ie.block >= 0)
    {
        double driftTime = rawdata_->GetDriftTime(ie.function, ie.scan);
        scan.set(MS_ion_mobility_drift_time, driftTime, UO_millisecond);
    }

    if (msLevel > 1)
    {
        string setMassStr = rawdata_->GetScanStat(ie.function, scanStatIndex, MassLynxScanItem::SET_MASS);

        if (!setMassStr.empty())
        {
            double setMass = lexical_cast<double>(setMassStr);
            if (setMass > 0)
            {
                Precursor precursor;
                SelectedIon selectedIon(setMass);
                precursor.isolationWindow.set(MS_isolation_window_target_m_z, setMass, MS_m_z);

                precursor.activation.set(MS_beam_type_collision_induced_dissociation); // AFAIK there is no Waters instrument with a trap collision cell

                string collisionEnergyStr = rawdata_->GetScanStat(ie.function, scanStatIndex, MassLynxScanItem::COLLISION_ENERGY);
                if (!collisionEnergyStr.empty())
                {
                    double collisionEnergy = lexical_cast<double>(collisionEnergyStr);
                    if (collisionEnergy > 0)
                        precursor.activation.set(MS_collision_energy, collisionEnergy, UO_electronvolt);
                }

                precursor.selectedIons.push_back(selectedIon);
                result->precursors.push_back(precursor);
            }
        }
    }

    if (detailLevel < DetailLevel_FullMetadata)
        return result;

    if (detailLevel == DetailLevel_FullData || detailLevel == DetailLevel_FullMetadata)
    {
        vector<float> masses, intensities;

        if (ie.block >= 0)
        {
            MassLynxRawScanReader& scanReader = binaryDataSource.lock()->GetCompressedDataClusterForBlock(ie.function, ie.block);
            scanReader.ReadScan(ie.function, ie.block, ie.scan, masses, intensities);
            result->defaultArrayLength = masses.size();

            if (detailLevel == DetailLevel_FullMetadata)
                return result;
        }
        else // not ion mobility
        {
            if (detailLevel != DetailLevel_FullMetadata)
                binaryDataSource.lock()->Reader.ReadScan(ie.function, ie.scan, masses, intensities);
        }

        vector<double> mzArray(masses.begin(), masses.end());
        vector<double> intensityArray(intensities.begin(), intensities.end());
        result->swapMZIntensityArrays(mzArray, intensityArray, MS_number_of_detector_counts); // Donate mass and intensity buffers to result vectors
    }

    return result;
}


PWIZ_API_DECL bool SpectrumList_Waters::hasSonarFunctions() const
{
    for (int function : rawdata_->FunctionIndexList())
        if (rawdata_->SonarEnabledByFunctionIndex()[function])
            return true;
    return false;
}

PWIZ_API_DECL pair<int, int> SpectrumList_Waters::sonarMzToDriftBinRange(int function, float precursorMz, float precursorTolerance) const
{
    pair<int, int> binRange;
    rawdata_->Info.GetSonarRange(function, precursorMz, precursorTolerance, binRange.first, binRange.second);
    return binRange;
}

PWIZ_API_DECL void SpectrumList_Waters::initializeCoefficients() const
{
    if (calibrationCoefficients_.empty())
    {    
        // the header property is something like: 6.08e-3,9.99e-1,-2.84e-6,7.63e-8,-6.91e-10,T1
        string coefficients = rawdata_->GetHeaderProp("Cal Function 1");
        vector<string> tokens;
        bal::split(tokens, coefficients, bal::is_any_of(","));
        if (tokens.size() < 5) // I don't know what the T1 is for, but it doesn't seem important
            throw runtime_error("[SpectrumList_Waters::spectrum()] error parsing calibration coefficients: " + coefficients);
        calibrationCoefficients_.resize(5);
        try
        {
            for (size_t i=0; i < 5; ++i)
                calibrationCoefficients_[i] = lexical_cast<double>(tokens[i]);
        }
        catch (bad_lexical_cast&)
        {
            throw runtime_error("[SpectrumList_Waters::spectrum()] error parsing calibration coefficients: " + coefficients);
        }
    }


}

PWIZ_API_DECL double SpectrumList_Waters::calibrate(const double& mz) const
{
    const vector<double>& c = calibrationCoefficients_;
    double sqrtMz = sqrt(mz);
    double x = c[0] + c[1]*sqrtMz + c[2]*mz + c[3]*mz*sqrtMz + c[4]*mz*mz;
    return x*x;
}


PWIZ_API_DECL pwiz::analysis::Spectrum3DPtr SpectrumList_Waters::spectrum3d(double scanStartTime, const boost::icl::interval_set<double>& driftTimeRanges) const
{
    pwiz::analysis::Spectrum3DPtr result(new pwiz::analysis::Spectrum3D);

    if (scanTimeToFunctionAndBlockMap_.empty()) // implies no ion mobility data
        return result;

    boost::container::flat_map<double, vector<pair<int, int> > >::const_iterator findItr = scanTimeToFunctionAndBlockMap_.lower_bound(floor(scanStartTime * 1e6) / 1e6);
    if (findItr == scanTimeToFunctionAndBlockMap_.end() || findItr->first - 1e-6 > scanStartTime)
        return result;

    for (size_t pair = 0; pair < findItr->second.size(); ++pair)
    {
        int function = findItr->second[pair].first;
        int block = findItr->second[pair].first;

        //int axisLength = 0, nonZeroDataPoints = 0;
        int numScansInBlock = 0;

        MassLynxRawScanReader& cdc = rawdata_->GetCompressedDataClusterForBlock(function, block);
        vector<float>& imsMasses = imsMasses_;
        vector<float>& imsIntensities = imsIntensities_;

        numScansInBlock = rawdata_->Info.GetDriftScanCount(function);

        for (int scan = 0; scan < numScansInBlock; ++scan)
        {
            double driftTime = rawdata_->GetDriftTime(function, scan);

            if (driftTimeRanges.find(driftTime) == driftTimeRanges.end())
                continue;

            cdc.ReadScan(function, block, scan, imsMasses, imsIntensities);

            boost::container::flat_map<double, float>& driftSpectrum = (*result)[driftTime];

            //driftSpectrum[imsCalibratedMasses_[imsMasses[0] - 1]] = 0;
            for (int i = 0, end = imsMasses.size(); i < end; ++i)
            {
                driftSpectrum[imsMasses[i]] = imsIntensities[i];
            }
            //driftSpectrum[imsCalibratedMasses_[massIndices[nonZeroDataPoints - 1] + 1]] = 0;
        }
    }

    return result;
}


PWIZ_API_DECL void SpectrumList_Waters::createIndex()
{
    using namespace boost::spirit::karma;

    multimap<float, std::pair<int, int> > functionAndScanByRetentionTime;

    int numScansInBlock = 0;   // number of drift scans per compressed block

    for(int function : rawdata_->FunctionIndexList())
    {
        int msLevel;
        CVID spectrumType;

        try { translateFunctionType(WatersToPwizFunctionType(rawdata_->Info.GetFunctionType(function)), msLevel, spectrumType); }
        catch(...) // unable to translate function type
        {
            cerr << "[SpectrumList_Waters::createIndex] Unable to translate function type \"" + rawdata_->Info.GetFunctionTypeString(rawdata_->Info.GetFunctionType(function)) + "\"" << endl;
            continue;
        }

        /*cout << "Scan items for function " << function << ":" << endl;
        vector<MassLynxScanItem> items = rawdata_->Info.GetAvailableScanItems(function);
        vector<std::string> itemNames = rawdata_->Info.GetScanItemString(items);
        for (auto item : itemNames)
        {
            cout << "  " << item << "\n";
        }*/

        if (spectrumType == MS_SRM_spectrum ||
            spectrumType == MS_SIM_spectrum ||
            spectrumType == MS_constant_neutral_loss_spectrum ||
            spectrumType == MS_constant_neutral_gain_spectrum)
            continue;

        int scanCount = rawdata_->Info.GetScansInFunction(function);

        if (!config_.combineIonMobilitySpectra && rawdata_->IonMobilityByFunctionIndex()[function])
        {
            numScansInBlock = rawdata_->Info.GetDriftScanCount(function);

            scanTimeToFunctionAndBlockMap_.reserve(scanCount);

            for (int i = 0; i < scanCount; ++i)
            {
                scanTimeToFunctionAndBlockMap_[rawdata_->Info.GetRetentionTime(function, i) * 60].push_back(make_pair(function, i));
                functionAndScanByRetentionTime.insert(make_pair(rawdata_->Info.GetRetentionTime(function, i), make_pair(function, i)));
            }
        }
        else
        {
            for (int i = 0; i < scanCount; ++i)
                functionAndScanByRetentionTime.insert(make_pair(rawdata_->Info.GetRetentionTime(function, i), make_pair(function, i)));
        }
    }

    typedef pair<int, int> FunctionScanPair;
    BOOST_FOREACH_FIELD((float rt)(const FunctionScanPair& functionScanPair), functionAndScanByRetentionTime)
    {
        if (!config_.combineIonMobilitySpectra && rawdata_->IonMobilityByFunctionIndex()[functionScanPair.first])
        {
            for (int j = 0; j < numScansInBlock; ++j)
            {
                index_.push_back(IndexEntry());
                IndexEntry& ie = index_.back();
                ie.function = functionScanPair.first;
                ie.process = 0;
                ie.block = functionScanPair.second;
                ie.scan = j;
                ie.index = index_.size() - 1;

                std::back_insert_iterator<std::string> sink(ie.id);
                generate(sink,
                         "function=" << int_ << " process=" << int_ << " scan=" << int_,
                         (ie.function + 1), ie.process, ((numScansInBlock*ie.block) + ie.scan + 1));
                idToIndexMap_[ie.id] = ie.index;
            }
        }
        else
        {
            index_.push_back(IndexEntry());
            IndexEntry& ie = index_.back();
            ie.function = functionScanPair.first;
            ie.process = 0;
            ie.block = -1; // block < 0 is not ion mobility
            ie.scan = functionScanPair.second;
            ie.index = index_.size() - 1;

            std::back_insert_iterator<std::string> sink(ie.id);
            generate(sink,
                     "function=" << int_ << " process=" << int_ << " scan=" << int_,
                     (ie.function + 1), ie.process, (ie.scan + 1));
            idToIndexMap_[ie.id] = ie.index;
        }
        rt = 0; // suppress warning
    }

    size_ = index_.size();
}


} // detail
} // msdata
} // pwiz

#else // PWIZ_READER_WATERS

//
// non-MSVC implementation
//

namespace pwiz {
namespace msdata {
namespace detail {

namespace {const SpectrumIdentity emptyIdentity;}

size_t SpectrumList_Waters::size() const {return 0;}
const SpectrumIdentity& SpectrumList_Waters::spectrumIdentity(size_t index) const {return emptyIdentity;}
size_t SpectrumList_Waters::find(const std::string& id) const {return 0;}
SpectrumPtr SpectrumList_Waters::spectrum(size_t index, bool getBinaryData) const {return SpectrumPtr();}
SpectrumPtr SpectrumList_Waters::spectrum(size_t index, DetailLevel detailLevel) const { return SpectrumPtr(); }
SpectrumPtr SpectrumList_Waters::spectrum(size_t index, bool getBinaryData, const pwiz::util::IntegerSet& msLevelsToCentroid) const { return SpectrumPtr(); }
SpectrumPtr SpectrumList_Waters::spectrum(size_t index, DetailLevel detailLevel, const pwiz::util::IntegerSet& msLevelsToCentroid) const { return SpectrumPtr(); }
SpectrumPtr SpectrumList_Waters::spectrum(size_t index, bool getBinaryData, double lockmassMzPosScans, double lockmassMzNegScans, double lockmassTolerance, const pwiz::util::IntegerSet& msLevelsToCentroid) const { return SpectrumPtr(); }
SpectrumPtr SpectrumList_Waters::spectrum(size_t index, DetailLevel detailLevel, double lockmassMzPosScans, double lockmassMzNegScans, double lockmassTolerance, const pwiz::util::IntegerSet& msLevelsToCentroid) const { return SpectrumPtr(); }
pwiz::analysis::Spectrum3DPtr SpectrumList_Waters::spectrum3d(double scanStartTime, const boost::icl::interval_set<double>& driftTimeRanges) const { return pwiz::analysis::Spectrum3DPtr(); }
bool SpectrumList_Waters::hasSonarFunctions() const { return false; }
pair<int, int> SpectrumList_Waters::sonarMzToDriftBinRange(int function, float precursorMz, float precursorTolerance) const { return pair<int, int>(); }

} // detail
} // msdata
} // pwiz

#endif // PWIZ_READER_WATERS
