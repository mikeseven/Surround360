/**
* Copyright (c) 2016-present, Facebook, Inc.
* All rights reserved.
*
* This source code is licensed under the BSD-style license found in the
* LICENSE_render file in the root directory of this subproject. An additional grant
* of patent rights can be found in the PATENTS file in the same directory.
*/

#include "ColorCalibration.h"

#include <iomanip>
#include <iostream>
#include <set>
#include <string>
#include <vector>
#include <algorithm>

#include "CameraIsp.h"
#include "CvUtil.h"
#include "LinearRegression.h"
#include "SystemUtil.h"
#include "VrCamException.h"

namespace surround360 {
namespace color_calibration {

using namespace std;
using namespace cv;
using namespace linear_regression;
using namespace util;

int getBitsPerPixel(const Mat& image) {
  uint8_t depth = image.type() & CV_MAT_DEPTH_MASK;
  return depth == CV_8U ? 8 : 16;
}

string getJson(const string& filename) {
  ifstream ifs(filename, ios::in);
  if (!ifs) {
    throw VrCamException("file read failed: " + filename);
  }

  string json(
    (istreambuf_iterator<char>(ifs)),
    istreambuf_iterator<char>());

  return json;
}

// From darkest to brightest
vector<int> getMacBethGrays() {
  static const int kNumGrayPatches = 5;
  vector<int> macBethGrayValues;
  const int iStart = rgbLinearMacbeth.size() - 1;
  for (int i = iStart; i >= iStart - kNumGrayPatches; --i) {
    macBethGrayValues.push_back(rgbLinearMacbeth[i][0]);
  }
  return macBethGrayValues;
}

Mat getRaw(const string& ispConfigFile, const Mat& image) {
  CameraIsp cameraIsp(getJson(ispConfigFile), getBitsPerPixel(image));
  cameraIsp.loadImage(image);
  return cameraIsp.getRawImage();
}

Mat findClampedPixels(const Mat& image8) {
  Mat clamped(image8.size(), image8.type(), Scalar::all(128));
  for (int y = 0; y < image8.rows; ++y) {
    for (int x = 0; x < image8.cols; ++x) {
      const int pixelVal = image8.at<uchar>(y, x);
      if (pixelVal == 0 || pixelVal == 255) {
        clamped.at<uchar>(y, x) = pixelVal;
      }
    }
  }
  return clamped;
}

ColorResponse computeRGBResponse(
    const Mat& raw,
    const bool isRaw,
    vector<ColorPatch>& colorPatches,
    const string& ispConfigFile,
    const bool saveDebugImages,
    const string& outputDir,
    int& stepDebugImages,
    const string& titleExtra) {

  ColorResponse colorResponse;
  Vec3f rgbSlope = Vec3f(0.0f, 0.0f, 0.0f);
  Vec3f rgbInterceptY = Vec3f(0.0f, 0.0f, 0.0f);
  Vec3f rgbInterceptXMin = Vec3f(0.0f, 0.0f, 0.0f);
  Vec3f rgbInterceptXMax = Vec3f(0.0f, 0.0f, 0.0f);

  // Get RGB medians in raw image
  computeRGBMedians(colorPatches, raw, isRaw, ispConfigFile);

  const vector<int> macBethGrayValues = getMacBethGrays();
  const int iStart = colorPatches.size() - 1;

  // Line between second darkest and second brightest medians
  static const int kBrightIdx = 4;
  static const int kDarkIdx = 1;
  const float xDark = float(macBethGrayValues[kDarkIdx]) / 255.0f;
  const float xBright = float(macBethGrayValues[kBrightIdx]) / 255.0f;
  const Vec3f& yDark = colorPatches[iStart - kDarkIdx].rgbMedian;
  const Vec3f& yBright = colorPatches[iStart - kBrightIdx].rgbMedian;

  // Each channel response is of the form y = mx + b
  static const int kNumChannels = 3;
  for (int ch = 0; ch < kNumChannels; ++ch) {
    rgbSlope[ch] = (yBright[ch] - yDark[ch]) / (xBright - xDark);
    rgbInterceptY[ch] = -rgbSlope[ch] * xDark + yDark[ch];
    rgbInterceptXMin[ch] = -rgbInterceptY[ch] / rgbSlope[ch];
    rgbInterceptXMax[ch] = (1.0 - rgbInterceptY[ch]) / rgbSlope[ch];
  }

  colorResponse.rgbSlope = rgbSlope;
  colorResponse.rgbInterceptY = rgbInterceptY;
  colorResponse.rgbInterceptXMin = rgbInterceptXMin;
  colorResponse.rgbInterceptXMax = rgbInterceptXMax;

  if (saveDebugImages) {
    plotGrayPatchResponse(
      colorPatches,
      raw,
      isRaw,
      ispConfigFile,
      titleExtra,
      outputDir,
      stepDebugImages);
  }

  return colorResponse;
}

void saveBlackLevel(const Vec3f& blackLevel, const string& outputDir) {
  const string blackLevelFilename = outputDir + "/black_level.txt";
  ofstream blackLevelStream(blackLevelFilename, ios::out);

  if (!blackLevelStream) {
    throw VrCamException("file open failed: " + blackLevelFilename);
  }

  blackLevelStream << blackLevel;
  blackLevelStream.close();
}

void saveXIntercepts(
    const ColorResponse& colorResponse,
    const string& outputDir) {

  const string interceptXFilename = outputDir + "/intercept_x.txt";
  ofstream interceptXStream(interceptXFilename, ios::out);

  if (!interceptXStream) {
    throw VrCamException("file open failed: " + interceptXFilename);
  }

  interceptXStream << "[";
  interceptXStream << colorResponse.rgbInterceptXMin;
  interceptXStream << ",";
  interceptXStream << colorResponse.rgbInterceptXMax;
  interceptXStream << "]";
  interceptXStream.close();
}

Mat adjustBlackLevel(
    const string& ispConfigFile,
    const Mat& rawRef,
    const Mat& raw,
    const Point3f& blackLevel) {

  const int bitsPerPixel = getBitsPerPixel(rawRef);
  const float maxPixelValue = float((1 << bitsPerPixel) - 1);
  CameraIsp cameraIsp(getJson(ispConfigFile), bitsPerPixel);
  cameraIsp.setBlackLevel(blackLevel * maxPixelValue);
  cameraIsp.setup();
  cameraIsp.loadImage(rawRef); // Load original image
  cameraIsp.setRawImage(raw); // Replace with modified version
  cameraIsp.blackLevelAdjust();
  return cameraIsp.getRawImage();
}

Mat whiteBalance(
    const string& ispConfigFile,
    const Mat& rawRef,
    const Mat& raw,
    const Vec3f& whiteBalanceGain) {

  CameraIsp cameraIsp(getJson(ispConfigFile), getBitsPerPixel(rawRef));
  cameraIsp.setWhiteBalance(whiteBalanceGain);
  cameraIsp.setup();
  cameraIsp.loadImage(rawRef);
  cameraIsp.setRawImage(raw);
  cameraIsp.whiteBalance(false); // no clamping
  return cameraIsp.getRawImage();
}

Mat clampAndStretch(
    const string& ispConfigFile,
    const Mat& rawRef,
    const Mat& raw,
    const ColorResponse& colorResponse,
    Vec3f& rgbClampMin,
    Vec3f& rgbClampMax) {

  // Get values at specific thresholds, assuming response is y = mx + b
  const Vec3f& m = colorResponse.rgbSlope;
  const Vec3f& b = colorResponse.rgbInterceptY;
  const float xMin = rgbClampMin[0];
  const float xMax = rgbClampMax[0];
  rgbClampMin = m * xMin + b;
  rgbClampMax = m * xMax + b;

  static const int kNumChannels = 3;
  for (int ch = 0; ch < kNumChannels; ++ch) {
    rgbClampMin[ch] = max(0.0f, rgbClampMin[ch]);
    rgbClampMax[ch] = min(1.0f, rgbClampMax[ch]);
  }

  CameraIsp cameraIsp(getJson(ispConfigFile), getBitsPerPixel(raw));
  cameraIsp.setClampMin(rgbClampMin);
  cameraIsp.setClampMax(rgbClampMax);
  cameraIsp.setup();
  cameraIsp.loadImage(rawRef);
  cameraIsp.setRawImage(raw);
  cameraIsp.clampAndStretch();
  return cameraIsp.getRawImage();
}

Mat demosaic(const string& ispConfigFile, const Mat& rawRef, const Mat& raw) {
  CameraIsp cameraIsp(getJson(ispConfigFile), getBitsPerPixel(rawRef));
  cameraIsp.setDemosaicFilter(BILINEAR_DM_FILTER);
  cameraIsp.setup();
  cameraIsp.loadImage(rawRef);
  cameraIsp.setRawImage(raw);
  cameraIsp.demosaic();
  return cameraIsp.getDemosaicedImage();
}

Mat colorCorrect(
    const string& ispConfigFile,
    const Mat& rawRef,
    const Mat& rgb,
    const Mat& ccm,
    const Vec3f& gamma) {

  CameraIsp cameraIsp(getJson(ispConfigFile), getBitsPerPixel(rawRef));
  cameraIsp.setCCM(ccm);
  cameraIsp.setGamma(gamma);
  cameraIsp.setup();
  cameraIsp.loadImage(rawRef);
  cameraIsp.setDemosaicedImage(rgb);
  cameraIsp.colorCorrect();
  return cameraIsp.getDemosaicedImage();
}

void writeIspConfigFile(
    const string& ispConfigFile,
    const string& ispConfigFileOut,
    const Mat& raw,
    const Vec3f& blackLevel,
    const Vec3f& whiteBalanceGain,
    const Vec3f& clampMin,
    const Vec3f& clampMax,
    const Mat& ccm,
    const Vec3f& gamma) {

  const int bitsPerPixel = getBitsPerPixel(raw);
  const float maxPixelValue = float((1 << bitsPerPixel) - 1);
  CameraIsp cameraIsp(getJson(ispConfigFile), bitsPerPixel);
  cameraIsp.setBlackLevel(blackLevel * maxPixelValue);
  cameraIsp.setWhiteBalance(whiteBalanceGain);
  cameraIsp.setClampMin(clampMin);
  cameraIsp.setClampMax(clampMax);
  cameraIsp.setCCM(ccm);
  cameraIsp.setGamma(gamma);
  cameraIsp.setup();
  cameraIsp.loadImage(raw);
  cameraIsp.dumpConfigFile(ispConfigFileOut);
}

Vec3f findBlackLevel(
    const Mat& raw16,
    const string& ispConfigFile,
    const bool saveDebugImages,
    const string& outputDir,
    int& stepDebugImages) {

  // Divide raw into R, G and B channels
  const int bitsPerPixel = getBitsPerPixel(raw16);
  const int maxPixelValue = (1 << bitsPerPixel) - 1;
  static const int kNumChannels = 3;
  vector<Mat> RGBs;
  for (int i = 0; i < kNumChannels; ++i) {
    // Initialize channel to max value. Unused pixels will be at the high end of
    // the histogram, which will not be reached
    RGBs.push_back(Mat(raw16.size(), CV_32F, Scalar(maxPixelValue)));
  }

  CameraIsp cameraIsp(getJson(ispConfigFile), bitsPerPixel);
  for (int i = 0; i < raw16.rows; ++i) {
    for (int j = 0; j < raw16.cols; j++) {
      const int channelIdx =
        cameraIsp.redPixel(i, j) ? 0 : (cameraIsp.greenPixel(i, j) ? 1 : 2);
      RGBs[channelIdx].at<float>(i, j) = raw16.at<uint16_t>(i, j);
    }
  }

  // Calculate per channel histograms
  Mat blackHoleMask = Mat::zeros(raw16.size(), CV_8UC1);
  static const int kNumPixelsMin = 50;
  double blackLevelThreshold = 0.0;
  for (int i = 0; i < kNumChannels; ++i) {
    // Black level threshold is the lowest non-zero value with enough pixel
    // count (to avoid noise and dead pixels)
    Mat hist = computeHistogram(RGBs[i], Mat());
    for (int h = 0; h < maxPixelValue; h++) {
      if (hist.at<float>(h) > kNumPixelsMin) {
        blackLevelThreshold = h;
        break;
      }
    }

    // Create mask with all pixels below threshold
    Mat mask;
    inRange(RGBs[i], 0.0f, blackLevelThreshold, mask);
    blackHoleMask = (blackHoleMask | mask);
  }

  // Black hole mask can contain outliers and pixels outside black hole. We need
  // to filter it
  vector<vector<Point>> contours;
  static const float kStraightenFactor = 0.01f;
  contours = findContours(
      blackHoleMask, false, outputDir, stepDebugImages, kStraightenFactor);

  // Filter contours
  vector<vector<Point>> contoursFiltered;
  vector<Point2f> circleCenters;
  vector<float> circleRadii;
  for (int i = 0; i < contours.size(); ++i) {
    vector<Point2i> cont = contours[i];
    const int contArea = contourArea(cont);

    Point2f circleCenter;
    float circleRadius;
    minEnclosingCircle(cont, circleCenter, circleRadius);
    const float circleArea = M_PI * circleRadius * circleRadius;

    // Discard contours that are too small and non-circular
    static const int kMinNumVertices = 10;
    static const float kMaxRatioAreas = 0.5f;
    if (contArea < kNumPixelsMin ||
        cont.size() < kMinNumVertices ||
        contArea / circleArea < kMaxRatioAreas) {
      continue;
    }

    circleCenters.push_back(circleCenter);
    circleRadii.push_back(circleRadius);
    contoursFiltered.push_back(contours[i]);
  }

  if (saveDebugImages) {
    Mat contoursPlot = Mat::zeros(blackHoleMask.size(), CV_8UC3);
    RNG rng(12345);
    for(int i = 0; i < contoursFiltered.size(); ++i) {
      Scalar color =
        Scalar(rng.uniform(0, 255), rng.uniform(0, 255), rng.uniform(0, 255));
      drawContours(contoursPlot, contoursFiltered, i, color);
      circle(contoursPlot, circleCenters[i], (int)circleRadii[i], color);
    }
    const string contoursImageFilename =
      outputDir + "/" + to_string(++stepDebugImages) + "_contours_filtered.png";
    imwriteExceptionOnFail(contoursImageFilename, contoursPlot);
  }

  // Get RGB median of each filtered contour
  vector<Mat> blackHoleMasks(contoursFiltered.size());
  vector<Vec3f> blackLevels(contoursFiltered.size());
  double minNorm = DBL_MAX;
  int minNormIdx = 0;
  for(int i = 0; i < contoursFiltered.size(); ++i) {
    // Create contour mask
    blackHoleMasks[i] = Mat::zeros(blackHoleMask.size(), CV_8UC1);
    drawContours(blackHoleMasks[i], contoursFiltered, i, Scalar(255), CV_FILLED);

    Mat rawNormalized = getRaw(ispConfigFile, raw16);
    static const bool kIsRaw = true;
    blackLevels[i] =
      getRgbMedianMask(rawNormalized, blackHoleMasks[i], ispConfigFile, kIsRaw);

    // Find distance to [0, 0, 0]
    double blackLevelNorm = norm(blackLevels[i]);
    if (blackLevelNorm < minNorm) {
      minNorm = blackLevelNorm;
      minNormIdx = i;
    }
  }

  // Black level is the one closest to origin
  Vec3f blackLevel = blackLevels[minNormIdx];

  if (saveDebugImages) {
    Mat rawRGB(raw16.size(), CV_8UC3);
    cvtColor(raw16, rawRGB, CV_GRAY2RGB);
    blackHoleMask = blackHoleMasks[minNormIdx];
    rawRGB.setTo(Scalar(0, maxPixelValue, 0), blackHoleMask);

    const string blackHoleMaskImageFilename =
      outputDir + "/" + to_string(++stepDebugImages) + "_black_hole_mask.png";
    imwriteExceptionOnFail(blackHoleMaskImageFilename, rawRGB);
  }

  const Vec3f blackLevelScaled = blackLevel * maxPixelValue;
  LOG(INFO) << "Black level (" << bitsPerPixel << "-bit): " << blackLevelScaled;

  return blackLevel;
}

Mat computeHistogram(const Mat& image, const Mat& mask) {
  static const int kNumImages = 1;
  static const int* kChannelsAuto = 0;
  static const int kNumDims = 1;
  const int bitsPerPixel = getBitsPerPixel(image);
  const float maxPixelValue = float((1 << bitsPerPixel) - 1);
  const int histSize = maxPixelValue + 1;
  float range[] = {0, maxPixelValue};
  const float *ranges[] = {range};
  Mat hist;
  calcHist(
    &image,
    kNumImages,
    kChannelsAuto,
    mask,
    hist,
    kNumDims,
    &histSize,
    ranges);
  return hist;
}

vector<ColorPatch> detectColorChart(
    const Mat& image,
    const int numSquaresW,
    const int numSquaresH,
    const bool saveDebugImages,
    const string& outputDir,
    int& stepDebugImages) {

  // Scale image to make patches brighter
  static const float kScale = 2.0f;
  Mat imageScaled = kScale * image;

  // Smooth image
  Mat imageBlur;
  static const Size kBlurSize = Size(15, 15);
  static const double kSigmaAuto = 0;
  GaussianBlur(imageScaled, imageBlur, kBlurSize, kSigmaAuto);

  if (saveDebugImages) {
    const string blurImageFilename =
      outputDir + "/" + to_string(++stepDebugImages) + "_scaled_blurred.png";
    imwriteExceptionOnFail(blurImageFilename, imageBlur);
  }

  // Adaptive thresholding
  Mat bw;
  static const double kMaxValue = 255.0;
  static const int kBlockSize = 19;
  static const int kWeightedSub = 2;
  adaptiveThreshold(
    imageBlur,
    bw,
    kMaxValue,
    ADAPTIVE_THRESH_MEAN_C,
    THRESH_BINARY_INV,
    kBlockSize,
    kWeightedSub);

  if (saveDebugImages) {
    const string adaptiveThreshImageFilename =
      outputDir + "/" + to_string(++stepDebugImages)
      + "_adaptive_threshold.png";
    imwriteExceptionOnFail(adaptiveThreshImageFilename, bw);
  }

  // Morphological closing to reattach patches
  bw = fillGaps(bw, saveDebugImages, outputDir, stepDebugImages);

  // Remove small objects
  bw = removeSmallObjects(bw, saveDebugImages, outputDir, stepDebugImages);

  // Dilate gaps so contours don't contain pixels outside patch
  bw = dilateGaps(bw, saveDebugImages, outputDir, stepDebugImages);

  // Morphological constraints for chart detection
  // - connected component must be larger than 1% of image size
  // - color chart cannot be larger than 40% of image size
  const float imSize = bw.cols * bw.rows;
  const float minNumPixels = 0.01f * imSize;
  const float maxAreaChart = 0.4f * imSize;
  static const float kStraightenFactor = 0.08f;

  // Connected components
  Mat labels;
  Mat stats;
  Mat centroids;
  int la = connectedComponentsWithStats(bw, labels, stats, centroids, 8);

  // Filter components
  vector<vector<Point>> contours;
  Mat bwLabel(bw.size(), CV_8UC1, Scalar::all(0));
  const Point center = Point(bw.cols / 2, bw.rows / 2);
  bool isChartFound = false;
  for (int label = 1; label < la; ++label) {
    const int numPixels = stats.at<int>(label, CC_STAT_AREA);
    if (numPixels < minNumPixels) {
      continue;
    }

    const int top  = stats.at<int>(label, CC_STAT_TOP);
    const int left = stats.at<int>(label, CC_STAT_LEFT);
    const int width = stats.at<int>(label, CC_STAT_WIDTH);
    const int height  = stats.at<int>(label, CC_STAT_HEIGHT);

    // Assuming chart is centered
    static const float kFracErrorX = 0.10f;
    if (left > (1.0f + kFracErrorX) * center.x ||
        top > center.y ||
        left + width < (1.0f - kFracErrorX) * center.x ||
        top + height < center.y) {
      continue;
    }

    // Assuming chart doesn't take too much of the image
    if (width * height > maxAreaChart) {
      continue;
    }

    // Get contours for current label
    bwLabel.setTo(Scalar::all(0));
    inRange(labels, label, label, bwLabel);
    contours = findContours(
      bwLabel, saveDebugImages, outputDir, stepDebugImages, kStraightenFactor);

    // Check if we have at least as many contours as number of patches
    if (contours.size() >= numSquaresW * numSquaresH) {
      isChartFound = true;
      break;
    }
  }

  if (!isChartFound) {
    throw VrCamException("No chart found");
  }

  vector<ColorPatch> colorPatchList;

  // Morphological constraints for patch filtering
  // - patch size between 0.01% and 0.45% of image size
  // - patch aspect ratio <= 1.2
  // - patch is square
  const float minArea = 0.01f / 100.0f * imSize;
  const float maxArea = 0.45f / 100.0f * imSize;
  static const float kMaxAspectRatio = 1.2f;
  static const int kNumEdges = 4;

  // Filter contours
  int countPatches = 0;
  for (int i = 0; i < contours.size(); ++i) {
    vector<Point2i> cont = contours[i];
    RotatedRect boundingBox = minAreaRect(cont);
    Moments mu = moments(cont, false);

    Point2f centroid = boundingBox.center;

    const int width = boundingBox.size.width;
    const int height = boundingBox.size.height;
    const int area = mu.m00;
    const int aspectRatio =
      1.0f * max(width, height) / (1.0f * min(width, height));

    // Discard contours that are too small/large, non-square and non-convex
    if (area < minArea || area > maxArea ||
        cont.size() != kNumEdges ||
        aspectRatio > kMaxAspectRatio ||
        !isContourConvex(cont)) {
      continue;
    }

    LOG(INFO) << "Patch found (" << countPatches++ << ")!";

    // Create patch mask
    Mat patchMask(bw.size(), CV_8UC1, Scalar::all(0));
    patchMask(boundingRect(cont)).setTo(Scalar::all(255));

    // Add patch to list
    ColorPatch colorPatch;
    colorPatch.centroid = centroid;
    colorPatch.mask = patchMask;

    colorPatchList.push_back(colorPatch);
  }

  if (colorPatchList.size() == 0) {
    return colorPatchList;
  }

  vector<ColorPatch> colorPatchListClean =
    removeContourOutliers(colorPatchList);

  vector<ColorPatch> colorPatchListSorted =
    sortPatches(colorPatchListClean, numSquaresW, image.size());

  LOG(INFO) << "Number of patches found: " << colorPatchListSorted.size();

  if (saveDebugImages) {
    Mat rgbDraw = image;
    cvtColor(rgbDraw, rgbDraw, CV_GRAY2RGB);
    rgbDraw = drawPatches(rgbDraw, colorPatchListSorted);
    const string patchesImageFilename =
      outputDir + "/" + to_string(++stepDebugImages) + "_detected_patches.png";
    imwriteExceptionOnFail(patchesImageFilename, rgbDraw);
  }

  return colorPatchListSorted;
}

Mat fillGaps(
    const Mat& imageBwIn,
    const bool saveDebugImages,
    const string& outputDir,
    int& stepDebugImages) {

  Mat imageBwOut;
  Mat element = createMorphElement(imageBwIn.size(), MORPH_CROSS);
  morphologyEx(imageBwIn, imageBwOut, MORPH_CLOSE, element);

  if (saveDebugImages) {
    const string morphImageFilename =
      outputDir + "/" + to_string(++stepDebugImages) + "_fill_gaps.png";
    imwriteExceptionOnFail(morphImageFilename, imageBwOut);
  }

  return imageBwOut;
}

Mat dilateGaps(
    const Mat& imageBwIn,
    const bool saveDebugImages,
    const string& outputDir,
    int& stepDebugImages) {

  Mat imageBwOut;
  Mat element = createMorphElement(imageBwIn.size(), MORPH_RECT);
  dilate(imageBwIn, imageBwOut, element);

  if (saveDebugImages) {
    const string morphImageFilename =
      outputDir + "/" + to_string(++stepDebugImages) + "_dilate.png";
    imwriteExceptionOnFail(morphImageFilename, imageBwOut);
  }

  return imageBwOut;
}

Mat createMorphElement(const Size imageSize, const int shape) {
  static const float kMorphFrac = 0.3f / 100.0f;
  const int morphRadius =
    kMorphFrac * min(imageSize.width, imageSize.height);
  Size morphSize = Size(2 * morphRadius + 1, 2 * morphRadius + 1);
  return getStructuringElement(
    shape,
    morphSize,
    Point2f(morphRadius, morphRadius));
}

Mat removeSmallObjects(
    const Mat& imageBwIn,
    const bool saveDebugImages,
    const string& outputDir,
    int& stepDebugImages) {

  static const float kMinAreaFrac = 0.01f / 100.0f;
  const int minArea = kMinAreaFrac * imageBwIn.cols * imageBwIn.rows;

  Mat labels;
  Mat stats;
  Mat centroids;
  std::set<int> labelsSmall;
  int numConnectedComponents =
    connectedComponentsWithStats(imageBwIn, labels, stats, centroids);

  for (int label = 0; label < numConnectedComponents; ++label) {
    if (stats.at<int>(label, CC_STAT_AREA) < minArea) {
      labelsSmall.insert(label);
    }
  }

  Mat imageBwOut = imageBwIn.clone();
  for (int y = 0; y < imageBwIn.rows; ++y) {
    for (int x = 0; x < imageBwIn.cols; ++x) {
      const int v = labels.at<int>(y, x);
      if (labelsSmall.find(v) != labelsSmall.end()) {
        imageBwOut.at<uchar>(y, x) = 0;
      }
    }
  }

  if (saveDebugImages) {
    const string morphImageFilename =
      outputDir + "/" + to_string(++stepDebugImages) + "_no_small_objects.png";
    imwriteExceptionOnFail(morphImageFilename, imageBwOut);
  }

  return imageBwOut;
}

vector<vector<Point>> findContours(
    const Mat& image,
    const bool saveDebugImages,
    const string& outputDir,
    int& stepDebugImages,
    const float straightenFactor) {

  vector<vector<Point>> contours;
  vector<Vec4i> hierarchy;
  static const Point kOffset = Point(0, 0);
  findContours(
    image,
    contours,
    hierarchy,
    CV_RETR_TREE,
    CV_CHAIN_APPROX_SIMPLE,
    kOffset);

  // Straighten contours to minimize number of vertices
  for(int i = 0; i < contours.size(); ++i) {
    const double epsilonPolyDP =
      straightenFactor * arcLength(contours[i], true);
    approxPolyDP(contours[i], contours[i], epsilonPolyDP, true);
  }

  if (saveDebugImages) {
    Mat contoursPlot = Mat::zeros(image.size(), CV_8UC3);
    RNG rng(12345);
    for(int i = 0; i < contours.size(); ++i) {
      Scalar color =
        Scalar(rng.uniform(0, 255), rng.uniform(0, 255), rng.uniform(0, 255));
      drawContours(contoursPlot, contours, i, color);
    }
    const string contoursImageFilename =
      outputDir + "/" + to_string(++stepDebugImages) + "_contours.png";
    imwriteExceptionOnFail(contoursImageFilename, contoursPlot);
   }

  return contours;
}

vector<ColorPatch> removeContourOutliers(vector<ColorPatch> colorPatchList) {
  // Store min distance between patches, for each patch
  vector<float> minDistances (colorPatchList.size(), FLT_MAX);
  for (int iPatch = 0; iPatch < colorPatchList.size(); ++iPatch) {
    for (int jPatch = 0; jPatch < colorPatchList.size(); ++jPatch) {
      if (iPatch == jPatch) {
        continue;
      }
      const Point2f ci = colorPatchList[iPatch].centroid;
      const Point2f cj = colorPatchList[jPatch].centroid;
      const float distance = norm(ci - cj);
      if (distance < minDistances[iPatch]) {
        minDistances[iPatch] = distance;
      }
    }
  }

  // Find median minimum distance between patches
  vector<float> minDistancesSorted = minDistances;
  sort(minDistancesSorted.begin(), minDistancesSorted.end());
  float minDistanceMedian = minDistancesSorted[minDistancesSorted.size() / 2];

  // Discard patches with too large of a minimum distance
  const float maxDistanceThreshold = 2.0f * minDistanceMedian;
  vector<ColorPatch> colorPatchListClean;
  for (int iPatch = 0; iPatch < colorPatchList.size(); ++iPatch) {
    if (minDistances[iPatch] < maxDistanceThreshold) {
      colorPatchListClean.push_back(colorPatchList[iPatch]);
    }
  }

  return colorPatchListClean;
}

vector<ColorPatch> sortPatches(
    const vector<ColorPatch>& colorPatchList,
    const int numSquaresW,
    const Size imageSize) {

  const Point2f topLeftRef = Point2f(0.0f, 0.0f);
  const Point2f topRightRef = Point2f(imageSize.width, 0.0f);
  Point2f topLeft = Point2f(FLT_MAX, FLT_MAX);
  Point2f topRight = Point2f(-1.0f, FLT_MAX);

  // Assuming top left is (0, 0)
  vector<Point2f> centroids;
  for (ColorPatch patch : colorPatchList) {
    Point2f centroid = patch.centroid;
    centroids.push_back(centroid);

    if (norm(topLeftRef - centroid) < norm(topLeftRef - topLeft)) {
      topLeft = centroid;
    }
    if (norm(topRightRef - centroid) < norm(topRightRef - topRight)) {
      topRight = centroid;
    }
  }

  vector<Point2f> centroidsSorted;

  while (centroids.size() > 0) {
    // Get points in current row, i.e. closest to line between top-left and
    // top-right patches
    vector<Point2f> centroidsDistances;
    const Point2f pLine1 = findTopLeft(centroids);
    const Point2f pLine2 = findTopRight(centroids, imageSize.width);

    // Sort centroids by their distance to the line
    sort(centroids.begin(), centroids.end(),
      [pLine1, pLine2] (const Point2f& p1, const Point2f& p2) {
        float d1 = pointToLineDistance(p1, pLine1, pLine2);
        float d2 = pointToLineDistance(p2, pLine1, pLine2);
        return d1 < d2;
      });

    // Get top numSquaresW
    vector<Point2f> centroidsRow;
    for (int i = 0; i < numSquaresW && i < centroids.size(); ++i) {
      centroidsRow.push_back(centroids[i]);
    }

    // Sort row by X coordinate
    sort(centroidsRow.begin(), centroidsRow.end(), [](Point pt1, Point pt2) {
      return pt1.x < pt2.x;
    });

    // Add row to parent vector
    for (Point2f& centroid : centroidsRow) {
      centroidsSorted.push_back(centroid);
    }

    // Remove row from vector
    centroids.erase(
      centroids.begin(),
      centroids.begin() + centroidsRow.size());
  }

  // Re-order vector
  vector<ColorPatch> colorPatchListSorted;
  for (const Point2f& c : centroidsSorted) {
    for (const ColorPatch cp : colorPatchList) {
      if (cp.centroid == c) {
        colorPatchListSorted.push_back(cp);
        break;
      }
    }
  }

  return colorPatchListSorted;
}

Point2f findTopLeft(const vector<Point2f>& points) {
  static const Point2f kTopLeftRef = Point2f(0.0f, 0.0f);
  Point2f topLeft = Point2f(FLT_MAX, FLT_MAX);
  for (const Point2f& p : points) {
    if (norm(kTopLeftRef - p) < norm(kTopLeftRef - topLeft)) {
      topLeft = p;
    }
  }
  return topLeft;
}

Point2f findTopRight(const vector<Point2f>& points, int imageWidth) {
  const Point2f topRightRef = Point2f(imageWidth, 0.0f);
  Point2f topRight = Point2f(-1.0f, FLT_MAX);
  for (const Point2f& p : points) {
    if (norm(topRightRef - p) < norm(topRightRef - topRight)) {
      topRight = p;
    }
  }
  return topRight;
}

float pointToLineDistance(
    const Point2f p,
    const Point2f pLine1,
    const Point2f pLine2) {

  // Numerator: height of the triangle defined by the three points
  // Denominator: distance between two points in line
  const float n1 = (pLine2.y - pLine1.y) * p.x;
  const float n2 = (pLine2.x - pLine1.x) * p.y;
  const float n3 = pLine2.x * pLine1.y;
  const float n4 = pLine2.y * pLine1.x;
  return fabs(n1 - n2 + n3 - n4) / norm(pLine1 - pLine2);
}

Mat drawPatches(const Mat& image, vector<ColorPatch>& colorPatches) {
  Mat imageDraw = image.clone();
  for (int i = 0; i < colorPatches.size(); ++i) {
    vector<vector<Point>> contours;
    vector<Vec4i> hierarchy;
    static const Point kOffset = Point(0, 0);
    findContours(
      colorPatches[i].mask,
      contours,
      hierarchy,
      CV_RETR_TREE,
      CV_CHAIN_APPROX_SIMPLE,
      kOffset);
    static const int kContourIdx = 0;
    static const Scalar kColorG = Scalar(0, 255, 0);
    drawContours(imageDraw, contours, kContourIdx, kColorG);

    const Point2f center = colorPatches[i].centroid;
    static const double kTextFontScale = 0.4;
    const string text = to_string(i);
    putText(
      imageDraw,
      text,
      center,
      FONT_HERSHEY_SIMPLEX,
      kTextFontScale,
      kColorG);
  }

  return imageDraw;
}

void computeRGBMedians(
    vector<ColorPatch>& colorPatches,
    const Mat& image,
    const bool isRaw,
    const string& ispConfigFile) {

  for (int i = 0; i < colorPatches.size(); ++i) {
    colorPatches[i].rgbMedian =
      getRgbMedianMask(image, colorPatches[i].mask, ispConfigFile, isRaw);
    LOG(INFO) << "Patch " << i << " RGB median: " << colorPatches[i].rgbMedian;
  }
}

Vec3f getRgbMedianMask(
    const Mat& image,
    const Mat& mask,
    const string& ispConfigFile,
    const bool isRaw) {

  // Allocate space for mask values on each channel
  static const int kNumChannels = 3;
  vector<vector<float>> RGBs;
  for (int ch = 0; ch < kNumChannels; ++ch) {
    vector<float> channel;
    RGBs.push_back(channel);
  }

  // Populate color channels
  Mat locs;
  findNonZero(mask, locs);
  CameraIsp cameraIsp(getJson(ispConfigFile), getBitsPerPixel(image));
  for (int ip = 0; ip < locs.rows; ++ip) {
    Point p = locs.at<Point>(ip);

    if (isRaw) {
      const float valRaw = image.at<float>(p);
      const int channelRaw = cameraIsp.redPixel(p.y, p.x)
        ? 0 : (cameraIsp.greenPixel(p.y, p.x) ? 1 : 2);
      RGBs[channelRaw].push_back(valRaw);
    } else {
      const Vec3f& val = image.at<Vec3f>(p);
      for (int ch = 0; ch < kNumChannels; ++ch) {
        RGBs[ch].push_back(val[ch]);
      }
    }
  }

  // Use partial sort to get median
  Vec3f rgbMedian = Vec3f(-1.0f, -1.0f, -1.0f);
  for (int ch = 0; ch < kNumChannels; ++ch) {
    vector<float>::iterator itMedian = RGBs[ch].begin() + RGBs[ch].size() / 2;
    std::nth_element(RGBs[ch].begin(), itMedian, RGBs[ch].end());
    rgbMedian[ch] = RGBs[ch][RGBs[ch].size() / 2];
  }

  return rgbMedian;

}

Vec3f plotGrayPatchResponse(
    vector<ColorPatch>& colorPatches,
    const Mat& image,
    const bool isRaw,
    const string& ispConfigFile,
    const string& titleExtra,
    const string& outputDir,
    int& stepDebugImages) {

  static const Scalar kRgbColors[] =
    {Scalar(0, 0, 255), Scalar(0, 255, 0), Scalar(255, 0, 0)};
  const int bitsPerPixel = getBitsPerPixel(image);
  const float maxPixelValue = float((1 << bitsPerPixel) - 1);
  static const int kScalePlot = 10;
  static const float maxScaled = 255.0f * kScalePlot;
  static const float maxRow = 1.5f * maxScaled;
  static const float maxCol = maxRow;

  Point2f textCenter;
  string text;
  const int textFont = FONT_HERSHEY_SIMPLEX;
  static const float textSize = kScalePlot * 0.2f;
  static const int kTextThickness = 3;

  // Get RGB medians
  LOG(INFO) << "RGB medians (" << titleExtra << ")...";
  vector<ColorPatch> colorPatchesPlot = colorPatches;
  computeRGBMedians(colorPatchesPlot, image, isRaw, ispConfigFile);

  CameraIsp cameraIsp(getJson(ispConfigFile), bitsPerPixel);
  const vector<int> macBethGrayValues = getMacBethGrays();
  const int iStart = colorPatchesPlot.size() - 1;
  const Point2f pShift = Point2f(5.0f * kScalePlot, 0.0f);
  const Point2f pShiftText = Point2f(pShift.x, 0.0f);
  static const int kNumChannels = 3;
  Mat scatterImage(maxRow, maxCol, CV_8UC3, Scalar::all(255));

  static const int kNumGreyPatches = 5;
  static const int kRadiusCircle = 3;
  for (int i = iStart; i >= iStart - kNumGreyPatches; --i) {
    const float xCoord =
      maxScaled * float(macBethGrayValues[iStart - i]) / 255.0f;

    // Only consider pixels inside patch mask
    Mat locs;
    findNonZero(colorPatchesPlot[i].mask, locs);

    // Plot all values
    for (int ip = 0; ip < locs.rows; ++ip) {
      Point p = locs.at<Point>(ip);
      if (isRaw) {
        const float patchValRaw = maxScaled * image.at<float>(p);
        const Point2f center = Point2f(xCoord, maxRow - patchValRaw);
        const int colorIdx = cameraIsp.redPixel(p.y, p.x)
          ? 0 : (cameraIsp.greenPixel(p.y, p.x) ? 1 : 2);
        circle(scatterImage, center, kRadiusCircle, kRgbColors[colorIdx], -1);
      } else {
        const Vec3f& patchVal = maxScaled * image.at<Vec3f>(p);
        for (int ch = 0; ch < kNumChannels; ++ch) {
          const Point2f center = Point2f(xCoord, maxRow - patchVal[ch]);
          circle(scatterImage, center, kRadiusCircle, kRgbColors[ch], -1);
        }
      }
    }

    // Plot medians
    const Vec3f& median = maxScaled * colorPatchesPlot[i].rgbMedian;
    static const int kLineThickness = 3;
    for (int ch = 0; ch < kNumChannels; ++ch) {
      const Point2f center = Point2f(xCoord, maxRow - median[ch]);
      line(
        scatterImage,
        center - pShift,
        center + pShift,
        kRgbColors[ch],
        kLineThickness);

      textCenter = center + pShiftText;
      const float medianReal = maxPixelValue * colorPatchesPlot[i].rgbMedian[ch];
      std::ostringstream textStream;
      textStream << fixed << std::setprecision(2) << medianReal;
      text = textStream.str();
      putText(
        scatterImage,
        text,
        textCenter,
        textFont,
        textSize * 0.8,
        kRgbColors[ch],
        kTextThickness);
    }
  }

  // Line between second darkest and second brightest medians
  static const int kBrightIdx = 4;
  static const int kDarkIdx = 1;
  const float xDark = maxScaled * float(macBethGrayValues[kDarkIdx]) / 255.0f;
  const float xBright =
    maxScaled * float(macBethGrayValues[kBrightIdx]) / 255.0f;
  const Vec3f& yDark = maxScaled * colorPatchesPlot[iStart - kDarkIdx].rgbMedian;
  const Vec3f& yBright =
    maxScaled * colorPatchesPlot[iStart - kBrightIdx].rgbMedian;

  textCenter = Point2f(50.0f, 0.0f);
  Vec3f yIntercepts = Vec3f(-1.0f, -1.0f, -1.0f);
  static const int kLineThickness = 3;
  for (int j = 0; j < kNumChannels; ++j) {
    const float slope = (yBright[j] - yDark[j]) / (xBright - xDark);
    const float yIntercept = -slope * xDark + yDark[j];
    const float xIntercept = -yIntercept / slope;
    const Point2f centerDark = Point2f(0.0f, maxRow - yIntercept);

    yIntercepts[j] = yIntercept / maxScaled * maxPixelValue;

    std::ostringstream textYIntercept;
    textYIntercept << fixed << std::setprecision(2) << yIntercepts[j];
    std::ostringstream textXIntercept;
    textXIntercept << fixed << std::setprecision(2) <<
      xIntercept / maxScaled * maxPixelValue;

    std::ostringstream textSlope;
    textSlope << fixed << std::setprecision(3) << slope;

    textCenter.y = 100.0f * (j + 1);
    text =
      "xIntercept: " + textXIntercept.str() +
      ", yIntercept: " + textYIntercept.str() +
      ", slope: " + textSlope.str();
    putText(
      scatterImage,
      text,
      textCenter,
      textFont,
      textSize,
      kRgbColors[j],
      kTextThickness);

    LOG(INFO) << (j == 0 ? "R" : (j == 1 ? "G" : "B")) << ": " << text;

    const float dest = slope * maxCol + yIntercept;
    const Point2f centerBright = Point2f(maxCol, maxRow - dest);
    line(scatterImage, centerDark, centerBright, kRgbColors[j], kLineThickness);
  }

  const string plotGrayImageFilename =
    outputDir + "/" + to_string(++stepDebugImages) + "_gray_patches_" +
    titleExtra + ".png";
  imwriteExceptionOnFail(plotGrayImageFilename, scatterImage);

  return yIntercepts;
}

Vec3f computeWhiteBalanceGains(const ColorResponse& colorResponse) {
  // White balance is inverse of the RGB slopes (ground truth has slope = 1)
  Vec3f wbGains;
  divide(Vec3f(1.0f, 1.0f, 1.0f), colorResponse.rgbSlope, wbGains);
  return wbGains;
}

Mat computeCCM(const vector<ColorPatch>& colorPatches) {
  vector<vector<float>> inputs;
  vector<vector<float>> outputs;

  // Assuming raster scan order
  // Assuming color patch medians are [0..1]
  for (int i = 0; i < colorPatches.size(); ++i) {
    Vec3f patchRGB = colorPatches[i].rgbMedian;

    vector<float> patchRGBv {patchRGB[0], patchRGB[1], patchRGB[2]};
    inputs.push_back(patchRGBv);

    // Convert RGB to float
    vector<float> rgbLinearMacbethOut(
      rgbLinearMacbeth[i].begin(),
      rgbLinearMacbeth[i].end());

    // Normalize ground truth color patch to [0..1]
    transform(
      rgbLinearMacbethOut.begin(),
      rgbLinearMacbethOut.end(),
      rgbLinearMacbethOut.begin(),
      bind1st(multiplies<float>(), 1.0f / 255.0f));

    outputs.push_back(rgbLinearMacbethOut);
  }

  static const int kInputDim = 3;
  static const int kOutputDim = 3;
  static const int kNumIterations = 100000;
  static const float kStepSize = 0.1f;
  static const bool kPrintObjective = false;
  vector<vector<float>> ccm = solveLinearRegressionRdToRk(
    kInputDim,
    kOutputDim,
    inputs,
    outputs,
    kNumIterations,
    kStepSize,
    kPrintObjective);

  // Make sure we have a 3x3 matrix
  assert(ccm.size() == 3 && ccm.size() == ccm[0].size());

  // Convert to Mat
  Mat ccmMat(ccm.size(), ccm.at(0).size(), CV_32FC1);
  for(int y = 0; y < ccmMat.rows; ++y) {
    for(int x = 0; x < ccmMat.cols; ++x) {
      ccmMat.at<float>(y, x) = ccm.at(y).at(x);
    }
  }

  return ccmMat;
}

pair<Vec4f, Vec4f> computeColorPatchErrors(
    const Mat& imBefore,
    const Mat& imAfter,
    const vector<ColorPatch>& colorPatches) {

  // Compute errors in [0..255]
  static const float kScale = 255.0f;
  imBefore *= kScale;
  imAfter *= kScale;

  Vec4f errBefore = Vec4f(0.0f, 0.0f, 0.0f, 0.0f);
  Vec4f errAfter = Vec4f(0.0f, 0.0f, 0.0f, 0.0f);
  const int numPatches =  colorPatches.size();

  for (int i = 0; i < numPatches; ++i) {
    Vec3f medianBefore = imBefore.at<Vec3f>(colorPatches[i].centroid);
    Vec3f medianAfter = imAfter.at<Vec3f>(colorPatches[i].centroid);
    Vec3f macbethPatchVal = Vec3i {
      rgbLinearMacbeth[i][0],
      rgbLinearMacbeth[i][1],
      rgbLinearMacbeth[i][2] };

    Vec3f diffBefore = colorPatches[i].rgbMedian  - macbethPatchVal;
    Vec3f diffAfter = medianAfter - macbethPatchVal;

    // RGB error
    errBefore[0] += norm(diffBefore, NORM_L2) / numPatches;
    errAfter[0] += norm(diffAfter, NORM_L2) / numPatches;

    // B, G, R errors
    static const int kErrorTypes = 4;
    for (int iErr = 1; iErr < kErrorTypes; ++iErr) {
      errBefore[iErr] += fabs(diffBefore[iErr - 1]) / numPatches;
      errAfter[iErr] += fabs(diffAfter[iErr - 1]) / numPatches;
    }
  }

  return make_pair(errBefore, errAfter);
}

} // namespace color_calibration
} // namespace surround360
