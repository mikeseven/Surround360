/**
* Copyright (c) 2016-present, Facebook, Inc.
* All rights reserved.
*
* This source code is licensed under the BSD-style license found in the
* LICENSE_render file in the root directory of this subproject. An additional grant
* of patent rights can be found in the PATENTS file in the same directory.
*/

#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "CvUtil.h"
#include "ImageWarper.h"
#include "StringUtil.h"
#include "SystemUtil.h"

#include <gflags/gflags.h>
#include <glog/logging.h>

using namespace std;
using namespace cv;
using namespace surround360::util;
using namespace surround360::warper;

DEFINE_string(test_dir, "", "path to dir with test files");

void testSideFisheyeProjection() {
  for(int i=1;i<=6;i++) {
    const string srcPath = FLAGS_test_dir + "/cam" +std::to_string(i)+".png";
    Mat srcImage = imreadExceptionOnFail(srcPath, -1);
    CameraMetadata camModel;
    /*camModel.isFisheye = true;
    camModel.usablePixelsRadius = 1298;
    camModel.imageCenterX = 1224;
    camModel.imageCenterY = 1024;
    Mat eqrImage = sideFisheyeToSpherical(srcImage, camModel, 2048, 2048);*/

	// VRCA 2688x2688
    camModel.isFisheye = true;
    camModel.fisheyeFovDegrees = 180;
    camModel.fovHorizontal = 180;
    //camModel.fisheyeFovDegreesCrop = 180;
    //camModel.fisheyeRotationDegrees = 0;
    camModel.aspectRatioWH = 1;
    camModel.usablePixelsRadius = 1356;//srcImage.cols/2;//1350;//1065;
    camModel.imageCenterX = 1377;//srcImage.cols/2;//1456;
    camModel.imageCenterY = 1335;//srcImage.rows/2;//1092;
	
	// Fujinon
    /* TODO camModel.isFisheye = true;
    camModel.fisheyeFovDegrees = 180;
    camModel.fovHorizontal = 180;
    camModel.fisheyeFovDegreesCrop = 180;
    camModel.fisheyeRotationDegrees = 0;
    camModel.aspectRatioWH = float(srcImage.cols)/float(srcImage.rows);
    camModel.usablePixelsRadius = 1220;//srcImage.cols/2;//1350;//1065;
    camModel.imageCenterX = 1377;//srcImage.cols/2;//1456;
    camModel.imageCenterY = 1335;//srcImage.rows/2;//1092;
	*/
    Mat eqrImage = sideFisheyeToSpherical(srcImage, camModel, 2048, 1024);//srcImage.cols, srcImage.rows);

    imwriteExceptionOnFail(FLAGS_test_dir + "/eqr"+std::to_string(i)+".png", eqrImage);
  }
}

int main(int argc, char** argv) {
  initSurround360(argc, argv);
  requireArg(FLAGS_test_dir, "test_dir");

  testSideFisheyeProjection();
  return EXIT_SUCCESS;
}
