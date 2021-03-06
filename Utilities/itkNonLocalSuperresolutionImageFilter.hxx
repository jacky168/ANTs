/*=========================================================================
 *
 *  Copyright Insight Software Consortium
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0.txt
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 *=========================================================================*/
#ifndef itkNonLocalSuperresolutionImageFilter_hxx
#define itkNonLocalSuperresolutionImageFilter_hxx

#include "itkNonLocalSuperresolutionImageFilter.h"

#include "itkArray.h"
#include "itkBoxMeanImageFilter.h"
#include "itkBSplineInterpolateImageFunction.h"
#include "itkDivideImageFilter.h"
#include "itkImageDuplicator.h"
#include "itkImageRegionConstIterator.h"
#include "itkImageRegionIterator.h"
#include "itkImageRegionIteratorWithIndex.h"
#include "itkIterationReporter.h"
#include "itkLinearInterpolateImageFunction.h"
#include "itkNearestNeighborInterpolateImageFunction.h"
#include "itkMath.h"
#include "itkNeighborhoodIterator.h"
#include "itkResampleImageFilter.h"
#include "itkSubtractImageFilter.h"

#include "itkProgressReporter.h"

#include <algorithm>
#include <numeric>

namespace itk {

template <typename TInputImage, typename TOutputImage>
NonLocalSuperresolutionImageFilter<TInputImage, TOutputImage>
::NonLocalSuperresolutionImageFilter() :
  m_Epsilon( 0.01 ),
  m_PatchSimilaritySigma( 1.0 ),
  m_IntensityDifferenceSigma( 1.0 ),
  m_CurrentIteration( 0 )
{
  this->SetNumberOfRequiredInputs( 2 );

  this->m_WeightSumImage = ITK_NULLPTR;

  this->m_InterpolatedLowResolutionInputImage = ITK_NULLPTR;

  // Interpolator --- default to linear
  typedef LinearInterpolateImageFunction<InputImageType, RealType> LinearInterpolatorType;
  this->m_Interpolator = LinearInterpolatorType::New();

  this->m_SimilarityMetric = MEAN_SQUARES;

  this->m_NeighborhoodPatchRadius.Fill( 1 );
  this->m_NeighborhoodSearchRadius.Fill( 3 );
}

template<typename TInputImage, typename TOutputImage>
void
NonLocalSuperresolutionImageFilter<TInputImage, TOutputImage>
::VerifyInputInformation()
{
}

template<typename TInputImage, typename TOutputImage>
void
NonLocalSuperresolutionImageFilter<TInputImage, TOutputImage>
::AllocateOutputs()
{
  typename OutputImageType::Pointer outputImage = this->GetOutput();
  outputImage->CopyInformation( this->GetHighResolutionReferenceImage() );
  outputImage->SetRegions( this->GetHighResolutionReferenceImage()->GetBufferedRegion() );
  outputImage->Allocate();
  outputImage->FillBuffer( 0 );
}

template<typename TInputImage, typename TOutputImage>
void
NonLocalSuperresolutionImageFilter<TInputImage, TOutputImage>
::GenerateOutputInformation()
{
  // Call the superclass' implementation of this method
  Superclass::GenerateOutputInformation();

  // Get pointers to the input and output
  OutputImageType *outputImage = this->GetOutput();
  if( !outputImage )
    {
    return;
    }

  const InputImageType *referenceImage = this->GetHighResolutionReferenceImage();

  // Set the size of the output region
  if( referenceImage )
    {
    outputImage->SetLargestPossibleRegion(
      referenceImage->GetLargestPossibleRegion() );
    outputImage->SetSpacing( referenceImage->GetSpacing() );
    outputImage->SetOrigin( referenceImage->GetOrigin() );
    outputImage->SetDirection( referenceImage->GetDirection() );
    }
}

template<typename TInputImage, typename TOutputImage>
void
NonLocalSuperresolutionImageFilter<TInputImage, TOutputImage>
::GenerateInputRequestedRegion()
{
  // Call the superclass' implementation of this method
  Superclass::GenerateInputRequestedRegion();

  if ( !this->GetInput() )
    {
    return;
    }

  // Get pointers to the input
  InputImagePointer inputPtr  =
    const_cast< TInputImage * >( this->GetInput() );

  // Determining the actual input region is non-trivial, especially
  // when we cannot assume anything about the transform being used.
  // So we do the easy thing and request the entire input image.
  //
  inputPtr->SetRequestedRegionToLargestPossibleRegion();
}

template <class TInputImage, class TOutputImage>
void
NonLocalSuperresolutionImageFilter<TInputImage, TOutputImage>
::GenerateData()
{

  if( this->m_ScaleLevels.size() == 0 )
    {
    itkExceptionMacro( "There are no scale levels." );
    }

  IterationReporter reporter( this, 0, 1 );

  this->m_CurrentIteration = 0;
  while( this->m_CurrentIteration < this->m_ScaleLevels.size() )
    {
    reporter.CompletedStep();

    this->BeforeThreadedGenerateData();

    typename ImageSource<TOutputImage>::ThreadStruct str1;
    str1.Filter = this;

    this->GetMultiThreader()->SetNumberOfThreads( this->GetNumberOfThreads() );
    this->GetMultiThreader()->SetSingleMethod( this->ThreaderCallback, &str1 );

    this->GetMultiThreader()->SingleMethodExecute();

    this->AfterThreadedGenerateData();

    this->m_CurrentIteration++;
    }
}

template<typename TInputImage, typename TOutputImage>
void
NonLocalSuperresolutionImageFilter<TInputImage, TOutputImage>
::BeforeThreadedGenerateData()
{
  if( this->m_CurrentIteration == 0 )
    {
    this->m_Interpolator->SetInputImage( this->GetLowResolutionInputImage() );

    typedef IdentityTransform<RealType, ImageDimension> IdentityTransformType;
    typename IdentityTransformType::Pointer identityTransform = IdentityTransformType::New();
    identityTransform->SetIdentity();

    typedef ResampleImageFilter<InputImageType, InputImageType, RealType> ResamplerType;
    typename ResamplerType::Pointer resampler = ResamplerType::New();
    resampler->SetInterpolator( this->m_Interpolator );
    resampler->SetInput( this->GetLowResolutionInputImage() );
    resampler->SetTransform( identityTransform );
    resampler->SetOutputParametersFromImage( this->GetHighResolutionReferenceImage() );
    resampler->Update();

    this->m_InterpolatedLowResolutionInputImage = resampler->GetOutput();

    // Initialize the weight sum image
    this->m_WeightSumImage = RealImageType::New();
    this->m_WeightSumImage->CopyInformation( this->GetHighResolutionReferenceImage() );
    this->m_WeightSumImage->SetRegions( this->GetHighResolutionReferenceImage()->GetBufferedRegion() );
    this->m_WeightSumImage->SetLargestPossibleRegion( this->GetHighResolutionReferenceImage()->GetLargestPossibleRegion() );
    this->m_WeightSumImage->Allocate();
    this->m_WeightSumImage->FillBuffer( 1.0 );

    // Determine the search and patch offset lists

    ConstNeighborhoodIterator<InputImageType> It( this->m_NeighborhoodSearchRadius,
      this->GetHighResolutionReferenceImage(), this->GetHighResolutionReferenceImage()->GetBufferedRegion() );

    this->m_NeighborhoodSearchOffsetList.clear();

    this->m_NeighborhoodSearchSize = ( It.GetNeighborhood() ).Size();
    for( unsigned int n = 0; n < this->m_NeighborhoodSearchSize; n++ )
      {
      this->m_NeighborhoodSearchOffsetList.push_back( ( It.GetNeighborhood() ).GetOffset( n ) );
      }

    ConstNeighborhoodIterator<InputImageType> It2( this->m_NeighborhoodPatchRadius,
      this->GetHighResolutionReferenceImage(), this->GetHighResolutionReferenceImage()->GetBufferedRegion() );

    this->m_NeighborhoodPatchOffsetList.clear();

    this->m_NeighborhoodPatchSize = ( It2.GetNeighborhood() ).Size();
    for( unsigned int n = 0; n < this->m_NeighborhoodPatchSize; n++ )
      {
      this->m_NeighborhoodPatchOffsetList.push_back( ( It2.GetNeighborhood() ).GetOffset( n ) );
      }

    this->m_TargetImageRequestedRegion = this->GetHighResolutionReferenceImage()->GetBufferedRegion();

    this->AllocateOutputs();
    }
  else
    {
    typedef CastImageFilter<OutputImageType, InputImageType> CasterType;
    typename CasterType::Pointer caster = CasterType::New();
    caster->SetInput( this->GetOutput() );
    caster->Update();

    this->m_InterpolatedLowResolutionInputImage = caster->GetOutput();

    this->m_WeightSumImage->FillBuffer( 1.0 );
    }

}

template<typename TInputImage, typename TOutputImage>
void
NonLocalSuperresolutionImageFilter<TInputImage, TOutputImage>
::ThreadedGenerateData( const RegionType &region, ThreadIdType threadId )
{
  ProgressReporter progress( this, threadId, region.GetNumberOfPixels(), 100 );

  const InputImageType *highResolutionInputImage = this->GetInput( 1 );

  OutputImageType *outputImage = this->GetOutput();

  std::vector<NeighborhoodOffsetType> searchNeighborhoodOffsetList
    = this->m_NeighborhoodSearchOffsetList;
  SizeValueType searchNeighborhoodSize = searchNeighborhoodOffsetList.size();

  // This is used for future extensions to include multiple high reference images

  InputImageList highResolutionInputImageList;
  highResolutionInputImageList.push_back( const_cast<InputImageType *>( highResolutionInputImage ) );

  ConstNeighborhoodIteratorType It( this->m_NeighborhoodPatchRadius, highResolutionInputImage, region );

  for( It.GoToBegin(); !It.IsAtEnd(); ++It )
    {
    progress.CompletedPixel();

    IndexType currentCenterIndex = It.GetIndex();

    InputImagePixelVectorType highResolutionPatch =
      this->VectorizeImageListPatch( highResolutionInputImageList, currentCenterIndex, true );

    for( SizeValueType i = 0; i < searchNeighborhoodSize; i++ )
      {
      IndexType searchIndex = currentCenterIndex + searchNeighborhoodOffsetList[i];

      if( searchIndex == currentCenterIndex )
        {
        continue;
        }

      if( !outputImage->GetBufferedRegion().IsInside( searchIndex ) )
        {
        continue;
        }

      RealType intensityDifference =
        It.GetCenterPixel() - highResolutionInputImage->GetPixel( searchIndex );

      if( std::fabs( intensityDifference ) > 3.0 * this->m_IntensityDifferenceSigma *
        vnl_math_sqr( this->m_ScaleLevels[this->m_CurrentIteration] ) )
        {
        continue;
        }

      RealType patchSimilarity = this->ComputeNeighborhoodPatchSimilarity(
        highResolutionInputImageList, searchIndex, highResolutionPatch, true );

      RealType intensityWeight = vnl_math_sqr( intensityDifference /
        ( this->m_IntensityDifferenceSigma * this->m_ScaleLevels[this->m_CurrentIteration] ) );

      RealType patchWeight = vnl_math_sqr( patchSimilarity /
        ( this->m_PatchSimilaritySigma * this->m_ScaleLevels[this->m_CurrentIteration] ) );

      RealType weight = std::exp( -( intensityWeight + patchWeight ) );

      outputImage->SetPixel( currentCenterIndex, outputImage->GetPixel( currentCenterIndex )
        + weight * this->m_InterpolatedLowResolutionInputImage->GetPixel( searchIndex ) );

      this->m_WeightSumImage->SetPixel( currentCenterIndex,
        this->m_WeightSumImage->GetPixel( currentCenterIndex ) + weight );
      }
    }
}

template<typename TInputImage, typename TOutputImage>
void
NonLocalSuperresolutionImageFilter<TInputImage, TOutputImage>
::AfterThreadedGenerateData()
{
  OutputImageType * outputImage = this->GetOutput();

  typedef DivideImageFilter<OutputImageType, RealImageType, OutputImageType> DividerType;
  typename DividerType::Pointer divider = DividerType::New();
  divider->SetInput1( outputImage );
  divider->SetInput2( this->m_WeightSumImage );
  divider->Update();

  this->SetNthOutput( 0, divider->GetOutput() );

  this->PerformMeanCorrection();
}

template<typename TInputImage, typename TOutputImage>
void
NonLocalSuperresolutionImageFilter<TInputImage, TOutputImage>
::PerformMeanCorrection()
{
  OutputImageType *outputImage = this->GetOutput();

  typedef BoxMeanImageFilter<TOutputImage, TInputImage> BoxMeanFilterType;
  typename BoxMeanFilterType::Pointer boxMeanFilter = BoxMeanFilterType::New();
  boxMeanFilter->SetInput( outputImage );

  typename InputImageType::SpacingType lowResolutionSpacing =
    this->GetLowResolutionInputImage()->GetSpacing();
  typename InputImageType::SpacingType highResolutionSpacing =
    this->GetHighResolutionReferenceImage()->GetSpacing();

  typename BoxMeanFilterType::RadiusType boxRadius;
  for( SizeValueType d = 0; d < ImageDimension; d++ )
    {
    boxRadius[d] = static_cast<SizeValueType>(
      std::ceil( highResolutionSpacing[d] / lowResolutionSpacing[d] ) ) - 1;
    }
  boxMeanFilter->SetRadius( boxRadius );
  boxMeanFilter->Update();

  typedef NearestNeighborInterpolateImageFunction<InputImageType, RealType> NearestNeighborInterpolatorType;
  typename NearestNeighborInterpolatorType::Pointer nearestNeighborInterpolator =
    NearestNeighborInterpolatorType::New();

  nearestNeighborInterpolator->SetInputImage( boxMeanFilter->GetOutput() );

  typedef IdentityTransform<RealType, ImageDimension> IdentityTransformType;
  typename IdentityTransformType::Pointer identityTransform = IdentityTransformType::New();
  identityTransform->SetIdentity();

  typedef ResampleImageFilter<InputImageType, InputImageType, RealType> ResamplerType;
  typename ResamplerType::Pointer resampler = ResamplerType::New();
  resampler->SetInterpolator( nearestNeighborInterpolator );
  resampler->SetInput( boxMeanFilter->GetOutput() );
  resampler->SetTransform( identityTransform );
  resampler->SetOutputParametersFromImage( this->GetLowResolutionInputImage() );

  typedef SubtractImageFilter<InputImageType> SubtracterType;
  typename SubtracterType::Pointer subtracter = SubtracterType::New();
  subtracter->SetInput1( resampler->GetOutput() );
  subtracter->SetInput2( this->GetLowResolutionInputImage() );
  subtracter->Update();

  nearestNeighborInterpolator->SetInputImage( subtracter->GetOutput() );

  typedef ResampleImageFilter<InputImageType, OutputImageType, RealType> ResamplerType2;
  typename ResamplerType2::Pointer resampler2 = ResamplerType2::New();
  resampler2->SetInterpolator( nearestNeighborInterpolator );
  resampler2->SetInput( subtracter->GetOutput() );
  resampler2->SetTransform( identityTransform );
  resampler2->SetOutputParametersFromImage( this->GetHighResolutionReferenceImage() );

  typedef SubtractImageFilter<OutputImageType> SubtracterType2;
  typename SubtracterType2::Pointer subtracter2 = SubtracterType2::New();
  subtracter2->SetInput1( outputImage );
  subtracter2->SetInput2( resampler2->GetOutput() );
  subtracter2->Update();

  ImageRegionIteratorWithIndex<OutputImageType> It( subtracter2->GetOutput(),
    subtracter2->GetOutput()->GetRequestedRegion() );
  for( It.GoToBegin(); !It.IsAtEnd(); ++It )
    {
    if( It.Get() < NumericTraits<OutputPixelType>::ZeroValue() )
      {
      It.Set( static_cast<OutputPixelType>(
        this->m_InterpolatedLowResolutionInputImage->GetPixel( It.GetIndex() ) ) );
      }
    }

  this->SetNthOutput( 0, subtracter2->GetOutput() );
}

template <class TInputImage, class TOutputImage>
typename NonLocalSuperresolutionImageFilter<TInputImage, TOutputImage>::InputImagePixelVectorType
NonLocalSuperresolutionImageFilter<TInputImage, TOutputImage>
::VectorizeImageListPatch( const InputImageList &imageList, const IndexType index, const bool normalize )
{
  InputImagePixelVectorType patchVector( this->m_NeighborhoodPatchSize * imageList.size() );
  for( unsigned int i = 0; i < imageList.size(); i++ )
    {
    InputImagePixelVectorType patchVectorPerModality = this->VectorizeImagePatch( imageList[i], index, normalize );
    for( unsigned int j = 0; j < this->m_NeighborhoodPatchSize; j++ )
      {
      patchVector[i * this->m_NeighborhoodPatchSize + j] = patchVectorPerModality[j];
      }
    }
  return patchVector;
}

template <class TInputImage, class TOutputImage>
typename NonLocalSuperresolutionImageFilter<TInputImage, TOutputImage>::InputImagePixelVectorType
NonLocalSuperresolutionImageFilter<TInputImage, TOutputImage>
::VectorizeImagePatch( const InputImagePointer image, const IndexType index, const bool normalize )
{
  InputImagePixelVectorType patchVector( this->m_NeighborhoodPatchSize );
  for( SizeValueType i = 0; i < this->m_NeighborhoodPatchSize; i++ )
    {
    IndexType neighborhoodIndex = index + this->m_NeighborhoodPatchOffsetList[i];

    bool isInBounds = this->m_TargetImageRequestedRegion.IsInside( neighborhoodIndex );
    if( isInBounds )
      {
      InputPixelType pixel = image->GetPixel( neighborhoodIndex );
      patchVector[i] = pixel;
      }
    else
      {
      patchVector[i] = std::numeric_limits<RealType>::quiet_NaN();
      }
    }

  if( normalize )
    {
    RealType mean = 0.0;
    RealType standardDeviation = 0.0;
    this->GetMeanAndStandardDeviationOfVectorizedImagePatch( patchVector, mean, standardDeviation );

    standardDeviation = std::max( standardDeviation, NumericTraits<RealType>::OneValue() );

    typename InputImagePixelVectorType::iterator it;
    for( it = patchVector.begin(); it != patchVector.end(); ++it )
      {
      *it = ( *it - mean ) / standardDeviation;
      }
    }
  return patchVector;
}

template <class TInputImage, class TOutputImage>
void
NonLocalSuperresolutionImageFilter<TInputImage, TOutputImage>
::GetMeanAndStandardDeviationOfVectorizedImagePatch(
  const InputImagePixelVectorType &patchVector, RealType &mean, RealType &standardDeviation )
{
  RealType sum = 0.0;
  RealType sumOfSquares = 0.0;
  RealType count = 0.0;

  typename InputImagePixelVectorType::const_iterator it;
  for( it = patchVector.begin(); it != patchVector.end(); ++it )
    {
    if( std::isfinite( *it ) )
      {
      sum += *it;
      sumOfSquares += vnl_math_sqr( *it );
      count += 1.0;
      }
    }

  mean = sum / count;
  standardDeviation = std::sqrt( ( sumOfSquares - count * vnl_math_sqr( mean ) ) / ( count - 1.0 ) );
}

template <class TInputImage, class TOutputImage>
typename NonLocalSuperresolutionImageFilter<TInputImage, TOutputImage>::RealType
NonLocalSuperresolutionImageFilter<TInputImage, TOutputImage>
::ComputeNeighborhoodPatchSimilarity( const InputImageList &imageList, const IndexType index,
  const InputImagePixelVectorType &patchVectorY, const bool useOnlyFirstImage )
{
  unsigned int numberOfImagesToUse = imageList.size();
  if( useOnlyFirstImage )
    {
    numberOfImagesToUse = 1;
    }

  RealType sumX = 0.0;
  RealType sumOfSquaresX = 0.0;
  RealType sumOfSquaredDifferencesXY = 0.0;
  RealType sumXY = 0.0;
  RealType N = 0.0;

  SizeValueType count = 0;
  for( SizeValueType i = 0; i < numberOfImagesToUse; i++ )
    {
    for( SizeValueType j = 0; j < this->m_NeighborhoodPatchSize; j++ )
      {
      IndexType neighborhoodIndex = index + this->m_NeighborhoodPatchOffsetList[j];

      bool isInBounds = this->m_TargetImageRequestedRegion.IsInside( neighborhoodIndex );
      if( isInBounds && std::isfinite( patchVectorY[count] ) )
        {
        RealType x = static_cast<RealType>( imageList[i]->GetPixel( neighborhoodIndex ) );
        RealType y = static_cast<RealType>( patchVectorY[count] );

        sumX += x;
        sumOfSquaresX += vnl_math_sqr( x );
        sumXY += ( x * y );

        sumOfSquaredDifferencesXY += vnl_math_sqr( y - x );
        N += 1.0;
        }
      ++count;
      }
    }

  // If we are on the boundary, a neighborhood patch might not overlap
  // with the image.  If we have 2 voxels or less for a neighborhood patch
  // we don't consider it to be a suitable match.
  if( N < 3.0 )
    {
    return NumericTraits<RealType>::max();
    }

  if( this->m_SimilarityMetric == PEARSON_CORRELATION )
    {
    RealType varianceX = sumOfSquaresX - vnl_math_sqr( sumX ) / N;
    varianceX = std::max( varianceX, static_cast<RealType>( 1.0e-6 ) );

    RealType measure = vnl_math_sqr( sumXY ) / varianceX;
    return ( sumXY > 0 ? -measure : measure );
    }
  else if( this->m_SimilarityMetric == MEAN_SQUARES )
    {
    return ( sumOfSquaredDifferencesXY / N );
    }
  else
    {
    itkExceptionMacro( "Unrecognized similarity metric." );
    }
}

template<typename TInputImage, typename TOutputImage>
void
NonLocalSuperresolutionImageFilter<TInputImage, TOutputImage>
::PrintSelf( std::ostream &os, Indent indent ) const
{
  Superclass::PrintSelf( os, indent );

  if( this->m_SimilarityMetric == PEARSON_CORRELATION )
    {
    os << "Using Pearson correlation to measure the patch similarity." << std::endl;
    }
  else if( this->m_SimilarityMetric == MEAN_SQUARES )
    {
    os << "Using mean squares to measure the patch similarity." << std::endl;
    }

  os << "Interpolator: " << std::endl;
  this->m_Interpolator->Print( os, indent );

  os << indent << "Intensity difference sigma = " << this->m_IntensityDifferenceSigma << std::endl;
  os << indent << "Patch similarity sigma = " << this->m_PatchSimilaritySigma << std::endl;

  os << indent << "Neighborhood search radius = " << this->m_NeighborhoodSearchRadius << std::endl;
  os << indent << "Neighborhood block radius = " << this->m_NeighborhoodPatchRadius << std::endl;
}

} // end namespace itk

#endif
